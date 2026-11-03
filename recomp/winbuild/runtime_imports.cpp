// Calls from recompiled game code into the native libil2cpp runtime.
//
// Each handler reads arm64 arguments (x0-x7) from the Ctx, calls the native
// function and writes the result back to x0. The guest stack pointer is
// published first, so native code calling back into game code continues
// below the current guest frame.
//
// Addresses are runtime functions in the macOS v1.4.2 arm64 GameAssembly,
// identified as documented in docs/RECOMP_DESIGN.md: the out-of-line helpers
// of codegen/il2cpp-codegen-il2cpp.cpp (0x192c54-0x1931a0, in source order),
// plus the few libil2cpp internals and corlib icalls generated code calls
// directly.
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_set>
#include <cstring>

#include "codegen/il2cpp-codegen.h"
#include "il2cpp-pinvoke-support.h"
#include "mono/MonoPosixHelper.h"
#include "mono/ThreadPool/threadpool-ms.h"
#include "icalls/System/System/IOSelector.h"
#include "vm/Class.h"
#include "vm/Type.h"
#include "vm/InternalCalls.h"
#include "vm/ClassInlines.h"
#include "vm/ScopedThreadAttacher.h"
#include "gc/GarbageCollector.h"
#include "gc.h"

#include "native_adapter.h"
#include "il2cpp-api.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace recomp
{
    void RegisterIcallSignature(const char* icall, void* target);  // icall_marshal.cpp
    void* WrapInputIcall(const char* name, void* original);         // mouse_keys.cpp
    void* WrapDisplayIcall(const char* name, void* original);       // display.cpp
    extern void (*g_sampleStateHook)();                            // crash_report.cpp
}

namespace
{
    using recomp::Ctx;
    using namespace recomp::native;

    void WriteBarrier(Ctx& c)
    {
        Il2CppCodeGenWriteBarrier(Arg<void**>(c, 0), Arg<void*>(c, 1));
    }

    [[noreturn]] void NoReturn(Ctx&)
    {
        fputs("recomp: il2cpp_codegen_no_return reached\n", stderr);
        abort();
    }

    [[noreturn]] void Unsupported(Ctx&, const char* what)
    {
        fprintf(stderr, "recomp: %s is not supported on this platform\n", what);
        abort();
    }

    // Apple-only engine icalls (GameCenter, iOS/tvOS services) are missing
    // from the Windows player. They resolve to a no-op returning zero, which
    // is what the game sees on a device where the service is unavailable.
    uint64_t MissingIcall() { return 0; }

    Il2CppMethodPointer ResolveIcallOrStub(const char* name)
    {
        if (Il2CppMethodPointer p = il2cpp::vm::InternalCalls::Resolve(name))
        {
            p = reinterpret_cast<Il2CppMethodPointer>(recomp::WrapInputIcall(name, reinterpret_cast<void*>(p)));
            p = reinterpret_cast<Il2CppMethodPointer>(recomp::WrapDisplayIcall(name, reinterpret_cast<void*>(p)));
            recomp::RegisterIcallSignature(name, reinterpret_cast<void*>(p));
            return p;
        }
        fprintf(stderr, "GoSRecomp: engine function %s is not available on Windows, ignored\n", name);
        fflush(stderr);
        return reinterpret_cast<Il2CppMethodPointer>(&MissingIcall);
    }

    // Exception reports go to Player.log (Unity copies stderr there): on by
    // default so testers' logs say where things failed, capped at 300.
    // At most 3 reports per throw site (a site repeating every frame must not
    // hide the rare one that matters), 5000 in all.
    bool ShouldTrace(uint64_t site = 0)
    {
        static const bool off = getenv("GOSR_NO_TRACE") != nullptr;
        static std::atomic<int> count{0};
        static std::mutex lock;
        static std::unordered_map<uint64_t, int> perSite;
        if (off)
            return false;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (++perSite[site] > 3)
                return false;
        }
        return count.fetch_add(1, std::memory_order_relaxed) < 5000;
    }

    // Follows the guest frame-pointer chain (x29 -> {fp, lr}).
    void PrintGuestFrames(Ctx& c)
    {
        uint64_t fp = c.x[29];
        fputs("  guest frames:", stderr);
        for (int i = 0; i < 8 && fp && !(fp & 7); i++)
        {
            uint64_t lr = *reinterpret_cast<uint64_t*>(fp + 8);
            if (lr < recomp::g_imageBase || lr - recomp::g_imageBase >= recomp::g_imageSize)
                break;
            fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(lr - recomp::g_imageBase - 4));
            fp = *reinterpret_cast<uint64_t*>(fp);
        }
        fputc('\n', stderr);
    }

    void RaiseExceptionTraced(Ctx& c)
    {
        auto* ex = Arg<Il2CppException*>(c, 0);
        if (ex && ShouldTrace(c.x[30]))
        {
            Il2CppClass* k = reinterpret_cast<Il2CppObject*>(ex)->klass;
            std::string msg;
            if (ex->message)
                for (int32_t i = 0; i < ex->message->length && i < 160; i++)
                    msg += ex->message->chars[i] < 128 ? char(ex->message->chars[i]) : '?';
            fprintf(stderr, "recomp: %s.%s thrown at guest 0x%llx: %s\n", k->namespaze, k->name,
                    static_cast<unsigned long long>(c.x[30] - recomp::g_imageBase - 4), msg.c_str());
            PrintGuestFrames(c);
            fflush(stderr);
        }
        Enter(c);
        il2cpp_codegen_raise_exception(Arg<Exception_t*>(c, 0), Arg<MethodInfo*>(c, 1));
    }

    // Logs where game code raised each NullReferenceException (see ShouldTrace).
    void RaiseNullReference(Ctx& c)
    {
        if (ShouldTrace(c.x[30]))
        {
            fprintf(stderr, "recomp: NullReferenceException raised at guest 0x%llx\n",
                    static_cast<unsigned long long>(c.x[30] - recomp::g_imageBase - 4));
            PrintGuestFrames(c);
            fflush(stderr);
        }
        Enter(c);
        il2cpp_codegen_raise_null_reference_exception();
    }

    void ComUnsupported(Ctx& c) { Unsupported(c, "COM interop"); }

    void ClassInit(Ctx& c)
    {
        Enter(c);
        c.x[0] = il2cpp::vm::Class::Init(Arg<Il2CppClass*>(c, 0)) ? 1 : 0;
    }

    const VirtualInvokeData* InterfaceInvokeDataSlowPath(Il2CppObject* obj, const Il2CppClass* itf, uint64_t slot)
    {
        const VirtualInvokeData* data = &il2cpp::vm::ClassInlines::GetInterfaceInvokeDataFromVTableSlowPath(obj, itf, Il2CppMethodSlot(slot));
        HMODULE owner = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(data->methodPtr), &owner))
        {
            const Il2CppClass* k = obj->klass;
            fprintf(stderr, "recomp: interface slow path %s on %s slot %u -> vtable[%lld] methodPtr %p method %s (vtable_count %u, initialized %d)\n",
                    il2cpp_type_get_name(&itf->byval_arg), il2cpp_type_get_name(&k->byval_arg), unsigned(slot), static_cast<long long>(data - k->vtable),
                    reinterpret_cast<const void*>(data->methodPtr), data->method ? data->method->name : "-", k->vtable_count, int(k->initialized));
            for (uint16_t i = 0; i < k->interface_offsets_count; i++)
                fprintf(stderr, "   itf %s at %d\n", il2cpp_type_get_name(&k->interfaceOffsets[i].interfaceType->byval_arg), k->interfaceOffsets[i].offset);
        }
        return data;
    }

    // ScopedThreadAttacher lives in the guest frame of reverse P/Invoke wrappers.
    void ThreadAttacherCtor(Ctx& c)
    {
        Enter(c);
        new (Arg<void*>(c, 0)) il2cpp::vm::ScopedThreadAttacher();
    }

    void ThreadAttacherDtor(Ctx& c)
    {
        Enter(c);
        Arg<il2cpp::vm::ScopedThreadAttacher*>(c, 0)->~ScopedThreadAttacher();
    }

    // The guest's PInvokeArguments has a narrow module name; Windows wants UTF-16.
    struct GuestPInvokeArguments
    {
        const char* moduleName;
        size_t moduleNameLength;
        const char* entryPoint;
        size_t entryPointLength;
        Il2CppCallConvention callingConvention;
        Il2CppCharSet charSet;
        int parameterSize;
        bool isNoMangle;
    };

    void Resolve(Ctx& c)
    {
        Enter(c);
        auto* g = Arg<const GuestPInvokeArguments*>(c, 0);
        std::wstring module(g->moduleName, g->moduleName + g->moduleNameLength);
        PInvokeArguments args = {
            il2cpp::utils::StringView<Il2CppNativeChar>(module.c_str(), module.size()),
            il2cpp::utils::StringView<char>(g->entryPoint, g->entryPointLength),
            g->callingConvention, g->charSet, g->parameterSize, g->isNoMangle
        };
        c.x[0] = reinterpret_cast<uint64_t>(il2cpp_codegen_resolve(args));
    }

    void RaiseException(Exception_t* ex, MethodInfo* frame)
    {
        il2cpp_codegen_raise_exception(ex, frame);
    }

    Type_t* GetType3(const RuntimeMethod* m, String_t* name, const RuntimeMethod* caller)
    {
        return il2cpp_codegen_get_type(m, name, caller);
    }

    Type_t* GetType4(const RuntimeMethod* m, String_t* name, bool throwOnError, const RuntimeMethod* caller)
    {
        return il2cpp_codegen_get_type(m, name, throwOnError, caller);
    }

    Type_t* GetType5(const RuntimeMethod* m, String_t* name, bool throwOnError, bool ignoreCase, const RuntimeMethod* caller)
    {
        return il2cpp_codegen_get_type(m, name, throwOnError, ignoreCase, caller);
    }

    bool MethodIsInterfaceMethod(RuntimeMethod* m) { return il2cpp_codegen_method_is_interface_method(m); }
    bool ClassIsAssignableFrom(RuntimeClass* a, RuntimeClass* b) { return il2cpp_codegen_class_is_assignable_from(a, b); }
    char* MarshalString(String_t* s) { return il2cpp_codegen_marshal_string(s); }
    void MarshalStringFixed(String_t* s, char* buffer, int n) { il2cpp_codegen_marshal_string_fixed(s, buffer, n); }
    void MarshalWStringFixed(String_t* s, Il2CppChar* buffer, int n) { il2cpp_codegen_marshal_wstring_fixed(s, buffer, n); }
    // the macOS build never marshals BSTRs: this slot is the char* result of string-returning P/Invokes
    String_t* MarshalStringResult(const char* value) { return il2cpp_codegen_marshal_string_result(value); }
    String_t* MarshalWStringResult(const Il2CppChar* v) { return il2cpp_codegen_marshal_wstring_result(v); }
    Il2CppChar* MarshalEmptyWStringBuilder(StringBuilder_t* sb) { return il2cpp_codegen_marshal_empty_wstring_builder(sb); }
    Il2CppChar* MarshalWStringBuilder(StringBuilder_t* sb) { return il2cpp_codegen_marshal_wstring_builder(sb); }
    void MarshalWStringBuilderResult(StringBuilder_t* sb, Il2CppChar* buffer) { il2cpp_codegen_marshal_wstring_builder_result(sb, buffer); }
    void MarshalFree(void* p) { il2cpp_codegen_marshal_free(p); }
    Il2CppMethodPointer MarshalDelegate(MulticastDelegate_t* d) { return il2cpp_codegen_marshal_delegate(d); }
}

namespace recomp
{
    void RegisterGuestStackRoot(void* base, size_t size)
    {
        il2cpp::gc::GarbageCollector::RegisterRoot(static_cast<char*>(base), size);
        static const bool log = getenv("GOSR_GC_LOG") && *getenv("GOSR_GC_LOG");
        static bool once = [] {
            if (const char* v = getenv("GOSR_NO_GC"); v && *v)  // diagnostics: rules the collector in or out
                il2cpp::gc::GarbageCollector::Disable();
            if (log)
                GC_set_on_collection_event([](GC_EventType e) {
                    if (e == GC_EVENT_START || e == GC_EVENT_END)
                        fprintf(stderr, "gc: %s #%u on thread %lu\n", e == GC_EVENT_START ? "start" : "end",
                                unsigned(GC_get_gc_no()), GetCurrentThreadId());
                });
            return true;
        }();
        if (log)
            fprintf(stderr, "gc: guest stack %p+%zx registered on thread %lu\n", base, size, GetCurrentThreadId());
        (void)once;
    }

    void DescribeBadCall(Ctx& c, uint64_t target)
    {
        fprintf(stderr, "recomp: call to non-code address %p from guest lr 0x%llx\n", reinterpret_cast<void*>(target),
                static_cast<unsigned long long>(c.x[30] - g_imageBase));
        auto* obj = reinterpret_cast<Il2CppObject*>(c.x[0]);
        if (obj && obj->klass)
        {
            Il2CppClass* k = obj->klass;
            fprintf(stderr, "  x0 is a %s.%s (vtable %u, interfaces %u)\n", k->namespaze, k->name, k->vtable_count, k->interface_offsets_count);
            for (uint16_t i = 0; i < k->interface_offsets_count; i++)
                fprintf(stderr, "   itf %s.%s at %d\n", k->interfaceOffsets[i].interfaceType->namespaze, k->interfaceOffsets[i].interfaceType->name, k->interfaceOffsets[i].offset);
            for (int r = 1; r <= 3; r++)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                auto* maybe = reinterpret_cast<Il2CppClass*>(c.x[r]);
                if (c.x[r] && VirtualQuery(maybe, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && maybe->klass == maybe)
                    fprintf(stderr, "  x%d is class %s.%s\n", r, maybe->namespaze, maybe->name);
                else
                    fprintf(stderr, "  x%d = 0x%llx\n", r, static_cast<unsigned long long>(c.x[r]));
            }
            fprintf(stderr, "  vtable entries:\n");
            for (uint16_t i = 0; i < k->vtable_count && i < 24; i++)
            {
                const VirtualInvokeData& v = k->vtable[i];
                fprintf(stderr, "   [%2u] %p %s\n", i, reinterpret_cast<void*>(v.methodPtr), v.method ? v.method->name : "?");
            }
        }
        fflush(stderr);
    }

    void RegisterLibcImports();
    void RegisterIcallImports();

    // Engine icalls and the IL2CPP runtime raise managed exceptions natively
    // (C++ throw of an Il2CppExceptionWrapper), bypassing the codegen thunks.
    // A first-chance handler reports those too, with the native stack, which
    // names the recompiled function (f_<guest address>) via the map file.
    bool IsIl2CppThrow(const EXCEPTION_RECORD* record)
    {
        constexpr DWORD kMsvcException = 0xE06D7363;
        if (record->ExceptionCode != kMsvcException || record->NumberParameters < 4)
            return false;
        auto base = static_cast<uintptr_t>(record->ExceptionInformation[3]);
        auto throwInfo = reinterpret_cast<const int32_t*>(record->ExceptionInformation[2]);
        if (!base || !throwInfo)
            return false;
        auto types = reinterpret_cast<const int32_t*>(base + throwInfo[3]);
        for (int32_t i = 0; i < types[0] && i < 8; i++)
        {
            auto catchable = reinterpret_cast<const int32_t*>(base + types[1 + i]);
            auto name = reinterpret_cast<const char*>(base + catchable[1] + 16);  // TypeDescriptor::name
            if (strstr(name, "Il2CppExceptionWrapper"))
                return true;
        }
        return false;
    }

    LONG CALLBACK OnNativeThrow(EXCEPTION_POINTERS* info)
    {
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        if (!IsIl2CppThrow(record))
            return EXCEPTION_CONTINUE_SEARCH;
        void* frames[24];
        USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        uint64_t site = 0;
        for (USHORT i = 0; i < n && i < 10; i++)
            site = site * 1000003 ^ reinterpret_cast<uint64_t>(frames[i]);
        if (!ShouldTrace(site))
            return EXCEPTION_CONTINUE_SEARCH;
        auto* wrapper = reinterpret_cast<Il2CppExceptionWrapper*>(record->ExceptionInformation[1]);
        Il2CppException* ex = wrapper ? wrapper->ex : nullptr;
        if (!ex)
            return EXCEPTION_CONTINUE_SEARCH;
        Il2CppClass* k = reinterpret_cast<Il2CppObject*>(ex)->klass;
        std::string msg;
        if (ex->message)
            for (int32_t i = 0; i < ex->message->length && i < 160; i++)
                msg += ex->message->chars[i] < 128 ? char(ex->message->chars[i]) : '?';
        fprintf(stderr, "recomp: %s.%s raised natively: %s\n", k->namespaze, k->name, msg.c_str());
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&OnNativeThrow), &self);
        fputs("  native frames:", stderr);
        for (USHORT i = 0; i < n; i++)
        {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(frames[i]), &m);
            uintptr_t off = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(m);
            if (m == self)
                fprintf(stderr, " GA+0x%llx", static_cast<unsigned long long>(off));
            else if (m)
            {
                wchar_t name[MAX_PATH];
                DWORD len = GetModuleFileNameW(m, name, MAX_PATH);
                const wchar_t* b = name + len;
                while (b > name && b[-1] != L'\\' && b[-1] != L'/')
                    b--;
                fprintf(stderr, " %ls+0x%llx", b, static_cast<unsigned long long>(off));
            }
        }
        fputc('\n', stderr);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // GOSR_LAYOUT_CHECK=<file>: compares the runtime's field offsets with the
    // ones the game was compiled with (lines "namespace\tclass\tfield\toffset",
    // offsets as Il2CppDumper prints them: value types without the object header).
    Il2CppClass* FindClassForCheck(const std::string& ns, const std::string& name)
    {
        std::vector<std::string> parts;
        for (size_t i = 0; i <= name.size();)
        {
            size_t dot = name.find('.', i);
            parts.push_back(name.substr(i, dot == std::string::npos ? std::string::npos : dot - i));
            if (dot == std::string::npos)
                break;
            i = dot + 1;
        }
        size_t count = 0;
        const Il2CppAssembly** asms = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
        for (size_t a = 0; a < count; a++)
        {
            Il2CppClass* k = il2cpp_class_from_name(il2cpp_assembly_get_image(asms[a]), ns.c_str(), parts[0].c_str());
            for (size_t n = 1; k && n < parts.size(); n++)
            {
                void* iter = nullptr;
                Il2CppClass* nested = nullptr;
                while ((nested = il2cpp_class_get_nested_types(k, &iter)))
                    if (parts[n] == il2cpp_class_get_name(nested))
                        break;
                k = nested;
            }
            if (k)
                return k;
        }
        return nullptr;
    }

    void CheckLayouts(const char* path)
    {
        FILE* f = fopen(path, "r");
        if (!f)
            return;
        char line[1024];
        int checked = 0, mismatched = 0, missing = 0;
        std::string lastKey;
        Il2CppClass* k = nullptr;
        while (fgets(line, sizeof line, f))
        {
            char* fields[4] = {};
            char* p = line;
            for (int i = 0; i < 4 && p; i++)
            {
                fields[i] = p;
                p = strchr(p, '\t');
                if (p)
                    *p++ = 0;
            }
            if (!fields[3])
                continue;
            std::string key = std::string(fields[0]) + "|" + fields[1];
            if (key != lastKey)
            {
                lastKey = key;
                k = FindClassForCheck(fields[0], fields[1]);
            }
            if (!k)
            {
                missing++;
                continue;
            }
            FieldInfo* fi = il2cpp_class_get_field_from_name(k, fields[2]);
            if (!fi)
            {
                missing++;
                continue;
            }
            size_t want = strtoul(fields[3], nullptr, 10);
            size_t got = il2cpp_field_get_offset(fi) - (il2cpp_class_is_valuetype(k) ? sizeof(Il2CppObject) : 0);
            checked++;
            if (got != want && mismatched++ < 200)
                fprintf(stderr, "layout: %s.%s::%s compiled at %zu, runtime %zu\n", fields[0], fields[1], fields[2], want, got);
        }
        fclose(f);
        fprintf(stderr, "layout: %d fields checked, %d differ, %d not found\n", checked, mismatched, missing);
    }

    // GOSR_STATICS_CHECK: walks every object reachable from static fields the
    // way the engine's liveness pass (UnloadUnusedAssets) does, and reports the
    // field holding a reference that is not an object.
    uint64_t SafeRead(uint64_t address, bool& ok)
    {
        __try
        {
            ok = true;
            return *reinterpret_cast<const uint64_t*>(address);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
            return 0;
        }
    }

    struct HeapWalk
    {
        std::unordered_set<uint64_t> seen;
        std::vector<uint64_t> pending;
        std::unordered_set<std::string> reported;
        int bad = 0;
        uint64_t current = 0;

        bool IsObject(uint64_t v)
        {
            if (v < 0x10000 || (v & 7))
                return false;
            bool ok;
            uint64_t k = SafeRead(v, ok);
            if (!ok || k < 0x10000 || (k & 7))
                return false;
            uint64_t kk = SafeRead(k + offsetof(Il2CppClass, klass), ok);
            return ok && kk == k;
        }

        void Ref(uint64_t v, Il2CppClass* owner, const char* field, const char* via)
        {
            if (!v)
                return;
            if (!IsObject(v))
            {
                std::string key = std::string(owner->namespaze) + "." + owner->name + "::" + field;
                if (reported.insert(key).second && bad++ < 50)
                {
                    fprintf(stderr, "statics: %s holds 0x%llx, not an object (%s)\n", key.c_str(), static_cast<unsigned long long>(v), via);
                    if (current)
                    {
                        Il2CppClass* ck = *reinterpret_cast<Il2CppClass**>(current);
                        fprintf(stderr, "  in a %s.%s (instance size 0x%x):", ck->namespaze, il2cpp_type_get_name(&ck->byval_arg), ck->instance_size);
                        for (uint32_t i = 0; i < ck->instance_size / 8 && i < 64; i++)
                            fprintf(stderr, "%s%llx", i % 8 ? " " : "\n    ", static_cast<unsigned long long>(reinterpret_cast<uint64_t*>(current)[i]));
                        fputc('\n', stderr);
                        for (Il2CppClass* k = ck; k; k = k->parent)
                        {
                            void* it = nullptr;
                            while (const FieldInfo* f = il2cpp_class_get_fields(k, &it))
                                if (!(f->type->attrs & FIELD_ATTRIBUTE_STATIC))
                                    fprintf(stderr, "    %s::%s @0x%x\n", k->name, f->name, f->offset);
                            if (k->parent && !strcmp(k->parent->name, "MonoBehaviour"))
                                break;
                        }
                    }
                }
                return;
            }
            if (seen.insert(v).second)
                pending.push_back(v);
        }

        // fields of an object or a struct; base points at field offset 0 minus the header for structs
        void Fields(char* base, Il2CppClass* klass, bool isStruct, const char* via, int depth = 0)
        {
            for (Il2CppClass* k = klass; k && depth < 8; k = isStruct ? nullptr : k->parent)
            {
                void* it = nullptr;
                while (FieldInfo* f = const_cast<FieldInfo*>(il2cpp_class_get_fields(k, &it)))
                {
                    if (f->type->attrs & (FIELD_ATTRIBUTE_STATIC | FIELD_ATTRIBUTE_LITERAL))
                        continue;
                    char* at = base + f->offset - (isStruct ? sizeof(Il2CppObject) : 0);
                    Field(at, f, k, via, depth);
                }
            }
        }

        void Field(char* at, FieldInfo* f, Il2CppClass* owner, const char* via, int depth)
        {
            if (f->type->byref || f->type->type == IL2CPP_TYPE_PTR || f->type->type == IL2CPP_TYPE_FNPTR)
                return;
            Il2CppClass* fk = il2cpp_class_from_type(f->type);
            if (!fk)
                return;
            if (il2cpp_class_is_valuetype(fk))
            {
                if (!il2cpp_class_is_enum(fk) && fk->has_references)
                    Fields(at, fk, true, via, depth + 1);
                return;
            }
            bool ok;
            uint64_t v = SafeRead(reinterpret_cast<uint64_t>(at), ok);
            if (ok)
                Ref(v, owner, f->name, via);
        }

        void Object(uint64_t o)
        {
            Il2CppClass* k = *reinterpret_cast<Il2CppClass**>(o);
            if (k->rank)
            {
                Il2CppClass* e = k->element_class;
                uint64_t n = *reinterpret_cast<uint64_t*>(o + 0x18);
                char* data = reinterpret_cast<char*>(o + 0x20);
                if (!il2cpp_class_is_valuetype(e))
                {
                    for (uint64_t i = 0; i < n; i++)
                        Ref(reinterpret_cast<uint64_t*>(data)[i], k, "[]", "array element");
                }
                else if (!il2cpp_class_is_enum(e) && e->has_references)
                {
                    uint32_t size = il2cpp_class_array_element_size(e);
                    for (uint64_t i = 0; i < n; i++)
                        Fields(data + i * size, e, true, "struct array element");
                }
                return;
            }
            if (k->has_references)
                Fields(reinterpret_cast<char*>(o), k, false, "instance field");
        }
    };

    void CheckStatics()
    {
        HeapWalk w;
        const auto& classes = il2cpp::vm::Class::GetStaticFieldData();
        for (Il2CppClass* klass : classes)
        {
            if (!klass || !klass->size_inited || !klass->static_fields)
                continue;
            void* it = nullptr;
            while (FieldInfo* field = const_cast<FieldInfo*>(il2cpp_class_get_fields(klass, &it)))
            {
                if (!(field->type->attrs & FIELD_ATTRIBUTE_STATIC) || (field->type->attrs & FIELD_ATTRIBUTE_LITERAL) ||
                    field->offset == THREAD_STATIC_FIELD_OFFSET)
                    continue;
                w.Field(static_cast<char*>(klass->static_fields) + field->offset, field, klass, "static field", 0);
            }
        }
        size_t visited = 0;
        while (!w.pending.empty() && visited < 20000000)
        {
            uint64_t o = w.pending.back();
            w.pending.pop_back();
            w.current = o;
            visited++;
            __try
            {
                w.Object(o);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                fprintf(stderr, "statics: fault walking object %p\n", reinterpret_cast<void*>(o));
            }
        }
        static int runs = 0;
        if (runs++ < 3 || w.bad)
            fprintf(stderr, "statics: %zu objects reachable, %d bad references\n", visited, w.bad);
    }

    // Engine timing state for the sampler: tells "frames advance but nothing
    // renders" apart from a stalled frame. Reads engine globals only.
    void PrintEngineState()
    {
        using IntGetter = int32_t (*)();
        using FloatGetter = float (*)();
        static IntGetter frame = reinterpret_cast<IntGetter>(il2cpp::vm::InternalCalls::Resolve("UnityEngine.Time::get_frameCount()"));
        static IntGetter rendered = reinterpret_cast<IntGetter>(il2cpp::vm::InternalCalls::Resolve("UnityEngine.Time::get_renderedFrameCount()"));
        static FloatGetter scale = reinterpret_cast<FloatGetter>(il2cpp::vm::InternalCalls::Resolve("UnityEngine.Time::get_timeScale()"));
        static FloatGetter fixedDelta = reinterpret_cast<FloatGetter>(il2cpp::vm::InternalCalls::Resolve("UnityEngine.Time::get_fixedDeltaTime()"));
        static FloatGetter maxDelta = reinterpret_cast<FloatGetter>(il2cpp::vm::InternalCalls::Resolve("UnityEngine.Time::get_maximumDeltaTime()"));
        static FloatGetter time = reinterpret_cast<FloatGetter>(il2cpp::vm::InternalCalls::Resolve("UnityEngine.Time::get_realtimeSinceStartup()"));
        static IntGetter target = reinterpret_cast<IntGetter>(il2cpp::vm::InternalCalls::Resolve("UnityEngine.Application::get_targetFrameRate()"));
        if (!frame || !rendered)
            return;
        // TimeManager fields read by StepFixedTime (UnityPlayer 2020.3.30f1 x64:
        // GetTimeManager at RVA 0x5429f0)
        if (!frame || !rendered || frame() < 1)
            return;  // engine managers do not exist before the first frame
        static bool layoutChecked = false;
        if (!layoutChecked)
        {
            layoutChecked = true;
            if (const char* path = getenv("GOSR_LAYOUT_CHECK"))
                CheckLayouts(path);
        }
        static const bool staticsCheck = getenv("GOSR_STATICS_CHECK") != nullptr;
        if (staticsCheck)
            CheckStatics();
        // which effects plugin the game loaded (a stale stub keeps battles on the loading screen)
        static bool pluginReported = false;
        if (!pluginReported)
            if (HMODULE spfx = GetModuleHandleW(L"SPFXUnityPlugin.dll"))
            {
                pluginReported = true;
                wchar_t path[MAX_PATH] = L"?";
                GetModuleFileNameW(spfx, path, MAX_PATH);
                using VersionFn = const char* (*)();
                auto version = reinterpret_cast<VersionFn>(GetProcAddress(spfx, "SPFX_PLUGIN_GetVersionString"));
                fprintf(stderr, "spfx: %ls: %s\n", path, version ? version() : "?");
            }
        if (HMODULE player = GetModuleHandleW(L"UnityPlayer.dll"))
        {
            using GetManager = uint8_t* (*)();
            uint8_t* tm = reinterpret_cast<GetManager>(reinterpret_cast<uint8_t*>(player) + 0x5429f0)();
            if (tm)
            {
                double fixedCur, dynCur, d40, d70;
                float fixedDelta2, fc;
                memcpy(&fixedCur, tm + 0x30, 8);
                memcpy(&d40, tm + 0x40, 8);
                memcpy(&fixedDelta2, tm + 0x48, 4);
                memcpy(&dynCur, tm + 0x60, 8);
                memcpy(&d70, tm + 0x70, 8);
                memcpy(&fc, tm + 0xfc, 4);
                double e0, e8;
                float d8;
                memcpy(&e0, tm + 0xe0, 8);
                memcpy(&e8, tm + 0xe8, 8);
                memcpy(&d8, tm + 0xd8, 4);
                fprintf(stderr, "timemanager: fixed %.6g +%g dynamic %.6g [0x40] %.6g [0x70] %.6g flag %d [0xfc] %g [0xd8] %g [0xe0] %.6g [0xe8] %.6g\n",
                        fixedCur, fixedDelta2, dynCur, d40, d70, tm[0xc2], fc, d8, e0, e8);
            }
        }
        static Il2CppClass* onDemand = [] {
            size_t n = 0;
            const Il2CppAssembly** asms = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
            for (size_t i = 0; i < n; i++)
                if (Il2CppClass* k = il2cpp_class_from_name(il2cpp_assembly_get_image(asms[i]), "UnityEngine.Rendering", "OnDemandRendering"))
                    return k;
            return static_cast<Il2CppClass*>(nullptr);
        }();
        if (onDemand && onDemand->static_fields)
            fprintf(stderr, "ondemand: m_RenderFrameInterval %d (cctor %s)\n", *static_cast<int32_t*>(onDemand->static_fields),
                    onDemand->cctor_finished ? "done" : "not run");
        fprintf(stderr, "state: frame %d rendered %d timeScale %g fixedDelta %g maxDelta %g realtime %g targetFps %d\n",
                frame(), rendered(), scale ? scale() : -1.0, fixedDelta ? fixedDelta() : -1.0, maxDelta ? maxDelta() : -1.0,
                time ? time() : -1.0, target ? target() : -1);
    }

    void RegisterRuntimeImports()
    {
        g_sampleStateHook = &PrintEngineState;
        if (!getenv("GOSR_NO_TRACE"))
            AddVectoredExceptionHandler(1, OnNativeThrow);
        // codegen/il2cpp-codegen-common.cpp
        RegisterImport(0x192c54, NoReturn);
        RegisterImport(0x192c58, WriteBarrier);
        // codegen/il2cpp-codegen-il2cpp.cpp
        RegisterImport(0x192c5c, N<il2cpp_codegen_marshal_store_last_error>);
        RegisterImport(0x192c60, N<il2cpp_codegen_delegate_begin_invoke>);
        RegisterImport(0x192c64, N<il2cpp_codegen_delegate_end_invoke>);
        RegisterImport(0x192c68, N<ResolveIcallOrStub>);
        RegisterImport(0x192c9c, N<il2cpp_codegen_type_get_object>);
        RegisterImport(0x192ca0, N<il2cpp_codegen_get_executing_assembly>);
        RegisterImport(0x192cb4, N<il2cpp_codegen_initialize_runtime_metadata>);
        RegisterImport(0x192cb8, N<il2cpp_codegen_initialize_runtime_metadata_inline>);
        RegisterImport(0x192cbc, N<il2cpp_codegen_get_thread_static_data>);
        RegisterImport(0x192cc4, N<il2cpp_codegen_memory_barrier>);
        RegisterImport(0x192cc8, N<SZArrayNew>);
        RegisterImport(0x192cd0, N<GenArrayNew>);
        RegisterImport(0x192cd8, N<il2cpp_codegen_method_is_generic_instance>);
        RegisterImport(0x192cdc, N<il2cpp_codegen_method_get_declaring_type>);
        RegisterImport(0x192ce0, N<MethodIsStatic>);
        RegisterImport(0x192cf8, N<il2cpp_codegen_raise_profile_exception>);
        RegisterImport(0x192d50, RaiseExceptionTraced);
        RegisterImport(0x192d5c, N<il2cpp_codegen_get_generic_virtual_method_internal>);
        RegisterImport(0x192d60, N<il2cpp_codegen_runtime_class_init>);
        RegisterImport(0x192d64, N<il2cpp_codegen_raise_execution_engine_exception>);
        RegisterImport(0x192d68, N<il2cpp_codegen_raise_execution_engine_exception_if_method_is_not_found>);
        RegisterImport(0x192d6c, N<IsInst>);
        RegisterImport(0x192d70, N<Box>);
        RegisterImport(0x192d74, N<Unbox_internal>);
        RegisterImport(0x192d78, N<UnBoxNullable_internal>);
        RegisterImport(0x192d7c, N<il2cpp_codegen_object_new>);
        RegisterImport(0x192d80, N<static_cast<void* (*)(size_t)>(il2cpp_codegen_marshal_allocate)>);
        RegisterImport(0x192d84, RaiseNullReference);
        RegisterImport(0x192d90, N<il2cpp_codegen_get_argument_null_exception>);
        RegisterImport(0x192d94, N<il2cpp_codegen_get_overflow_exception>);
        RegisterImport(0x192da0, N<il2cpp_codegen_get_array_type_mismatch_exception>);
        RegisterImport(0x192da4, N<il2cpp_codegen_get_marshal_directive_exception>);
        RegisterImport(0x192da8, N<il2cpp_codegen_get_missing_method_exception>);
        RegisterImport(0x192dac, N<il2cpp_codegen_get_index_out_of_range_exception>);
        for (uint64_t com : { 0x192db0, 0x192db4, 0x192db8, 0x192dd8, 0x192ddc, 0x192de0, 0x192e7c, 0x192eb8 })
            RegisterImport(com, ComUnsupported);
        RegisterImport(0x192ebc, N<il2cpp_codegen_string_new_wrapper>);
        RegisterImport(0x192ec0, N<GetType3>);
        RegisterImport(0x192f70, N<GetType4>);
        RegisterImport(0x19302c, N<GetType5>);
        RegisterImport(0x1930f4, N<RaiseInvalidCastException>);
        RegisterImport(0x193154, N<MethodIsInterfaceMethod>);
        RegisterImport(0x193168, N<ClassIsAssignableFrom>);
        RegisterImport(0x19316c, N<il2cpp_codegen_class_from_type_internal>);
        RegisterImport(0x193174, N<MarshalString>);
        RegisterImport(0x193178, N<MarshalStringFixed>);
        RegisterImport(0x19317c, N<MarshalWStringFixed>);
        RegisterImport(0x193180, N<MarshalStringResult>);
        RegisterImport(0x193184, N<MarshalWStringResult>);
        RegisterImport(0x193188, N<MarshalEmptyWStringBuilder>);
        RegisterImport(0x19318c, N<MarshalWStringBuilder>);
        RegisterImport(0x193190, N<MarshalWStringBuilderResult>);
        RegisterImport(0x193194, N<MarshalFree>);
        RegisterImport(0x193198, N<MarshalDelegate>);
        RegisterImport(0x19319c, N<il2cpp_codegen_marshal_function_ptr_to_delegate_internal>);
        RegisterImport(0x1931a0, Resolve);

        // libil2cpp internals called directly by generated code
        RegisterImport(0x12fd8c, ClassInit);
        RegisterImport(0x130030, N<InterfaceInvokeDataSlowPath>);
        RegisterImport(0x146764, ThreadAttacherCtor);
        RegisterImport(0x1467fc, ThreadAttacherDtor);
        RegisterImport(0x18f3e4, WriteBarrier);  // GarbageCollector::SetWriteBarrier

        // corlib icalls and MonoPosixHelper exports bound at build time
        RegisterImport(0x189294, N<CreateZStream>);
        RegisterImport(0x1893b0, N<CloseZStream>);
        RegisterImport(0x1894a4, N<Flush>);
        RegisterImport(0x189530, N<ReadZStream>);
        RegisterImport(0x189608, N<WriteZStream>);
        RegisterImport(0x18ac84, N<il2cpp::icalls::System::System::IOSelector::Add>);
        RegisterImport(0x18b9d8, N<ves_icall_System_Threading_ThreadPool_InitializeVMTp>);
        RegisterImport(0x18b9e4, N<ves_icall_System_Threading_ThreadPool_NotifyWorkItemComplete>);
        RegisterImport(0x18c428, N<ves_icall_System_Threading_ThreadPool_NotifyWorkItemProgressNative>);
        RegisterImport(0x18c49c, N<ves_icall_System_Threading_ThreadPool_ReportThreadStatus>);
        RegisterImport(0x18c4a0, N<ves_icall_System_Threading_ThreadPool_RequestWorkerThread>);
        RegisterImport(0x18c834, N<ves_icall_System_Threading_ThreadPool_IsThreadPoolHosted>);

        RegisterLibcImports();
        RegisterIcallImports();
    }
}

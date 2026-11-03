// The game's server, running inside the game process.
//
// bsSession.SendRequest is a coroutine; its iterator's MoveNext
// (bsSession.<SendRequest>d__22, 0x182B004 in the macOS v1.4.2 image) is
// replaced by LocalServerMoveNext through the recompiler's --override. It
// decodes the request's MessagePack parameters, lets a handler produce the
// response body, and calls the request's RecvCallback the way the HTTP path
// does after ParseTopHeader: (www = null, version, body bytes, bodyTop = 0,
// header). The body is the MessagePack map of the protocol's BODY class,
// keyed by member name (see docs/server/OFFLINE.md).
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include "il2cpp-api.h"
#include "il2cpp-object-internals.h"
#include "vm/Array.h"

#include "local_server.h"
#include "msgpack.h"
#include "recomp_bridge.h"
#include "recomp_ctx.h"

namespace gosr
{
    namespace
    {
        // bsSession.<SendRequest>d__22 field offsets
        constexpr size_t kState = 0x10;
        constexpr size_t kPhpUrl = 0x20;
        constexpr size_t kIsSSKeyRequest = 0x38;
        constexpr size_t kJsonForm = 0x48;
        constexpr size_t kRecvCallback = 0x50;
        // bsJsonForm.m_request_params
        constexpr size_t kRequestParams = 0x20;
        // RecvCallback's version argument (the response format version)
        constexpr int kResponseVersion = 1;

        template <typename T>
        T& Field(Il2CppObject* obj, size_t offset)
        {
            return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + offset);
        }

        std::string ToUtf8(Il2CppString* s)
        {
            if (!s)
                return {};
            std::string out;
            const Il2CppChar* p = il2cpp_string_chars(s);
            int32_t n = il2cpp_string_length(s);
            for (int32_t i = 0; i < n; i++)
            {
                uint32_t c = p[i];
                if (c >= 0xD800 && c < 0xDC00 && i + 1 < n)
                    c = 0x10000 + ((c - 0xD800) << 10) + (p[++i] - 0xDC00);
                if (c < 0x80) out += char(c);
                else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
                else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
                else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
            }
            return out;
        }

        FILE* Log()
        {
            static FILE* f = [] {
                FILE* file = fopen("gosr_server.log", "a");
                return file ? file : stderr;
            }();
            return f;
        }

        std::string EndpointName(const std::string& url)
        {
            size_t slash = url.find_last_of('/');
            std::string name = slash == std::string::npos ? url : url.substr(slash + 1);
            size_t dot = name.rfind(".php");
            return dot == std::string::npos ? name : name.substr(0, dot);
        }

        void SetField(Il2CppObject* obj, const char* name, void* value)
        {
            if (FieldInfo* f = il2cpp_class_get_field_from_name(il2cpp_object_get_class(obj), name))
                il2cpp_field_set_value(obj, f, value);
        }

        Il2CppObject* MakeHeader(const MethodInfo* invoke, int result)
        {
            const Il2CppType* t = il2cpp_method_get_param(invoke, 4);
            Il2CppClass* klass = il2cpp_class_from_type(t);
            Il2CppObject* header = il2cpp_object_new(klass);
            il2cpp_runtime_object_init(header);
            SetField(header, "<result>k__BackingField", &result);
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            SetField(header, "<unix_time>k__BackingField", &now);
            Il2CppString* sess = il2cpp_string_new("offline");
            SetField(header, "<sess_id>k__BackingField", sess);
            return header;
        }

        std::unordered_map<std::string, Handler>& Handlers()
        {
            static std::unordered_map<std::string, Handler> handlers;
            return handlers;
        }
    }

    void RegisterHandler(const std::string& endpoint, Handler handler)
    {
        Handlers()[endpoint] = std::move(handler);
    }

    // Runs one request; returns the body to send back.
    msgpack::Value HandleRequest(const std::string& endpoint, const msgpack::Value& request)
    {
        auto it = Handlers().find(endpoint);
        if (it != Handlers().end())
            return it->second(request);
        fprintf(Log(), "[server] %s: no handler, empty body; request %s\n", endpoint.c_str(), request.Dump().substr(0, 400).c_str());
        fflush(Log());
        return msgpack::Value::Map();
    }
}

namespace recomp
{
    // bool bsSession.<SendRequest>d__22.MoveNext()
    void LocalServerMoveNext(Ctx& c)
    {
        using namespace gosr;
        SetGuestStackPointer(c.sp);
        auto* it = reinterpret_cast<Il2CppObject*>(c.x[0]);
        if (Field<int32_t>(it, kState) != 0)
        {
            c.x[0] = 0;
            return;
        }
        Field<int32_t>(it, kState) = -1;

        std::string url = ToUtf8(Field<Il2CppString*>(it, kPhpUrl));
        std::string endpoint = EndpointName(url);
        msgpack::Value request = msgpack::Value::Map();
        if (auto* form = Field<Il2CppObject*>(it, kJsonForm))
        {
            if (auto* params = Field<Il2CppArray*>(form, kRequestParams))
            {
                auto* data = reinterpret_cast<const uint8_t*>(il2cpp::vm::Array::GetFirstElementAddress(params));
                if (!msgpack::Decode(data, il2cpp_array_length(params), request))
                    fprintf(Log(), "[server] %s: undecodable request (%u bytes)\n", endpoint.c_str(), il2cpp_array_length(params));
            }
        }
        fprintf(Log(), "[server] %s%s\n", endpoint.c_str(), Field<bool>(it, kIsSSKeyRequest) ? " (ss key)" : "");
        fflush(Log());

        std::vector<uint8_t> body = msgpack::Encode(HandleRequest(endpoint, request));

        auto* callback = Field<Il2CppObject*>(it, kRecvCallback);
        if (!callback)
        {
            c.x[0] = 0;
            return;
        }
        const MethodInfo* invoke = il2cpp_class_get_method_from_name(il2cpp_object_get_class(callback), "Invoke", 5);
        Il2CppArray* bytes = il2cpp_array_new(il2cpp_defaults.byte_class, body.size());
        memcpy(il2cpp::vm::Array::GetFirstElementAddress(bytes), body.data(), body.size());
        int32_t version = kResponseVersion, bodyTop = 0;
        Il2CppObject* header = MakeHeader(invoke, 0);
        void* args[5] = { nullptr, &version, bytes, &bodyTop, header };
        Il2CppException* exc = nullptr;
        il2cpp_runtime_invoke(invoke, callback, args, &exc);
        if (exc)
        {
            fprintf(Log(), "[server] %s: RecvCallback threw\n", endpoint.c_str());
            fflush(Log());
        }
        c.x[0] = 0;  // the coroutine ends
    }
}

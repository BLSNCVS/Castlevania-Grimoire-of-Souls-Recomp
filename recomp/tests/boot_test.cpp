// Boots the IL2CPP runtime from GameAssembly.dll the way UnityPlayer.dll does,
// then lists the game's assemblies and looks up a few game classes.
//
// usage: boot_test <GameAssembly.dll> <game Data folder>
#include <cstdio>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("usage: %s <GameAssembly.dll> <game Data folder>\n", argv[0]);
        return 1;
    }

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll)
    {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    auto get = [&](const char* name)
    {
        FARPROC p = GetProcAddress(dll, name);
        if (!p)
            printf("missing export %s\n", name);
        return p;
    };

    using SetDirFn = void (*)(const char*);
    using InitFn = int (*)(const char*);
    using DomainGetFn = void* (*)();
    using GetAssembliesFn = void** (*)(void*, size_t*);
    using AssemblyImageFn = void* (*)(void*);
    using ImageNameFn = const char* (*)(void*);
    using ImageClassCountFn = size_t (*)(void*);
    using ClassFromNameFn = void* (*)(void*, const char*, const char*);

    std::string data = argv[2];
    reinterpret_cast<SetDirFn>(get("il2cpp_set_data_dir"))((data + "/il2cpp_data").c_str());
    reinterpret_cast<SetDirFn>(get("il2cpp_set_config_dir"))((data + "/il2cpp_data/etc").c_str());

    printf("calling il2cpp_init...\n");
    fflush(stdout);
    int ok = reinterpret_cast<InitFn>(get("il2cpp_init"))("IL2CPP Root Domain");
    printf("il2cpp_init returned %d\n", ok);

    void* domain = reinterpret_cast<DomainGetFn>(get("il2cpp_domain_get"))();
    size_t count = 0;
    void** assemblies = reinterpret_cast<GetAssembliesFn>(get("il2cpp_domain_get_assemblies"))(domain, &count);
    printf("%zu assemblies:\n", count);
    void* gameImage = nullptr;
    for (size_t i = 0; i < count; i++)
    {
        void* image = reinterpret_cast<AssemblyImageFn>(get("il2cpp_assembly_get_image"))(assemblies[i]);
        const char* name = reinterpret_cast<ImageNameFn>(get("il2cpp_image_get_name"))(image);
        printf("  %-45s %6zu classes\n", name, reinterpret_cast<ImageClassCountFn>(get("il2cpp_image_get_class_count"))(image));
        if (std::string(name) == "Assembly-CSharp.dll")
            gameImage = image;
    }

    if (gameImage)
    {
        for (const char* className : { "gsJoystick", "gsFps", "bsSession", "netSequencerDummy" })
        {
            void* klass = reinterpret_cast<ClassFromNameFn>(get("il2cpp_class_from_name"))(gameImage, "", className);
            printf("class %-20s %s\n", className, klass ? "found" : "NOT FOUND");
        }
    }
    return ok ? 0 : 1;
}

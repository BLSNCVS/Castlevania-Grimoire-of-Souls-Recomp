// Batch HLSL -> DXBC compiler around d3dcompiler_47.dll (loaded from the
// executable's folder first, then the system).
//
// usage: hlslc <list file>
//   each line: <profile> <input.hlsl> <output.dxbc>
// Errors go to <output.dxbc>.err; a summary line is printed at the end.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using CompileFn = decltype(&D3DCompile);
using StripFn = decltype(&D3DStripShader);

static std::string ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <list file>\n", argv[0]);
        return 1;
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    HMODULE dll = LoadLibraryA((dir + "d3dcompiler_47.dll").c_str());
    if (!dll)
        dll = LoadLibraryA("d3dcompiler_47.dll");
    if (!dll)
    {
        fprintf(stderr, "cannot load d3dcompiler_47.dll\n");
        return 1;
    }
    auto compile = reinterpret_cast<CompileFn>(GetProcAddress(dll, "D3DCompile"));
    auto strip = reinterpret_cast<StripFn>(GetProcAddress(dll, "D3DStripShader"));

    std::ifstream list(argv[1]);
    std::string profile, in, out;
    int ok = 0, failed = 0;
    while (list >> profile >> in >> out)
    {
        std::string src = ReadFile(in);
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = compile(src.data(), src.size(), in.c_str(), nullptr, nullptr, "main", profile.c_str(),
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (SUCCEEDED(hr))
        {
            ID3DBlob* stripped = nullptr;
            if (strip && SUCCEEDED(strip(code->GetBufferPointer(), code->GetBufferSize(),
                                         D3DCOMPILER_STRIP_DEBUG_INFO | D3DCOMPILER_STRIP_TEST_BLOBS, &stripped)))
            {
                code->Release();
                code = stripped;
            }
            std::ofstream(out, std::ios::binary).write(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
            code->Release();
            ok++;
        }
        else
        {
            std::ofstream e(out + ".err");
            if (errors)
                e.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            failed++;
        }
        if (errors)
            errors->Release();
    }
    printf("compiled %d, failed %d\n", ok, failed);
    return failed ? 2 : 0;
}

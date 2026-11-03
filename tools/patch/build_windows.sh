#!/usr/bin/env bash
# Builds gosr_patch.exe (the patch tool Setup.bat runs) for Windows x64.
# usage: build_windows.sh <out dir>   (MSVC_SYSROOT, CASE_ALIASES as for the other builds)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?usage: build_windows.sh <out dir>}"
M="${MSVC_SYSROOT:?set MSVC_SYSROOT to an xwin splat directory (CRT + Windows SDK)}"
CASE="${CASE_ALIASES:?set CASE_ALIASES to the header case-alias directory}"
mkdir -p "$OUT"
clang-cl-18 --target=x86_64-pc-windows-msvc /nologo /c /O2 /EHsc /MT /std:c++17 -Wno-everything \
    /imsvc"$CASE" /imsvc"$M/crt/include" /imsvc"$M/sdk/include/ucrt" /imsvc"$M/sdk/include/um" /imsvc"$M/sdk/include/shared" \
    /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH "$HERE/gosr_patch.cpp" /Fo"$OUT/gosr_patch.obj"
lld-link-18 /nologo /out:"$OUT/gosr_patch.exe" /subsystem:console \
    /libpath:"$M/crt/lib/x86_64" /libpath:"$M/sdk/lib/um/x86_64" /libpath:"$M/sdk/lib/ucrt/x86_64" \
    "$OUT/gosr_patch.obj" libcmt.lib libcpmt.lib libucrt.lib libvcruntime.lib kernel32.lib
echo "built $OUT/gosr_patch.exe"

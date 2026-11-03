#!/usr/bin/env bash
# Builds the Windows stand-ins for the game's macOS plugins (one DLL each,
# named like the bundles the game's P/Invokes ask for). The SPFX effects
# plugin is built separately (plugins/spfx/build.sh).
# usage: build.sh <out dir>   (MSVC_SYSROOT: xwin CRT + SDK)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?usage: build.sh <out dir>}"
M="${MSVC_SYSROOT:?set MSVC_SYSROOT to an xwin splat directory (CRT + Windows SDK)}"
CASE="${CASE_ALIASES:?set CASE_ALIASES to the header case-alias directory (recomp/winbuild/fix_case.py)}"
mkdir -p "$OUT/obj"
SYS="--target=x86_64-pc-windows-msvc /nologo /c /EHsc /MT /GS- /O2 /std:c++17 -Wno-everything /imsvc$CASE /imsvc$M/crt/include \
  /imsvc$M/sdk/include/ucrt /imsvc$M/sdk/include/um /imsvc$M/sdk/include/shared /DNOMINMAX /D_WIN32_WINNT=0x0A00 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"
LIBS="/libpath:$M/crt/lib/x86_64 /libpath:$M/sdk/lib/um/x86_64 /libpath:$M/sdk/lib/ucrt/x86_64 \
  ole32.lib user32.lib gdi32.lib kernel32.lib libcmt.lib libcpmt.lib libucrt.lib libvcruntime.lib"
build() {  # dll name, sources...
    local name="$1"; shift
    local objs=()
    for src in "$@"; do
        local obj="$OUT/obj/$(basename "${src%.cpp}").obj"
        clang-cl-18 $SYS "$HERE/$src" /Fo"$obj"
        objs+=("$obj")
    done
    lld-link-18 /nologo /dll /out:"$OUT/$name.dll" /implib:"$OUT/obj/$name.lib" /machine:x64 "${objs[@]}" $LIBS
}
build FPBundleWrapper fairplay.cpp
build GameCenterAccessPointBundleWrapper gamecenter_access_point.cpp
build iCloudBundleWrapper icloud.cpp
build GCControllerBundleWrapper gccontroller.cpp
build SymbolImageBundleWrapper symbol_image.cpp
build SystemLangBundle ../systemlang/systemlang.cpp

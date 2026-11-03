// libil2cpp's runtime field layout, built with the clang C++ ABI rule.
//
// The game code was compiled by clang for macOS, where a derived class's
// fields start in the tail padding of its base class (actual size, not the
// aligned size). libil2cpp picks the MSVC rule whenever _MSC_VER is defined,
// so generic instances laid out at runtime on Windows put base-class fields
// where the compiled code keeps derived-class fields (e.g. every
// cmnPopupBase<,> popup, whose bad references then crash the GC's liveness
// pass in UnloadUnusedAssets). This replaces metadata/FieldLayout.cpp.
#include "il2cpp-config.h"
#undef IL2CPP_CXX_ABI_MSVC
#define IL2CPP_CXX_ABI_MSVC 0
#include "metadata/FieldLayout.cpp"

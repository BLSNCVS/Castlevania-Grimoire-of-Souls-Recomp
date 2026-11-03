#pragma once
// Translates the GLSL programs SPFX generates (GLSL 1.40) to HLSL for
// Shader Model 5, through glslang (GLSL -> SPIR-V) and SPIRV-Cross
// (SPIR-V -> HLSL). Loose uniforms are gathered into one constant buffer per
// stage; varyings get matching locations by name.
#include <map>
#include <string>
#include <vector>

namespace spfx::gl
{
    struct HlslStage
    {
        std::string source;
        std::string error;
    };

    struct HlslProgram
    {
        HlslStage vertex, pixel;
        std::vector<std::string> attributes;  // vertex inputs in location order (TEXCOORD<location>)
    };

    bool TranslateProgram(const std::string& vertexGlsl, const std::string& fragmentGlsl, HlslProgram& out);
}

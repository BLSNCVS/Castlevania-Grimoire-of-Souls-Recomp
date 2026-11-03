#include "glsl_to_hlsl.h"

#include <mutex>
#include <regex>
#include <sstream>

#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv_hlsl.hpp"

namespace spfx::gl
{
    namespace
    {
        // GLSL 1.40 (what SPFX writes for a GL 2.1 context) as 410 core:
        // same language for these shaders, and the oldest one with locations
        // on vertex outputs.
        std::string Prepare(const std::string& src, const std::map<std::string, int>& varyingLocation, bool vertex,
                            std::vector<std::string>* attributes)
        {
            std::string s = std::regex_replace(src, std::regex(R"(#version\s+\d+[^\n]*)"), "#version 410 core");
            // varyings: matching locations in both stages, by name
            std::istringstream in(s);
            std::ostringstream out;
            std::string line;
            int attribute = 0;
            static const std::regex varying(R"(^\s*VARYING\s+\w+\s+(\w+)\s*;)");
            static const std::regex attrib(R"(^\s*ATTRIBUTE\s+\w+\s+(\w+)\s*;)");
            std::smatch m;
            while (std::getline(in, line))
            {
                if (std::regex_search(line, m, varying))
                {
                    auto it = varyingLocation.find(m[1]);
                    if (it != varyingLocation.end())
                        line = "layout(location=" + std::to_string(it->second) + ") " + line;
                }
                else if (vertex && std::regex_search(line, m, attrib))
                {
                    line = "layout(location=" + std::to_string(attribute++) + ") " + line;
                    if (attributes)
                        attributes->push_back(m[1]);
                }
                else if (!vertex && line.find("out vec4 FragColor") != std::string::npos)
                    line = "layout(location=0) " + line;
                out << line << "\n";
            }
            return out.str();
        }

        bool ToSpirv(const std::string& src, EShLanguage stage, std::vector<uint32_t>& spirv, std::string& error)
        {
            glslang::TShader shader(stage);
            const char* text = src.c_str();
            shader.setStrings(&text, 1);
            shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
            shader.setEnvInputVulkanRulesRelaxed();
            shader.setGlobalUniformBlockName("SPFXUniforms");
            shader.setGlobalUniformSet(0);
            shader.setGlobalUniformBinding(0);
            shader.setAutoMapLocations(true);
            shader.setAutoMapBindings(true);
            EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
            if (!shader.parse(GetDefaultResources(), 410, ECoreProfile, false, false, messages))
            {
                error = shader.getInfoLog();
                return false;
            }
            glslang::TProgram program;
            program.addShader(&shader);
            if (!program.link(messages) || !program.mapIO())
            {
                error = program.getInfoLog();
                return false;
            }
            glslang::SpvOptions options;
            options.generateDebugInfo = false;
            glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
            return true;
        }

        bool ToHlsl(const std::vector<uint32_t>& spirv, bool vertex, std::string& hlsl, std::string& error)
        {
            try
            {
                spirv_cross::CompilerHLSL compiler(spirv);
                spirv_cross::CompilerHLSL::Options options;
                options.shader_model = 50;
                compiler.set_hlsl_options(options);
                spirv_cross::CompilerGLSL::Options common = compiler.get_common_options();
                common.vertex.flip_vert_y = false;
                common.vertex.fixup_clipspace = false;  // the game hands SPFX D3D-style matrices
                compiler.set_common_options(common);
                // stage inputs/outputs as TEXCOORD<location>
                compiler.set_resource_binding_flags(0);
                hlsl = compiler.compile();
                (void)vertex;
                return true;
            }
            catch (const std::exception& e)
            {
                error = e.what();
                return false;
            }
        }
    }

    bool TranslateProgram(const std::string& vertexGlsl, const std::string& fragmentGlsl, HlslProgram& out)
    {
        static std::once_flag init;
        std::call_once(init, [] { glslang::InitializeProcess(); });

        // varying locations from the vertex stage's declaration order
        std::map<std::string, int> varyings;
        static const std::regex varying(R"(VARYING\s+\w+\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(vertexGlsl.begin(), vertexGlsl.end(), varying); it != std::sregex_iterator(); ++it)
            varyings.emplace((*it)[1], int(varyings.size()));

        out.attributes.clear();
        std::string vs = Prepare(vertexGlsl, varyings, true, &out.attributes);
        std::string fs = Prepare(fragmentGlsl, varyings, false, nullptr);
        std::vector<uint32_t> vsSpirv, fsSpirv;
        if (!ToSpirv(vs, EShLangVertex, vsSpirv, out.vertex.error))
            return false;
        if (!ToSpirv(fs, EShLangFragment, fsSpirv, out.pixel.error))
            return false;
        return ToHlsl(vsSpirv, true, out.vertex.source, out.vertex.error) &&
               ToHlsl(fsSpirv, false, out.pixel.source, out.pixel.error);
    }
}

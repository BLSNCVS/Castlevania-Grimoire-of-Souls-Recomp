// The OpenGL functions the SPFX plugin's GL backend calls: GL objects and
// state are kept here (gl_state.h); gl_d3d11.cpp draws with them on Unity's
// Direct3D 11 device.
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gl_state.h"
#include "recomp_bridge.h"
#include "recomp_simd.h"
#include "spfx_host.h"

namespace
{
    using recomp::Ctx;
    using namespace spfx::gl;

    void* P(uint64_t v) { return reinterpret_cast<void*>(v); }
    State& G() { return spfx::gl::GetState(); }

    // ------------------------------------------------------------ queries

    void GetString(Ctx& c)
    {
        static const char* vendor = "GoSRecomp";
        static const char* renderer = "GoSRecomp GL on Direct3D 11";
        static const char* version = "2.1 GoSRecomp";
        static const char* glsl = "1.20";
        static const char* extensions = "GL_APPLE_vertex_array_object GL_EXT_texture_compression_s3tc";
        switch (uint32_t(c.x[0]))
        {
        case 0x1F00: c.x[0] = reinterpret_cast<uint64_t>(vendor); break;
        case 0x1F01: c.x[0] = reinterpret_cast<uint64_t>(renderer); break;
        case 0x1F02: c.x[0] = reinterpret_cast<uint64_t>(version); break;
        case 0x8B8C: c.x[0] = reinterpret_cast<uint64_t>(glsl); break;
        case 0x1F03: c.x[0] = reinterpret_cast<uint64_t>(extensions); break;
        default: c.x[0] = 0; break;
        }
    }

    void GetStringi(Ctx& c) { c.x[0] = 0; }

    void GetIntegerv(Ctx& c)
    {
        auto* out = static_cast<int32_t*>(P(c.x[1]));
        switch (uint32_t(c.x[0]))
        {
        case 0x821D: *out = 0; break;           // GL_NUM_EXTENSIONS
        case 0x0D33: *out = 16384; break;       // GL_MAX_TEXTURE_SIZE
        case 0x8872: *out = 16; break;          // GL_MAX_TEXTURE_IMAGE_UNITS
        case 0x8869: *out = 16; break;          // GL_MAX_VERTEX_ATTRIBS
        case 0x8CA6: *out = 0; break;           // GL_DRAW_FRAMEBUFFER_BINDING / FRAMEBUFFER_BINDING
        case 0x0BA2: memcpy(out, G().viewport, 16); break;  // GL_VIEWPORT
        case 0x8B8D: *out = int32_t(G().currentProgram); break;  // GL_CURRENT_PROGRAM
        case 0x8069: *out = int32_t(G().unit[G().activeUnit].texture2d); break;  // GL_TEXTURE_BINDING_2D
        case 0x84E0: *out = int32_t(0x84C0 + G().activeUnit); break;  // GL_ACTIVE_TEXTURE
        case 0x8894: *out = int32_t(G().arrayBuffer); break;  // GL_ARRAY_BUFFER_BINDING
        case 0x8895: *out = int32_t(G().elementBuffer()); break;
        default: *out = 0; break;
        }
    }

    void GetFloatv(Ctx& c)
    {
        auto* out = static_cast<float*>(P(c.x[1]));
        switch (uint32_t(c.x[0]))
        {
        case 0x84FF: *out = 16.0f; break;  // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
        default: *out = 0.0f; break;
        }
    }

    // ------------------------------------------------------------ names

    template <typename T>
    void Gen(Ctx& c, std::unordered_map<uint32_t, T>& objects)
    {
        auto* names = static_cast<uint32_t*>(P(c.x[1]));
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        for (int32_t i = 0; i < int32_t(c.x[0]); i++)
        {
            uint32_t n = G().nextName++;
            objects[n];
            names[i] = n;
        }
    }

    template <typename T>
    void Delete(Ctx& c, std::unordered_map<uint32_t, T>& objects)
    {
        auto* names = static_cast<const uint32_t*>(P(c.x[1]));
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        for (int32_t i = 0; i < int32_t(c.x[0]); i++)
        {
            objects.erase(names[i]);
            spfx::gl::OnDelete(names[i]);
        }
    }

    void GenBuffers(Ctx& c) { Gen(c, G().buffers); }
    void DeleteBuffers(Ctx& c) { Delete(c, G().buffers); }
    void GenTextures(Ctx& c) { Gen(c, G().textures); }
    void DeleteTextures(Ctx& c) { Delete(c, G().textures); }
    void GenSamplers(Ctx& c) { Gen(c, G().samplers); }
    void DeleteSamplers(Ctx& c) { Delete(c, G().samplers); }
    void GenVertexArrays(Ctx& c) { Gen(c, G().vertexArrays); }
    void DeleteVertexArrays(Ctx& c) { Delete(c, G().vertexArrays); }

    // ------------------------------------------------------------ buffers

    void BindBuffer(Ctx& c)
    {
        uint32_t target = uint32_t(c.x[0]), name = uint32_t(c.x[1]);
        if (target == 0x8892)  // GL_ARRAY_BUFFER
            G().arrayBuffer = name;
        else if (target == 0x8893)  // GL_ELEMENT_ARRAY_BUFFER
            G().vao().elementBuffer = name;
    }

    Buffer* Bound(uint32_t target)
    {
        uint32_t name = target == 0x8892 ? G().arrayBuffer : G().elementBuffer();
        auto it = G().buffers.find(name);
        return it == G().buffers.end() ? nullptr : &it->second;
    }

    void BufferData(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        if (Buffer* b = Bound(uint32_t(c.x[0])))
        {
            b->data.assign(size_t(c.x[1]), 0);
            if (c.x[2])
                memcpy(b->data.data(), P(c.x[2]), size_t(c.x[1]));
            b->version++;
        }
    }

    void BufferSubData(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        if (Buffer* b = Bound(uint32_t(c.x[0])))
        {
            size_t off = size_t(c.x[1]), size = size_t(c.x[2]);
            if (off + size > b->data.size())
                b->data.resize(off + size);
            memcpy(b->data.data() + off, P(c.x[3]), size);
            b->version++;
        }
    }

    void BindVertexArray(Ctx& c) { G().currentVao = uint32_t(c.x[0]); }

    void VertexAttribPointer(Ctx& c)
    {
        uint32_t index = uint32_t(c.x[0]);
        if (index >= kMaxAttribs)
            return;
        Attrib& a = G().vao().attribs[index];
        a.size = int32_t(c.x[1]);
        a.type = uint32_t(c.x[2]);
        a.normalized = uint8_t(c.x[3]) != 0;
        a.stride = int32_t(c.x[4]);
        a.offset = c.x[5];
        a.buffer = G().arrayBuffer;
    }

    void EnableAttrib(Ctx& c)
    {
        if (uint32_t(c.x[0]) < kMaxAttribs)
            G().vao().attribs[uint32_t(c.x[0])].enabled = true;
    }

    void DisableAttrib(Ctx& c)
    {
        if (uint32_t(c.x[0]) < kMaxAttribs)
            G().vao().attribs[uint32_t(c.x[0])].enabled = false;
    }

    // ------------------------------------------------------------ textures

    void ActiveTexture(Ctx& c)
    {
        uint32_t unit = uint32_t(c.x[0]) - 0x84C0;
        if (unit < kMaxUnits)
            G().activeUnit = unit;
    }

    void BindTexture(Ctx& c)
    {
        uint32_t target = uint32_t(c.x[0]), name = uint32_t(c.x[1]);
        TextureUnit& u = G().unit[G().activeUnit];
        if (target == 0x8513)  // GL_TEXTURE_CUBE_MAP
            u.textureCube = name;
        else
            u.texture2d = name;
    }

    Texture* BoundTexture()
    {
        auto it = G().textures.find(G().unit[G().activeUnit].texture2d);
        return it == G().textures.end() ? nullptr : &it->second;
    }

    // glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels)
    void TexImage2D(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        Texture* t = BoundTexture();
        if (!t)
            return;
        int32_t level = int32_t(c.x[1]);
        uint32_t format = uint32_t(c.x[6]), type = uint32_t(c.x[7]);
        const void* pixels = P(*reinterpret_cast<uint64_t*>(c.sp));  // 9th argument
        spfx::gl::SetTextureImage(*t, level, uint32_t(c.x[2]), int32_t(c.x[3]), int32_t(c.x[4]), format, type, pixels, 0);
    }

    // glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data)
    void CompressedTexImage2D(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        Texture* t = BoundTexture();
        if (!t)
            return;
        spfx::gl::SetTextureImage(*t, int32_t(c.x[1]), uint32_t(c.x[2]), int32_t(c.x[3]), int32_t(c.x[4]), 0, 0,
                                  P(c.x[7]), int32_t(c.x[6]));
    }

    // glCopyTexImage2D / glCopyTexSubImage2D: screen copies for distortion effects
    void CopyTexImage(Ctx&)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        if (Texture* t = BoundTexture())
            spfx::gl::CopyFramebufferTo(*t);
    }

    void TexParameteri(Ctx& c)
    {
        if (Texture* t = BoundTexture())
            t->params[uint32_t(c.x[1])] = float(int32_t(c.x[2]));
    }

    void TexParameterf(Ctx& c)
    {
        if (Texture* t = BoundTexture())
            t->params[uint32_t(c.x[1])] = c.v[0].f32[0];
    }

    void BindSampler(Ctx& c)
    {
        if (uint32_t(c.x[0]) < kMaxUnits)
            G().unit[uint32_t(c.x[0])].sampler = uint32_t(c.x[1]);
    }

    void SamplerParameteri(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        G().samplers[uint32_t(c.x[0])].params[uint32_t(c.x[1])] = float(int32_t(c.x[2]));
    }

    void SamplerParameterf(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        G().samplers[uint32_t(c.x[0])].params[uint32_t(c.x[1])] = c.v[0].f32[0];
    }

    void PixelStorei(Ctx&) {}

    // ------------------------------------------------------------ shaders

    void CreateShader(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        uint32_t n = G().nextName++;
        G().shaders[n].type = uint32_t(c.x[0]);
        c.x[0] = n;
    }

    // glShaderSource(shader, count, strings, lengths)
    void ShaderSource(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        Shader& s = G().shaders[uint32_t(c.x[0])];
        s.source.clear();
        auto* strings = static_cast<const uint64_t*>(P(c.x[2]));
        auto* lengths = static_cast<const int32_t*>(P(c.x[3]));
        for (int32_t i = 0; i < int32_t(c.x[1]); i++)
        {
            const char* str = static_cast<const char*>(P(strings[i]));
            if (lengths && lengths[i] >= 0)
                s.source.append(str, size_t(lengths[i]));
            else
                s.source.append(str);
        }
    }

    void CompileShader(Ctx&) {}

    void GetShaderiv(Ctx& c)
    {
        auto* out = static_cast<int32_t*>(P(c.x[2]));
        uint32_t pname = uint32_t(c.x[1]);
        *out = pname == 0x8B81 ? 1 : 0;  // GL_COMPILE_STATUS: true; GL_INFO_LOG_LENGTH: 0
    }

    void GetInfoLog(Ctx& c)
    {
        if (c.x[2])
            *static_cast<int32_t*>(P(c.x[2])) = 0;
        if (c.x[3] && c.x[1])
            *static_cast<char*>(P(c.x[3])) = 0;
    }

    void DeleteShader(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        G().shaders.erase(uint32_t(c.x[0]));
    }

    void CreateProgram(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        uint32_t n = G().nextName++;
        G().programs[n];
        c.x[0] = n;
    }

    void AttachShader(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        Program& p = G().programs[uint32_t(c.x[0])];
        auto it = G().shaders.find(uint32_t(c.x[1]));
        if (it == G().shaders.end())
            return;
        (it->second.type == 0x8B31 ? p.vertexSource : p.fragmentSource) = it->second.source;
    }

    void LinkProgram(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        spfx::gl::LinkProgram(uint32_t(c.x[0]), G().programs[uint32_t(c.x[0])]);
    }

    void GetProgramiv(Ctx& c)
    {
        auto* out = static_cast<int32_t*>(P(c.x[2]));
        *out = uint32_t(c.x[1]) == 0x8B82 ? 1 : 0;  // GL_LINK_STATUS
    }

    void DeleteProgram(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        G().programs.erase(uint32_t(c.x[0]));
    }

    void UseProgram(Ctx& c) { G().currentProgram = uint32_t(c.x[0]); }

    void GetAttribLocation(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        Program& p = G().programs[uint32_t(c.x[0])];
        auto it = p.attribs.find(static_cast<const char*>(P(c.x[1])));
        c.x[0] = uint64_t(int64_t(it == p.attribs.end() ? -1 : it->second));
    }

    void GetUniformLocation(Ctx& c)
    {
        std::lock_guard<std::recursive_mutex> guard(G().lock);
        Program& p = G().programs[uint32_t(c.x[0])];
        auto it = p.uniformIndex.find(static_cast<const char*>(P(c.x[1])));
        c.x[0] = uint64_t(int64_t(it == p.uniformIndex.end() ? -1 : it->second));
    }

    Program* Current()
    {
        auto it = G().programs.find(G().currentProgram);
        return it == G().programs.end() ? nullptr : &it->second;
    }

    void Uniform1i(Ctx& c)
    {
        if (Program* p = Current())
            spfx::gl::SetUniform(*p, int32_t(c.x[0]), &c.x[1], 0, true);
    }

    void Uniform4fv(Ctx& c)
    {
        if (Program* p = Current())
            spfx::gl::SetUniform(*p, int32_t(c.x[0]), P(c.x[2]), int32_t(c.x[1]) * 16, false);
    }

    // glUniformMatrix4fv(location, count, transpose, value)
    void UniformMatrix4fv(Ctx& c)
    {
        if (Program* p = Current())
            spfx::gl::SetUniformMatrix(*p, int32_t(c.x[0]), static_cast<const float*>(P(c.x[3])), int32_t(c.x[1]),
                                       uint8_t(c.x[2]) != 0);
    }

    // ------------------------------------------------------------ state

    void Enable(Ctx& c) { G().SetCap(uint32_t(c.x[0]), true); }
    void Disable(Ctx& c) { G().SetCap(uint32_t(c.x[0]), false); }
    void BlendEquationSeparate(Ctx& c) { G().blendEqRgb = uint32_t(c.x[0]); G().blendEqAlpha = uint32_t(c.x[1]); }
    void BlendFuncSeparate(Ctx& c)
    {
        G().srcRgb = uint32_t(c.x[0]); G().dstRgb = uint32_t(c.x[1]);
        G().srcAlpha = uint32_t(c.x[2]); G().dstAlpha = uint32_t(c.x[3]);
    }
    void ColorMask(Ctx& c)
    {
        G().colorMask = (uint8_t(c.x[0]) ? 1 : 0) | (uint8_t(c.x[1]) ? 2 : 0) | (uint8_t(c.x[2]) ? 4 : 0) | (uint8_t(c.x[3]) ? 8 : 0);
    }
    void CullFace(Ctx& c) { G().cullFace = uint32_t(c.x[0]); }
    void FrontFace(Ctx& c) { G().frontFace = uint32_t(c.x[0]); }
    void DepthFunc(Ctx& c) { G().depthFunc = uint32_t(c.x[0]); }
    void DepthMask(Ctx& c) { G().depthMask = uint8_t(c.x[0]) != 0; }
    void PolygonMode(Ctx&) {}
    void StencilFuncSeparate(Ctx& c) { G().stencilFunc = uint32_t(c.x[1]); G().stencilRef = int32_t(c.x[2]); G().stencilReadMask = uint32_t(c.x[3]); }
    void StencilMaskSeparate(Ctx& c) { G().stencilWriteMask = uint32_t(c.x[1]); }
    void StencilOpSeparate(Ctx& c) { G().stencilFail = uint32_t(c.x[1]); G().stencilDepthFail = uint32_t(c.x[2]); G().stencilPass = uint32_t(c.x[3]); }

    // ------------------------------------------------------------ draws

    void DrawArrays(Ctx& c) { spfx::gl::Draw(uint32_t(c.x[0]), int32_t(c.x[1]), int32_t(c.x[2]), 0, 0); }
    // glDrawElements(mode, count, type, indices)
    void DrawElements(Ctx& c) { spfx::gl::Draw(uint32_t(c.x[0]), 0, int32_t(c.x[1]), uint32_t(c.x[2]), c.x[3]); }
}

namespace spfx
{
    void RegisterGlImports()
    {
        RegisterHandler("_glGetString", GetString);
        RegisterHandler("_glGetStringi", GetStringi);
        RegisterHandler("_glGetIntegerv", GetIntegerv);
        RegisterHandler("_glGetFloatv", GetFloatv);
        RegisterHandler("_glGenBuffers", GenBuffers);
        RegisterHandler("_glDeleteBuffers", DeleteBuffers);
        RegisterHandler("_glGenTextures", GenTextures);
        RegisterHandler("_glDeleteTextures", DeleteTextures);
        RegisterHandler("_glGenSamplers", GenSamplers);
        RegisterHandler("_glDeleteSamplers", DeleteSamplers);
        RegisterHandler("_glGenVertexArraysAPPLE", GenVertexArrays);
        RegisterHandler("_glDeleteVertexArraysAPPLE", DeleteVertexArrays);
        RegisterHandler("_glBindVertexArrayAPPLE", BindVertexArray);
        RegisterHandler("_glBindBuffer", BindBuffer);
        RegisterHandler("_glBufferData", BufferData);
        RegisterHandler("_glBufferSubData", BufferSubData);
        RegisterHandler("_glVertexAttribPointer", VertexAttribPointer);
        RegisterHandler("_glEnableVertexAttribArray", EnableAttrib);
        RegisterHandler("_glDisableVertexAttribArray", DisableAttrib);
        RegisterHandler("_glActiveTexture", ActiveTexture);
        RegisterHandler("_glBindTexture", BindTexture);
        RegisterHandler("_glTexImage2D", TexImage2D);
        RegisterHandler("_glCompressedTexImage2D", CompressedTexImage2D);
        RegisterHandler("_glCopyTexImage2D", CopyTexImage);
        RegisterHandler("_glCopyTexSubImage2D", CopyTexImage);
        RegisterHandler("_glTexParameteri", TexParameteri);
        RegisterHandler("_glTexParameterf", TexParameterf);
        RegisterHandler("_glBindSampler", BindSampler);
        RegisterHandler("_glSamplerParameteri", SamplerParameteri);
        RegisterHandler("_glSamplerParameterf", SamplerParameterf);
        RegisterHandler("_glPixelStorei", PixelStorei);
        RegisterHandler("_glCreateShader", CreateShader);
        RegisterHandler("_glShaderSource", ShaderSource);
        RegisterHandler("_glCompileShader", CompileShader);
        RegisterHandler("_glGetShaderiv", GetShaderiv);
        RegisterHandler("_glGetShaderInfoLog", GetInfoLog);
        RegisterHandler("_glDeleteShader", DeleteShader);
        RegisterHandler("_glCreateProgram", CreateProgram);
        RegisterHandler("_glAttachShader", AttachShader);
        RegisterHandler("_glLinkProgram", LinkProgram);
        RegisterHandler("_glGetProgramiv", GetProgramiv);
        RegisterHandler("_glGetProgramInfoLog", GetInfoLog);
        RegisterHandler("_glDeleteProgram", DeleteProgram);
        RegisterHandler("_glUseProgram", UseProgram);
        RegisterHandler("_glGetAttribLocation", GetAttribLocation);
        RegisterHandler("_glGetUniformLocation", GetUniformLocation);
        RegisterHandler("_glUniform1i", Uniform1i);
        RegisterHandler("_glUniform4fv", Uniform4fv);
        RegisterHandler("_glUniformMatrix4fv", UniformMatrix4fv);
        RegisterHandler("_glEnable", Enable);
        RegisterHandler("_glDisable", Disable);
        RegisterHandler("_glBlendEquationSeparate", BlendEquationSeparate);
        RegisterHandler("_glBlendFuncSeparate", BlendFuncSeparate);
        RegisterHandler("_glColorMask", ColorMask);
        RegisterHandler("_glCullFace", CullFace);
        RegisterHandler("_glFrontFace", FrontFace);
        RegisterHandler("_glDepthFunc", DepthFunc);
        RegisterHandler("_glDepthMask", DepthMask);
        RegisterHandler("_glPolygonMode", PolygonMode);
        RegisterHandler("_glStencilFuncSeparate", StencilFuncSeparate);
        RegisterHandler("_glStencilMaskSeparate", StencilMaskSeparate);
        RegisterHandler("_glStencilOpSeparate", StencilOpSeparate);
        RegisterHandler("_glDrawArrays", DrawArrays);
        RegisterHandler("_glDrawElements", DrawElements);
    }
}

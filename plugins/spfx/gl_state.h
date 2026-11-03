#pragma once
// GL objects and state of the emulated OpenGL 2.1 context the SPFX plugin
// draws with (spfx_gl.cpp records, gl_d3d11.cpp draws).
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace spfx::gl
{
    constexpr uint32_t kMaxAttribs = 16;
    constexpr uint32_t kMaxUnits = 16;

    struct Buffer
    {
        std::vector<uint8_t> data;
        uint64_t version = 0;
    };

    struct Attrib
    {
        bool enabled = false, normalized = false;
        int32_t size = 4, stride = 0;
        uint32_t type = 0x1406;  // GL_FLOAT
        uint64_t offset = 0;
        uint32_t buffer = 0;
    };

    struct VertexArray
    {
        Attrib attribs[kMaxAttribs];
        uint32_t elementBuffer = 0;
    };

    struct Texture
    {
        uint32_t internalFormat = 0, width = 0, height = 0;
        bool compressed = false, screenCopy = false;
        std::vector<std::vector<uint8_t>> levels;  // RGBA8 or block-compressed data per mip level
        std::map<uint32_t, float> params;
        uint64_t version = 0;
        void* gpu = nullptr;  // backend object
        void* external = nullptr;  // a Unity D3D11 texture (ID3D11Resource*) the name stands for
    };

    struct Sampler
    {
        std::map<uint32_t, float> params;
    };

    struct Shader
    {
        uint32_t type = 0;
        std::string source;
    };

    struct Uniform
    {
        std::string name;
        uint32_t type = 0;  // 0 float vector/matrix data, 1 sampler
        int32_t arraySize = 1;
        uint32_t floats = 4;  // per element: 4 (vectors, scalars) or 16 (mat4, GL column-major)
        std::vector<float> values;
        int32_t samplerUnit = 0;
        bool cube = false;
    };

    struct Program
    {
        std::string vertexSource, fragmentSource;
        std::map<std::string, int32_t> attribs;           // attribute -> location
        std::map<std::string, int32_t> uniformIndex;      // name (and name[i]) -> location
        std::vector<Uniform> uniforms;                     // by location
        std::vector<int32_t> locationElement;              // location -> element index within its uniform
        std::vector<uint32_t> locationUniform;             // location -> uniform
        void* gpu = nullptr;                               // backend object
        std::shared_ptr<void> build;                       // backend shader build
        bool linked = false;
    };

    struct TextureUnit
    {
        uint32_t texture2d = 0, textureCube = 0, sampler = 0;
    };

    struct State
    {
        std::recursive_mutex lock;
        uint32_t nextName = 1;
        std::unordered_map<uint32_t, Buffer> buffers;
        std::unordered_map<uint32_t, Texture> textures;
        std::unordered_map<uint32_t, Sampler> samplers;
        std::unordered_map<uint32_t, VertexArray> vertexArrays;
        std::unordered_map<uint32_t, Shader> shaders;
        std::unordered_map<uint32_t, Program> programs;

        uint32_t currentVao = 0, arrayBuffer = 0, currentProgram = 0, activeUnit = 0;
        VertexArray defaultVao;
        TextureUnit unit[kMaxUnits];
        int32_t viewport[4] = { 0, 0, 1, 1 };

        bool blend = false, depthTest = false, cull = false, stencil = false, scissor = false;
        uint32_t blendEqRgb = 0x8006, blendEqAlpha = 0x8006;  // GL_FUNC_ADD
        uint32_t srcRgb = 1, dstRgb = 0, srcAlpha = 1, dstAlpha = 0;
        uint32_t colorMask = 15;
        uint32_t cullFace = 0x0405, frontFace = 0x0901;  // GL_BACK, GL_CCW
        uint32_t depthFunc = 0x0201;  // GL_LESS
        bool depthMask = true;
        uint32_t stencilFunc = 0x0207, stencilReadMask = 0xFF, stencilWriteMask = 0xFF;
        int32_t stencilRef = 0;
        uint32_t stencilFail = 0x1E00, stencilDepthFail = 0x1E00, stencilPass = 0x1E00;

        VertexArray& vao()
        {
            if (!currentVao)
                return defaultVao;
            return vertexArrays[currentVao];
        }
        uint32_t elementBuffer() { return vao().elementBuffer; }

        void SetCap(uint32_t cap, bool on)
        {
            switch (cap)
            {
            case 0x0BE2: blend = on; break;
            case 0x0B71: depthTest = on; break;
            case 0x0B44: cull = on; break;
            case 0x0B90: stencil = on; break;
            case 0x0C11: scissor = on; break;
            default: break;
            }
        }
    };

    State& GetState();

    // backend (gl_d3d11.cpp)
    void SetTextureImage(Texture& t, int32_t level, uint32_t internalFormat, int32_t width, int32_t height,
                         uint32_t format, uint32_t type, const void* pixels, int32_t compressedSize);
    void LinkProgram(uint32_t name, Program& p);
    void SetUniform(Program& p, int32_t location, const void* data, int32_t bytes, bool isInt);
    void SetUniformMatrix(Program& p, int32_t location, const float* data, int32_t count, bool transpose);
    void Draw(uint32_t mode, int32_t first, int32_t count, uint32_t indexType, uint64_t indices);
    void OnDelete(uint32_t name);
    void CopyFramebufferTo(Texture& t);
}

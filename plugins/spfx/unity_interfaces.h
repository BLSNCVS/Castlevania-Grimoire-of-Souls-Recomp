#pragma once
// The parts of Unity's native plugin API (IUnityInterface.h,
// IUnityGraphics.h, IUnityGraphicsD3D11.h) this plugin uses.
#include <cstdint>

#define UNITY_INTERFACE_API __stdcall

struct UnityInterfaceGUID
{
    unsigned long long m_GUIDHigh;
    unsigned long long m_GUIDLow;
};

struct IUnityInterface
{
};

struct IUnityInterfaces
{
    IUnityInterface*(UNITY_INTERFACE_API* GetInterface)(UnityInterfaceGUID guid);
    void(UNITY_INTERFACE_API* RegisterInterface)(UnityInterfaceGUID guid, IUnityInterface* ptr);
    IUnityInterface*(UNITY_INTERFACE_API* GetInterfaceSplit)(unsigned long long guidHigh, unsigned long long guidLow);
    void(UNITY_INTERFACE_API* RegisterInterfaceSplit)(unsigned long long guidHigh, unsigned long long guidLow, IUnityInterface* ptr);

    template <typename T>
    T* Get()
    {
        return static_cast<T*>(GetInterfaceSplit(T::kGuidHigh, T::kGuidLow));
    }
};

enum UnityGfxRenderer
{
    kUnityGfxRendererD3D11 = 2,
    kUnityGfxRendererNull = 4,
    kUnityGfxRendererOpenGLES20 = 8,
    kUnityGfxRendererOpenGLES30 = 11,
    kUnityGfxRendererMetal = 16,
    kUnityGfxRendererOpenGLCore = 17,
    kUnityGfxRendererD3D12 = 18,
    kUnityGfxRendererVulkan = 21,
};

enum UnityGfxDeviceEventType
{
    kUnityGfxDeviceEventInitialize = 0,
    kUnityGfxDeviceEventShutdown = 1,
    kUnityGfxDeviceEventBeforeReset = 2,
    kUnityGfxDeviceEventAfterReset = 3,
};

typedef void(UNITY_INTERFACE_API* IUnityGraphicsDeviceEventCallback)(UnityGfxDeviceEventType eventType);

struct IUnityGraphics : IUnityInterface
{
    static constexpr unsigned long long kGuidHigh = 0x7CBA0A9CA4DDB544ull, kGuidLow = 0x8C5AD4926EB17B11ull;
    UnityGfxRenderer(UNITY_INTERFACE_API* GetRenderer)();
    void(UNITY_INTERFACE_API* RegisterDeviceEventCallback)(IUnityGraphicsDeviceEventCallback callback);
    void(UNITY_INTERFACE_API* UnregisterDeviceEventCallback)(IUnityGraphicsDeviceEventCallback callback);
    int(UNITY_INTERFACE_API* ReserveEventIDRange)(int count);
};

struct ID3D11Device;
struct ID3D11Resource;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
typedef struct RenderSurfaceBase* UnityRenderBuffer;
typedef unsigned int UnityTextureID;

struct IUnityGraphicsD3D11 : IUnityInterface
{
    static constexpr unsigned long long kGuidHigh = 0xAAB37EF87A87D748ull, kGuidLow = 0xBF76967F07EFB177ull;
    ID3D11Device*(UNITY_INTERFACE_API* GetDevice)();
    ID3D11Resource*(UNITY_INTERFACE_API* TextureFromRenderBuffer)(UnityRenderBuffer buffer);
    ID3D11Resource*(UNITY_INTERFACE_API* TextureFromNativeTexture)(UnityTextureID texture);
    ID3D11RenderTargetView*(UNITY_INTERFACE_API* RTVFromRenderBuffer)(UnityRenderBuffer surface);
    ID3D11ShaderResourceView*(UNITY_INTERFACE_API* SRVFromNativeTexture)(UnityTextureID texture);
};

namespace spfx
{
    IUnityInterfaces* RealInterfaces();
}

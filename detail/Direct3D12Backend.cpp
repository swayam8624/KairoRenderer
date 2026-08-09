#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "Direct3D12Backend.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kairo::renderer::detail
{
    using Microsoft::WRL::ComPtr;

    static_assert(sizeof(Direct3D12Vertex) == 44u);
    static_assert(sizeof(Direct3D12DebugVertex) == 28u);
    static_assert(sizeof(Direct3D12Light) == 80u);

    namespace
    {
        constexpr UINT FrameCount = 2u;
        constexpr UINT ShadowResolution = 2048u;
        constexpr DXGI_FORMAT ColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr DXGI_FORMAT ObjectIDFormat = DXGI_FORMAT_R32_UINT;
        constexpr DXGI_FORMAT DepthFormat = DXGI_FORMAT_D32_FLOAT;

        void Require(HRESULT result, const char* task)
        {
            if (SUCCEEDED(result)) return;
            char code[16]{};
            std::snprintf(code, sizeof(code), "0x%08X",
                static_cast<unsigned>(result));
            throw std::runtime_error(std::string(task) + " failed (" + code + ").");
        }

        [[nodiscard]] UINT64 Align(UINT64 value, UINT64 alignment) noexcept
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

        [[nodiscard]] D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            return barrier;
        }

        [[nodiscard]] D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type)
        {
            D3D12_HEAP_PROPERTIES value{};
            value.Type = type;
            value.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            value.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            value.CreationNodeMask = 1u;
            value.VisibleNodeMask = 1u;
            return value;
        }

        [[nodiscard]] D3D12_RESOURCE_DESC BufferDesc(UINT64 size)
        {
            D3D12_RESOURCE_DESC value{};
            value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            value.Width = size;
            value.Height = 1u;
            value.DepthOrArraySize = 1u;
            value.MipLevels = 1u;
            value.Format = DXGI_FORMAT_UNKNOWN;
            value.SampleDesc.Count = 1u;
            value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return value;
        }

        [[nodiscard]] D3D12_RASTERIZER_DESC Rasterizer()
        {
            D3D12_RASTERIZER_DESC value{};
            value.FillMode = D3D12_FILL_MODE_SOLID;
            value.CullMode = D3D12_CULL_MODE_BACK;
            value.FrontCounterClockwise = TRUE;
            value.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
            value.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
            value.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
            value.DepthClipEnable = TRUE;
            value.MultisampleEnable = FALSE;
            value.AntialiasedLineEnable = FALSE;
            value.ForcedSampleCount = 0u;
            value.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            return value;
        }

        [[nodiscard]] D3D12_BLEND_DESC Blend(bool alpha)
        {
            D3D12_BLEND_DESC value{};
            value.AlphaToCoverageEnable = FALSE;
            value.IndependentBlendEnable = TRUE;
            for (auto& target : value.RenderTarget)
            {
                target.BlendEnable = FALSE;
                target.LogicOpEnable = FALSE;
                target.SrcBlend = D3D12_BLEND_ONE;
                target.DestBlend = D3D12_BLEND_ZERO;
                target.BlendOp = D3D12_BLEND_OP_ADD;
                target.SrcBlendAlpha = D3D12_BLEND_ONE;
                target.DestBlendAlpha = D3D12_BLEND_ZERO;
                target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                target.LogicOp = D3D12_LOGIC_OP_NOOP;
                target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }
            if (alpha)
            {
                auto& target = value.RenderTarget[0];
                target.BlendEnable = TRUE;
                target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                target.SrcBlendAlpha = D3D12_BLEND_ONE;
                target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            }
            return value;
        }

        [[nodiscard]] D3D12_DEPTH_STENCIL_DESC Depth(bool write)
        {
            D3D12_DEPTH_STENCIL_DESC value{};
            value.DepthEnable = TRUE;
            value.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL :
                D3D12_DEPTH_WRITE_MASK_ZERO;
            value.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            value.StencilEnable = FALSE;
            return value;
        }

        [[nodiscard]] ComPtr<ID3DBlob> Compile(const char* source,
            const char* entry, const char* profile)
        {
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
            ComPtr<ID3DBlob> shader;
            ComPtr<ID3DBlob> errors;
            const HRESULT result = D3DCompile(source, std::strlen(source),
                "KairoDirect3D12", nullptr, nullptr, entry, profile, flags, 0u,
                &shader, &errors);
            if (FAILED(result))
            {
                const std::string message = errors
                    ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                        errors->GetBufferSize())
                    : "unknown shader compiler error";
                throw std::runtime_error("Direct3D 12 shader compilation failed: " +
                    message);
            }
            return shader;
        }

        constexpr const char* ShaderSource = R"hlsl(
#define PI 3.14159265359
struct Light { float4 positionType; float4 directionRange; float4 colorIntensity; float4 spotArea; uint castsShadows; uint3 padding; };
cbuffer Frame : register(b0) {
 row_major float4x4 view; row_major float4x4 projection; row_major float4x4 lightViewProjection;
 float4 cameraPosition; float4 ambientExposure; float4 backgroundEnvironment;
 uint lightCount; int shadowLightIndex; uint shadingMode; uint shadowsEnabled;
 float shadowStrength; float receiverBias; float shadowTexel; float framePadding;
 Light lights[16];
};
cbuffer Draw : register(b1) {
 row_major float4x4 model; float4 normal0; float4 normal1; float4 normal2;
 float4 baseColor; float4 emissiveNormalScale; float4 factors;
 uint alphaMode; uint objectID; uint receiveShadows; uint drawPadding;
};
Texture2D baseTex:register(t0); Texture2D normalTex:register(t1); Texture2D mrTex:register(t2);
Texture2D emissiveTex:register(t3); Texture2D occlusionTex:register(t4); Texture2D environmentTex:register(t5);
Texture2D<float> shadowTex:register(t6);
SamplerState baseSampler:register(s0); SamplerState normalSampler:register(s1); SamplerState mrSampler:register(s2);
SamplerState emissiveSampler:register(s3); SamplerState occlusionSampler:register(s4); SamplerState environmentSampler:register(s5);
SamplerComparisonState shadowSampler:register(s6);
struct VertexIn { float3 position:POSITION; float3 color:COLOR; float3 normal:NORMAL; float2 uv:TEXCOORD; };
struct VertexOut { float4 position:SV_Position; float3 world:WORLD; float3 color:COLOR; float3 normal:NORMAL; float2 uv:TEXCOORD; float4 shadow:SHADOW; };
VertexOut MeshVS(VertexIn i) { VertexOut o; float4 world=mul(float4(i.position,1),model); o.position=mul(mul(world,view),projection); o.world=world.xyz; float3x3 n=float3x3(normal0.xyz,normal1.xyz,normal2.xyz); o.normal=normalize(mul(i.normal,n)); o.color=i.color; o.uv=i.uv; o.shadow=mul(world,lightViewProjection); return o; }
float3 Fresnel(float c,float3 f0){return f0+(1-f0)*pow(saturate(1-c),5);}
float2 EnvironmentUV(float3 d){d=normalize(d);return float2(atan2(d.z,d.x)/(2*PI)+.5,asin(clamp(d.y,-1,1))/PI+.5);}
struct PixelOut { float4 color:SV_Target0; uint id:SV_Target1; };
PixelOut MeshPS(VertexOut i) {
 float4 base=baseTex.Sample(baseSampler,i.uv)*baseColor*float4(i.color,1); if(alphaMode==2&&base.a<factors.w)discard;
 float4 mr=mrTex.Sample(mrSampler,i.uv); float metallic=saturate(factors.x*mr.b); float roughness=clamp(factors.y*mr.g,.045,1); float ao=saturate(factors.z*occlusionTex.Sample(occlusionSampler,i.uv).r);
 float3 N=normalize(i.normal),V=normalize(cameraPosition.xyz-i.world); float3 tn=normalTex.Sample(normalSampler,i.uv).xyz*2-1;
 float3 dx=ddx(i.world),dy=ddy(i.world);float2 ux=ddx(i.uv),uy=ddy(i.uv);float det=ux.x*uy.y-ux.y*uy.x;
 if(abs(det)>1e-8){float3 T=normalize((dx*uy.y-dy*ux.y)/det);float3 B=normalize(cross(N,T))*sign(det);tn.xy*=emissiveNormalScale.w;N=normalize(mul(tn,float3x3(T,B,N)));}
 PixelOut o;if(shadingMode==2){o.color=float4(N*.5+.5,1);o.id=objectID;return o;}
 float3 f0=lerp(.04.xxx,base.rgb,metallic),direct=0;float diagnostic=0;
 [loop]for(uint n=0;n<lightCount;n++){Light light=lights[n];uint type=(uint)light.positionType.w;float3 L;float attenuation=1;
  if(type==1)L=normalize(light.directionRange.xyz);else{float3 delta=light.positionType.xyz-i.world;float distance=length(delta);L=delta/max(distance,1e-4);float normalized=distance/max(light.directionRange.w,1e-4);attenuation=pow(saturate(1-pow(normalized,4)),2)/max(distance*distance,.01);if(type==3)attenuation*=smoothstep(light.spotArea.y,light.spotArea.x,dot(-L,normalize(light.directionRange.xyz)));else if(type==4)attenuation*=max(dot(-L,normalize(light.directionRange.xyz)),0)*light.spotArea.z*light.spotArea.w;}
  float nl=max(dot(N,L),0),nv=max(dot(N,V),1e-4),visibility=1;if(shadowsEnabled&&receiveShadows&&int(n)==shadowLightIndex&&i.shadow.w>0){float3 sc=i.shadow.xyz/i.shadow.w;float2 uv=float2(sc.x*.5+.5,.5-sc.y*.5);if(all(uv>=0)&&all(uv<=1)&&sc.z>=0&&sc.z<=1){float sum=0;[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)sum+=shadowTex.SampleCmpLevelZero(shadowSampler,uv+float2(x,y)*shadowTexel,sc.z-receiverBias);visibility=lerp(1,sum/9,shadowStrength);}}
  diagnostic+=nl*attenuation*visibility*light.colorIntensity.w;if(nl<=0||attenuation<=0)continue;float3 H=normalize(V+L);float nh=max(dot(N,H),0),vh=max(dot(V,H),0);float a=roughness*roughness,a2=a*a,d=nh*nh*(a2-1)+1;float D=a2/max(PI*d*d,1e-4),k=(roughness+1)*(roughness+1)/8;float G=(nl/(nl*(1-k)+k))*(nv/(nv*(1-k)+k));float3 F=Fresnel(vh,f0);float3 spec=D*G*F/max(4*nl*nv,1e-4),diff=(1-F)*(1-metallic)*base.rgb/PI;direct+=(diff+spec)*light.colorIntensity.rgb*light.colorIntensity.w*attenuation*nl*visibility;}
 float3 emissive=emissiveNormalScale.xyz*emissiveTex.Sample(emissiveSampler,i.uv).rgb;float3 reflected=reflect(-V,N);uint w,h,levels;environmentTex.GetDimensions(0,w,h,levels);float3 environment=environmentTex.SampleLevel(environmentSampler,EnvironmentUV(reflected),roughness*max(int(levels)-1,0)).rgb*backgroundEnvironment.w;float nv=max(dot(N,V),0);float3 ambient=(ambientExposure.rgb*base.rgb+environment*base.rgb*(1-metallic)+environment*Fresnel(nv,f0)*(1-roughness*.65))*ao;
 float3 color=shadingMode==1?base.rgb+emissive:(shadingMode==3?diagnostic.xxx:ambient+direct+emissive);color*=exp2(ambientExposure.w);color=color/(color+1);o.color=float4(color,alphaMode==3?base.a:1);o.id=objectID;return o;
}
float4 ShadowVS(VertexIn i):SV_Position{return mul(mul(float4(i.position,1),model),lightViewProjection);}
struct DebugIn{float3 position:POSITION;float4 color:COLOR;};struct DebugOut{float4 position:SV_Position;float4 color:COLOR;};
DebugOut DebugVS(DebugIn i){DebugOut o;o.position=mul(mul(float4(i.position,1),view),projection);o.color=i.color;return o;}float4 DebugPS(DebugOut i):SV_Target{return i.color;}
struct FullscreenOut{float4 position:SV_Position;float2 uv:TEXCOORD;};
FullscreenOut FullscreenVS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);FullscreenOut o;o.position=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.uv=uv;return o;}
float4 PresentPS(FullscreenOut i):SV_Target{return baseTex.Sample(baseSampler,i.uv);}
)hlsl";

        class DescriptorAllocator final
        {
            ComPtr<ID3D12DescriptorHeap> m_Heap;
            UINT m_Increment = 0u;
            UINT m_Capacity = 0u;
            UINT m_Next = 0u;
            std::vector<UINT> m_Free;

        public:
            void Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                UINT capacity, bool shaderVisible)
            {
                D3D12_DESCRIPTOR_HEAP_DESC desc{};
                desc.Type = type;
                desc.NumDescriptors = capacity;
                desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE :
                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                Require(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_Heap)),
                    "Direct3D 12 descriptor-heap creation");
                m_Increment = device->GetDescriptorHandleIncrementSize(type);
                m_Capacity = capacity;
            }

            [[nodiscard]] UINT Allocate()
            {
                if (!m_Free.empty())
                {
                    const UINT index = m_Free.back();
                    m_Free.pop_back();
                    return index;
                }
                if (m_Next >= m_Capacity)
                    throw std::runtime_error("Direct3D 12 descriptor heap is exhausted.");
                return m_Next++;
            }

            void Free(UINT index)
            {
                if (index >= m_Next || std::ranges::find(m_Free, index) != m_Free.end())
                    throw std::invalid_argument("Direct3D 12 descriptor was not allocated.");
                m_Free.push_back(index);
            }

            [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CPU(UINT index) const
            {
                auto handle = m_Heap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += static_cast<SIZE_T>(index) * m_Increment;
                return handle;
            }

            [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GPU(UINT index) const
            {
                auto handle = m_Heap->GetGPUDescriptorHandleForHeapStart();
                handle.ptr += static_cast<UINT64>(index) * m_Increment;
                return handle;
            }

            [[nodiscard]] ID3D12DescriptorHeap* Heap() const noexcept
            {
                return m_Heap.Get();
            }
        };

        struct GpuMesh final
        {
            ComPtr<ID3D12Resource> Vertices;
            ComPtr<ID3D12Resource> Indices;
            D3D12_VERTEX_BUFFER_VIEW VertexView{};
            D3D12_INDEX_BUFFER_VIEW IndexView{};
            UINT IndexCount = 0u;
        };

        struct GpuTexture final
        {
            ComPtr<ID3D12Resource> Resource;
            UINT SRV = std::numeric_limits<UINT>::max();
            UINT Sampler = std::numeric_limits<UINT>::max();
        };

        struct alignas(256) FrameConstants final
        {
            float View[16]{};
            float Projection[16]{};
            float LightViewProjection[16]{};
            float CameraPosition[4]{};
            float AmbientExposure[4]{};
            float BackgroundEnvironment[4]{};
            std::uint32_t LightCount = 0u;
            std::int32_t ShadowLightIndex = -1;
            std::uint32_t ShadingMode = 0u;
            std::uint32_t ShadowsEnabled = 0u;
            float ShadowStrength = 0.0f;
            float ReceiverBias = 0.0f;
            float ShadowTexel = 0.0f;
            float Padding = 0.0f;
            Direct3D12Light Lights[16]{};
        };

        struct alignas(256) DrawConstants final
        {
            float Model[16]{};
            float Normal0[4]{};
            float Normal1[4]{};
            float Normal2[4]{};
            float BaseColor[4]{};
            float EmissiveNormalScale[4]{};
            float Factors[4]{};
            std::uint32_t AlphaMode = 1u;
            std::uint32_t ObjectID = 0u;
            std::uint32_t ReceiveShadows = 1u;
            std::uint32_t Padding = 0u;
        };
    }

    class Direct3D12Backend::Impl final
    {
    public:
        ComPtr<IDXGIFactory6> Factory;
        ComPtr<ID3D12Device> Device;
        ComPtr<ID3D12CommandQueue> Queue;
        ComPtr<IDXGISwapChain3> Swapchain;
        std::array<ComPtr<ID3D12Resource>, FrameCount> BackBuffers;
        ComPtr<ID3D12DescriptorHeap> RTVHeap;
        ComPtr<ID3D12DescriptorHeap> DSVHeap;
        UINT RTVIncrement = 0u;
        UINT DSVIncrement = 0u;
        DescriptorAllocator SRVs;
        DescriptorAllocator Samplers;
        ComPtr<ID3D12CommandAllocator> Allocator;
        ComPtr<ID3D12GraphicsCommandList> Commands;
        ComPtr<ID3D12Fence> Fence;
        HANDLE FenceEvent = nullptr;
        UINT64 FenceValue = 0u;
        ComPtr<ID3D12RootSignature> RootSignature;
        ComPtr<ID3D12PipelineState> MeshPipeline;
        ComPtr<ID3D12PipelineState> BlendPipeline;
        ComPtr<ID3D12PipelineState> DoubleSidedMeshPipeline;
        ComPtr<ID3D12PipelineState> DoubleSidedBlendPipeline;
        ComPtr<ID3D12PipelineState> ShadowPipeline;
        ComPtr<ID3D12PipelineState> DebugPipeline;
        ComPtr<ID3D12PipelineState> PresentPipeline;
        ComPtr<ID3D12Resource> ConstantUpload;
        std::byte* ConstantMapped = nullptr;
        UINT64 ConstantCapacity = 8u * 1024u * 1024u;
        UINT64 ConstantCursor = 0u;
        ComPtr<ID3D12Resource> ViewportColor;
        ComPtr<ID3D12Resource> ViewportID;
        ComPtr<ID3D12Resource> ViewportDepth;
        ComPtr<ID3D12Resource> ShadowDepth;
        UINT ViewportSRV = std::numeric_limits<UINT>::max();
        UINT ShadowSRV = std::numeric_limits<UINT>::max();
        UINT ShadowSampler = std::numeric_limits<UINT>::max();
        D3D12_RESOURCE_STATES ColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_STATES IDState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        std::unordered_map<std::uint64_t, GpuMesh> Meshes;
        std::unordered_map<std::uint64_t, GpuTexture> Textures;
        GpuTexture White;
        GpuTexture Normal;
        GpuTexture Black;
        std::uint64_t NextMesh = 1u;
        std::uint64_t NextTexture = 1u;
        std::uint32_t ViewportWidth = 1u;
        std::uint32_t ViewportHeight = 1u;
        std::uint32_t DrawableWidth = 1u;
        std::uint32_t DrawableHeight = 1u;
        Direct3D12OverlayRecorder Overlay;

        Impl(GLFWwindow* window, std::uint32_t width, std::uint32_t height)
            : ViewportWidth(width), ViewportHeight(height), DrawableWidth(width),
              DrawableHeight(height)
        {
            HWND hwnd = glfwGetWin32Window(window);
            if (hwnd == nullptr)
                throw std::runtime_error("GLFW did not expose a Win32 window for Direct3D 12.");
#if defined(_DEBUG)
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
#endif
            Require(CreateDXGIFactory2(0u, IID_PPV_ARGS(&Factory)),
                "DXGI factory creation");
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT index = 0u; Factory->EnumAdapterByGpuPreference(index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
                DXGI_ERROR_NOT_FOUND; ++index)
            {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0u &&
                    SUCCEEDED(D3D12CreateDevice(adapter.Get(),
                        D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)))) break;
                adapter.Reset();
            }
            if (!Device)
            {
                ComPtr<IDXGIAdapter> warp;
                if (SUCCEEDED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
                    (void)D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0,
                        IID_PPV_ARGS(&Device));
            }
            if (!Device)
                Require(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                    IID_PPV_ARGS(&Device)), "Direct3D 12 device creation");

            D3D12_COMMAND_QUEUE_DESC queue{};
            queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            Require(Device->CreateCommandQueue(&queue, IID_PPV_ARGS(&Queue)),
                "Direct3D 12 command-queue creation");
            DXGI_SWAP_CHAIN_DESC1 swap{};
            swap.Width = width;
            swap.Height = height;
            swap.Format = ColorFormat;
            swap.SampleDesc.Count = 1u;
            swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swap.BufferCount = FrameCount;
            swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<IDXGISwapChain1> baseSwapchain;
            Require(Factory->CreateSwapChainForHwnd(Queue.Get(), hwnd, &swap,
                nullptr, nullptr, &baseSwapchain), "DXGI swap-chain creation");
            Require(baseSwapchain.As(&Swapchain), "DXGI swap-chain query");
            Factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

            D3D12_DESCRIPTOR_HEAP_DESC rtv{};
            rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtv.NumDescriptors = FrameCount + 2u;
            Require(Device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&RTVHeap)),
                "Direct3D 12 RTV heap creation");
            RTVIncrement = Device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_DESCRIPTOR_HEAP_DESC dsv{};
            dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsv.NumDescriptors = 2u;
            Require(Device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&DSVHeap)),
                "Direct3D 12 DSV heap creation");
            DSVIncrement = Device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            SRVs.Initialize(Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                4096u, true);
            Samplers.Initialize(Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                1024u, true);

            Require(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&Allocator)), "Direct3D 12 command allocator creation");
            Require(Device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT,
                Allocator.Get(), nullptr, IID_PPV_ARGS(&Commands)),
                "Direct3D 12 command-list creation");
            Require(Commands->Close(), "Direct3D 12 initial command-list close");
            Require(Device->CreateFence(0u, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&Fence)), "Direct3D 12 fence creation");
            FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (FenceEvent == nullptr)
                throw std::runtime_error("Direct3D 12 fence event creation failed.");

            BuildRootSignatureAndPipelines();
            BuildBackBuffers();
            BuildTargets(width, height);
            ConstantUpload = CreateBuffer(ConstantCapacity,
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            Require(ConstantUpload->Map(0u, nullptr,
                reinterpret_cast<void**>(&ConstantMapped)),
                "Direct3D 12 constant-buffer mapping");
            White = SolidTexture(255u, 255u, 255u, 255u);
            Normal = SolidTexture(128u, 128u, 255u, 255u);
            Black = SolidTexture(0u, 0u, 0u, 255u);
        }

        ~Impl()
        {
            try { WaitIdle(); } catch (...) {}
            if (ConstantUpload && ConstantMapped) ConstantUpload->Unmap(0u, nullptr);
            if (FenceEvent != nullptr) CloseHandle(FenceEvent);
        }

        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE RTV(UINT index) const
        {
            auto handle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * RTVIncrement;
            return handle;
        }

        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DSV(UINT index) const
        {
            auto handle = DSVHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * DSVIncrement;
            return handle;
        }

        void WaitIdle()
        {
            const UINT64 value = ++FenceValue;
            Require(Queue->Signal(Fence.Get(), value), "Direct3D 12 queue signal");
            if (Fence->GetCompletedValue() >= value) return;
            Require(Fence->SetEventOnCompletion(value, FenceEvent),
                "Direct3D 12 fence wait registration");
            WaitForSingleObject(FenceEvent, INFINITE);
        }

        void BeginCommands()
        {
            WaitIdle();
            Require(Allocator->Reset(), "Direct3D 12 command allocator reset");
            Require(Commands->Reset(Allocator.Get(), nullptr),
                "Direct3D 12 command-list reset");
        }

        void ExecuteCommands()
        {
            Require(Commands->Close(), "Direct3D 12 command-list close");
            ID3D12CommandList* lists[]{ Commands.Get() };
            Queue->ExecuteCommandLists(1u, lists);
            WaitIdle();
        }

        [[nodiscard]] ComPtr<ID3D12Resource> CreateBuffer(UINT64 size,
            D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state)
        {
            const auto heap = Heap(heapType);
            const auto desc = BufferDesc(size);
            ComPtr<ID3D12Resource> resource;
            Require(Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, state, nullptr, IID_PPV_ARGS(&resource)),
                "Direct3D 12 buffer allocation");
            return resource;
        }

        [[nodiscard]] ComPtr<ID3D12Resource> UploadBuffer(const void* bytes,
            UINT64 size, D3D12_RESOURCE_STATES finalState)
        {
            auto destination = CreateBuffer(size, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST);
            auto upload = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped = nullptr;
            Require(upload->Map(0u, nullptr, &mapped),
                "Direct3D 12 upload-buffer mapping");
            std::memcpy(mapped, bytes, static_cast<std::size_t>(size));
            upload->Unmap(0u, nullptr);
            BeginCommands();
            Commands->CopyBufferRegion(destination.Get(), 0u, upload.Get(), 0u, size);
            auto barrier = Transition(destination.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, finalState);
            Commands->ResourceBarrier(1u, &barrier);
            ExecuteCommands();
            return destination;
        }

        void BuildBackBuffers()
        {
            for (UINT index = 0u; index < FrameCount; ++index)
            {
                Require(Swapchain->GetBuffer(index, IID_PPV_ARGS(&BackBuffers[index])),
                    "Direct3D 12 swap-chain buffer query");
                Device->CreateRenderTargetView(BackBuffers[index].Get(), nullptr,
                    RTV(index));
            }
        }

        void ResizeSwapchain(std::uint32_t width, std::uint32_t height)
        {
            if (width == DrawableWidth && height == DrawableHeight) return;
            WaitIdle();
            for (auto& buffer : BackBuffers) buffer.Reset();
            Require(Swapchain->ResizeBuffers(FrameCount, width, height, ColorFormat,
                0u), "Direct3D 12 swap-chain resize");
            DrawableWidth = width;
            DrawableHeight = height;
            BuildBackBuffers();
        }

        [[nodiscard]] ComPtr<ID3D12Resource> Texture2D(std::uint32_t width,
            std::uint32_t height, DXGI_FORMAT format,
            D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
            const D3D12_CLEAR_VALUE* clear = nullptr, std::uint16_t mips = 1u)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1u;
            desc.MipLevels = mips;
            desc.Format = format;
            desc.SampleDesc.Count = 1u;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = flags;
            const auto heap = Heap(D3D12_HEAP_TYPE_DEFAULT);
            ComPtr<ID3D12Resource> result;
            Require(Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, state, clear, IID_PPV_ARGS(&result)),
                "Direct3D 12 texture allocation");
            return result;
        }

        void BuildTargets(std::uint32_t width, std::uint32_t height)
        {
            WaitIdle();
            ViewportWidth = width;
            ViewportHeight = height;
            if (ViewportSRV != std::numeric_limits<UINT>::max()) SRVs.Free(ViewportSRV);
            D3D12_CLEAR_VALUE colorClear{};
            colorClear.Format = ColorFormat;
            ViewportColor = Texture2D(width, height, ColorFormat,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear);
            D3D12_CLEAR_VALUE idClear{};
            idClear.Format = ObjectIDFormat;
            ViewportID = Texture2D(width, height, ObjectIDFormat,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &idClear);
            D3D12_CLEAR_VALUE depthClear{};
            depthClear.Format = DepthFormat;
            depthClear.DepthStencil.Depth = 1.0f;
            ViewportDepth = Texture2D(width, height, DepthFormat,
                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);
            Device->CreateRenderTargetView(ViewportColor.Get(), nullptr, RTV(FrameCount));
            Device->CreateRenderTargetView(ViewportID.Get(), nullptr, RTV(FrameCount + 1u));
            Device->CreateDepthStencilView(ViewportDepth.Get(), nullptr, DSV(0u));
            ViewportSRV = SRVs.Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = ColorFormat;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1u;
            Device->CreateShaderResourceView(ViewportColor.Get(), &srv,
                SRVs.CPU(ViewportSRV));
            ColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            IDState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (!ShadowDepth) BuildShadowTarget();
            auto colorBarrier = Transition(ViewportColor.Get(), ColorState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto idBarrier = Transition(ViewportID.Get(), IDState,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            BeginCommands();
            Commands->ResourceBarrier(1u, &colorBarrier);
            Commands->ResourceBarrier(1u, &idBarrier);
            ExecuteCommands();
            ColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            IDState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }

        void BuildShadowTarget()
        {
            D3D12_CLEAR_VALUE clear{};
            clear.Format = DepthFormat;
            clear.DepthStencil.Depth = 1.0f;
            ShadowDepth = Texture2D(ShadowResolution, ShadowResolution,
                DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear);
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DepthFormat;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            Device->CreateDepthStencilView(ShadowDepth.Get(), &dsv, DSV(1u));
            ShadowSRV = SRVs.Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1u;
            Device->CreateShaderResourceView(ShadowDepth.Get(), &srv,
                SRVs.CPU(ShadowSRV));
            ShadowSampler = Samplers.Allocate();
            D3D12_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            sampler.AddressU = sampler.AddressV = sampler.AddressW =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            Device->CreateSampler(&sampler, Samplers.CPU(ShadowSampler));
        }

        void BuildRootSignatureAndPipelines()
        {
            std::array<D3D12_DESCRIPTOR_RANGE, 14> ranges{};
            std::array<D3D12_ROOT_PARAMETER, 16> parameters{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[0].Descriptor.ShaderRegister = 0u;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[1].Descriptor.ShaderRegister = 1u;
            parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            for (UINT index = 0u; index < 7u; ++index)
            {
                ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                ranges[index].NumDescriptors = 1u;
                ranges[index].BaseShaderRegister = index;
                ranges[index].OffsetInDescriptorsFromTableStart = 0u;
                parameters[2u + index].ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[2u + index].DescriptorTable = { 1u, &ranges[index] };
                parameters[2u + index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                ranges[7u + index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                ranges[7u + index].NumDescriptors = 1u;
                ranges[7u + index].BaseShaderRegister = index;
                ranges[7u + index].OffsetInDescriptorsFromTableStart = 0u;
                parameters[9u + index].ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[9u + index].DescriptorTable =
                    { 1u, &ranges[7u + index] };
                parameters[9u + index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            }
            D3D12_ROOT_SIGNATURE_DESC root{};
            root.NumParameters = static_cast<UINT>(parameters.size());
            root.pParameters = parameters.data();
            root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ComPtr<ID3DBlob> signature;
            ComPtr<ID3DBlob> errors;
            const HRESULT serialized = D3D12SerializeRootSignature(&root,
                D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
            if (FAILED(serialized))
                throw std::runtime_error("Direct3D 12 root-signature serialization failed: " +
                    (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                        errors->GetBufferSize()) : std::string("unknown error")));
            Require(Device->CreateRootSignature(0u, signature->GetBufferPointer(),
                signature->GetBufferSize(), IID_PPV_ARGS(&RootSignature)),
                "Direct3D 12 root-signature creation");

            auto meshVS = Compile(ShaderSource, "MeshVS", "vs_5_1");
            auto meshPS = Compile(ShaderSource, "MeshPS", "ps_5_1");
            const D3D12_INPUT_ELEMENT_DESC meshInput[]{
                { "POSITION",0u,DXGI_FORMAT_R32G32B32_FLOAT,0u,0u,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0u },
                { "COLOR",0u,DXGI_FORMAT_R32G32B32_FLOAT,0u,12u,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0u },
                { "NORMAL",0u,DXGI_FORMAT_R32G32B32_FLOAT,0u,24u,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0u },
                { "TEXCOORD",0u,DXGI_FORMAT_R32G32_FLOAT,0u,36u,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0u }
            };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = RootSignature.Get();
            pipeline.VS = { meshVS->GetBufferPointer(), meshVS->GetBufferSize() };
            pipeline.PS = { meshPS->GetBufferPointer(), meshPS->GetBufferSize() };
            pipeline.BlendState = Blend(false);
            pipeline.SampleMask = UINT_MAX;
            pipeline.RasterizerState = Rasterizer();
            pipeline.DepthStencilState = Depth(true);
            pipeline.InputLayout = { meshInput, static_cast<UINT>(std::size(meshInput)) };
            pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline.NumRenderTargets = 2u;
            pipeline.RTVFormats[0] = ColorFormat;
            pipeline.RTVFormats[1] = ObjectIDFormat;
            pipeline.DSVFormat = DepthFormat;
            pipeline.SampleDesc.Count = 1u;
            Require(Device->CreateGraphicsPipelineState(&pipeline,
                IID_PPV_ARGS(&MeshPipeline)), "Direct3D 12 mesh pipeline creation");
            pipeline.BlendState = Blend(true);
            pipeline.DepthStencilState = Depth(false);
            Require(Device->CreateGraphicsPipelineState(&pipeline,
                IID_PPV_ARGS(&BlendPipeline)), "Direct3D 12 blend pipeline creation");
            pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            Require(Device->CreateGraphicsPipelineState(&pipeline,
                IID_PPV_ARGS(&DoubleSidedBlendPipeline)),
                "Direct3D 12 double-sided blend pipeline creation");
            pipeline.BlendState = Blend(false);
            pipeline.DepthStencilState = Depth(true);
            Require(Device->CreateGraphicsPipelineState(&pipeline,
                IID_PPV_ARGS(&DoubleSidedMeshPipeline)),
                "Direct3D 12 double-sided mesh pipeline creation");

            auto shadowVS = Compile(ShaderSource, "ShadowVS", "vs_5_1");
            pipeline.VS = { shadowVS->GetBufferPointer(), shadowVS->GetBufferSize() };
            pipeline.PS = {};
            pipeline.BlendState = Blend(false);
            pipeline.DepthStencilState = Depth(true);
            pipeline.RasterizerState = Rasterizer();
            pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pipeline.RasterizerState.DepthBias = 2;
            pipeline.RasterizerState.SlopeScaledDepthBias = 1.75f;
            pipeline.NumRenderTargets = 0u;
            pipeline.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
            pipeline.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
            Require(Device->CreateGraphicsPipelineState(&pipeline,
                IID_PPV_ARGS(&ShadowPipeline)), "Direct3D 12 shadow pipeline creation");

            auto debugVS = Compile(ShaderSource, "DebugVS", "vs_5_1");
            auto debugPS = Compile(ShaderSource, "DebugPS", "ps_5_1");
            const D3D12_INPUT_ELEMENT_DESC debugInput[]{
                { "POSITION",0u,DXGI_FORMAT_R32G32B32_FLOAT,0u,0u,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0u },
                { "COLOR",0u,DXGI_FORMAT_R32G32B32A32_FLOAT,0u,12u,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0u }
            };
            pipeline.VS = { debugVS->GetBufferPointer(), debugVS->GetBufferSize() };
            pipeline.PS = { debugPS->GetBufferPointer(), debugPS->GetBufferSize() };
            pipeline.InputLayout = { debugInput, static_cast<UINT>(std::size(debugInput)) };
            pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            pipeline.NumRenderTargets = 1u;
            pipeline.RTVFormats[0] = ColorFormat;
            pipeline.DSVFormat = DepthFormat;
            pipeline.RasterizerState = Rasterizer();
            pipeline.DepthStencilState = Depth(false);
            Require(Device->CreateGraphicsPipelineState(&pipeline,
                IID_PPV_ARGS(&DebugPipeline)), "Direct3D 12 debug pipeline creation");

            auto presentVS = Compile(ShaderSource, "FullscreenVS", "vs_5_1");
            auto presentPS = Compile(ShaderSource, "PresentPS", "ps_5_1");
            pipeline.VS = { presentVS->GetBufferPointer(), presentVS->GetBufferSize() };
            pipeline.PS = { presentPS->GetBufferPointer(), presentPS->GetBufferSize() };
            pipeline.InputLayout = {};
            pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline.NumRenderTargets = 1u;
            pipeline.RTVFormats[0] = ColorFormat;
            pipeline.RasterizerState = Rasterizer();
            pipeline.DepthStencilState = {};
            pipeline.DSVFormat = DXGI_FORMAT_UNKNOWN;
            Require(Device->CreateGraphicsPipelineState(&pipeline,
                IID_PPV_ARGS(&PresentPipeline)),
                "Direct3D 12 presentation pipeline creation");
        }

        [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS UploadConstants(
            const void* data, UINT64 size)
        {
            const UINT64 aligned = Align(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            if (ConstantCursor + aligned > ConstantCapacity)
                throw std::runtime_error("Direct3D 12 per-frame constant arena is exhausted.");
            std::memcpy(ConstantMapped + ConstantCursor, data,
                static_cast<std::size_t>(size));
            const auto address = ConstantUpload->GetGPUVirtualAddress() + ConstantCursor;
            ConstantCursor += aligned;
            return address;
        }

        [[nodiscard]] static DrawConstants MakeDraw(const Direct3D12Draw& draw)
        {
            DrawConstants output;
            std::copy_n(draw.Model, 16u, output.Model);
            output.Normal0[0]=draw.Normal[0];output.Normal0[1]=draw.Normal[1];output.Normal0[2]=draw.Normal[2];
            output.Normal1[0]=draw.Normal[3];output.Normal1[1]=draw.Normal[4];output.Normal1[2]=draw.Normal[5];
            output.Normal2[0]=draw.Normal[6];output.Normal2[1]=draw.Normal[7];output.Normal2[2]=draw.Normal[8];
            std::copy_n(draw.Material.BaseColor,4u,output.BaseColor);
            std::copy_n(draw.Material.Emissive,3u,output.EmissiveNormalScale);
            output.EmissiveNormalScale[3]=draw.Material.NormalScale;
            output.Factors[0]=draw.Material.Metallic;
            output.Factors[1]=draw.Material.Roughness;
            output.Factors[2]=draw.Material.AmbientOcclusion;
            output.Factors[3]=draw.Material.AlphaCutoff;
            output.AlphaMode=draw.Material.AlphaMode;
            output.ObjectID=draw.Material.ObjectID;
            output.ReceiveShadows=draw.Material.ReceiveShadows?1u:0u;
            return output;
        }

        [[nodiscard]] FrameConstants MakeFrame(const Direct3D12Frame& frame) const
        {
            FrameConstants output;
            std::copy_n(frame.View,16u,output.View);
            std::copy_n(frame.Projection,16u,output.Projection);
            std::copy_n(frame.LightViewProjection,16u,output.LightViewProjection);
            std::copy_n(frame.CameraPosition,3u,output.CameraPosition);
            std::copy_n(frame.Ambient,3u,output.AmbientExposure);
            output.AmbientExposure[3]=frame.Exposure;
            std::copy_n(frame.Background,3u,output.BackgroundEnvironment);
            output.BackgroundEnvironment[3]=frame.EnvironmentIntensity;
            output.LightCount=static_cast<std::uint32_t>(
                std::min<std::size_t>(16u,frame.Lights.size()));
            output.ShadowLightIndex=-1;
            output.ShadingMode=frame.ShadingMode;
            output.ShadowStrength=frame.ShadowStrength;
            output.ReceiverBias=frame.ReceiverBias;
            output.ShadowTexel=1.0f/ShadowResolution;
            for(std::size_t index=0u;index<output.LightCount;++index)
            {
                output.Lights[index]=frame.Lights[index];
                if(output.ShadowLightIndex<0&&frame.Lights[index].CastShadows&&
                    static_cast<unsigned>(frame.Lights[index].PositionType[3])==1u)
                    output.ShadowLightIndex=static_cast<int>(index);
            }
            output.ShadowsEnabled=frame.ShadowsEnabled&&
                output.ShadowLightIndex>=0?1u:0u;
            return output;
        }

        [[nodiscard]] const GpuTexture& TextureOr(std::uint64_t handle,
            const GpuTexture& fallback) const
        {
            if(handle==0u)return fallback;
            const auto found=Textures.find(handle);
            if(found==Textures.end())
                throw std::out_of_range("Direct3D 12 draw references an unknown texture handle.");
            return found->second;
        }

        void BindTexture(UINT slot,const GpuTexture& texture)
        {
            Commands->SetGraphicsRootDescriptorTable(2u+slot,SRVs.GPU(texture.SRV));
            Commands->SetGraphicsRootDescriptorTable(9u+slot,
                Samplers.GPU(texture.Sampler));
        }

        void BindDrawTextures(const Direct3D12Draw& draw,
            const Direct3D12Frame& frame)
        {
            BindTexture(0u,TextureOr(draw.Material.BaseColorTexture,White));
            BindTexture(1u,TextureOr(draw.Material.NormalTexture,Normal));
            BindTexture(2u,TextureOr(draw.Material.MetallicRoughnessTexture,White));
            BindTexture(3u,TextureOr(draw.Material.EmissiveTexture,White));
            BindTexture(4u,TextureOr(draw.Material.OcclusionTexture,White));
            BindTexture(5u,TextureOr(frame.EnvironmentTexture,Black));
            Commands->SetGraphicsRootDescriptorTable(8u,SRVs.GPU(ShadowSRV));
            Commands->SetGraphicsRootDescriptorTable(15u,Samplers.GPU(ShadowSampler));
        }

        void DrawMesh(const Direct3D12Draw& draw,
            D3D12_GPU_VIRTUAL_ADDRESS drawConstants)
        {
            const auto mesh=Meshes.find(draw.Mesh);
            if(mesh==Meshes.end())
                throw std::out_of_range("Direct3D 12 draw references an unknown mesh handle.");
            Commands->SetGraphicsRootConstantBufferView(1u,drawConstants);
            Commands->IASetVertexBuffers(0u,1u,&mesh->second.VertexView);
            Commands->IASetIndexBuffer(&mesh->second.IndexView);
            Commands->DrawIndexedInstanced(mesh->second.IndexCount,1u,0u,0,0u);
        }

        void EncodeShadow(const Direct3D12Frame& frame,
            const FrameConstants& frameConstants,
            D3D12_GPU_VIRTUAL_ADDRESS frameAddress)
        {
            if(frameConstants.ShadowsEnabled==0u)return;
            auto toDepth=Transition(ShadowDepth.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Commands->ResourceBarrier(1u,&toDepth);
            D3D12_VIEWPORT viewport{0.0f,0.0f,static_cast<float>(ShadowResolution),
                static_cast<float>(ShadowResolution),0.0f,1.0f};
            D3D12_RECT scissor{0,0,static_cast<LONG>(ShadowResolution),
                static_cast<LONG>(ShadowResolution)};
            Commands->RSSetViewports(1u,&viewport);
            Commands->RSSetScissorRects(1u,&scissor);
            const auto shadowDepth = DSV(1u);
            Commands->OMSetRenderTargets(0u,nullptr,FALSE,&shadowDepth);
            Commands->ClearDepthStencilView(shadowDepth,D3D12_CLEAR_FLAG_DEPTH,
                1.0f,0u,0u,nullptr);
            Commands->SetPipelineState(ShadowPipeline.Get());
            Commands->SetGraphicsRootSignature(RootSignature.Get());
            Commands->SetGraphicsRootConstantBufferView(0u,frameAddress);
            Commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for(const auto& draw:frame.Draws)
            {
                if(!draw.CastShadows||draw.Material.AlphaMode==3u)continue;
                const auto constants=MakeDraw(draw);
                DrawMesh(draw,UploadConstants(&constants,sizeof(constants)));
            }
            auto toRead=Transition(ShadowDepth.Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Commands->ResourceBarrier(1u,&toRead);
        }

        void EncodeScene(const Direct3D12Frame& frame)
        {
            ConstantCursor=0u;
            const FrameConstants frameConstants=MakeFrame(frame);
            const auto frameAddress=UploadConstants(&frameConstants,
                sizeof(frameConstants));
            EncodeShadow(frame,frameConstants,frameAddress);
            auto colorBarrier=Transition(ViewportColor.Get(),ColorState,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto idBarrier=Transition(ViewportID.Get(),IDState,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            Commands->ResourceBarrier(1u,&colorBarrier);
            Commands->ResourceBarrier(1u,&idBarrier);
            ColorState=D3D12_RESOURCE_STATE_RENDER_TARGET;
            IDState=D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_VIEWPORT viewport{0.0f,0.0f,static_cast<float>(ViewportWidth),
                static_cast<float>(ViewportHeight),0.0f,1.0f};
            D3D12_RECT scissor{0,0,static_cast<LONG>(ViewportWidth),
                static_cast<LONG>(ViewportHeight)};
            Commands->RSSetViewports(1u,&viewport);
            Commands->RSSetScissorRects(1u,&scissor);
            const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{RTV(FrameCount),
                RTV(FrameCount+1u)};
            const auto depth=DSV(0u);
            Commands->OMSetRenderTargets(2u,targets,FALSE,&depth);
            const float clear[]{frame.Background[0],frame.Background[1],
                frame.Background[2],1.0f};
            const float clearID[]{0.0f,0.0f,0.0f,0.0f};
            Commands->ClearRenderTargetView(targets[0],clear,0u,nullptr);
            Commands->ClearRenderTargetView(targets[1],clearID,0u,nullptr);
            Commands->ClearDepthStencilView(depth,D3D12_CLEAR_FLAG_DEPTH,
                1.0f,0u,0u,nullptr);
            ID3D12DescriptorHeap* heaps[]{SRVs.Heap(),Samplers.Heap()};
            Commands->SetDescriptorHeaps(2u,heaps);
            Commands->SetGraphicsRootSignature(RootSignature.Get());
            Commands->SetGraphicsRootConstantBufferView(0u,frameAddress);
            Commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            std::vector<const Direct3D12Draw*> ordered;
            std::vector<const Direct3D12Draw*> transparent;
            for(const auto& draw:frame.Draws)
                (draw.Material.AlphaMode==3u?transparent:ordered).push_back(&draw);
            std::stable_sort(transparent.begin(),transparent.end(),
                [&](const auto* a,const auto* b)
                {
                    const auto distance=[&](const auto* draw)
                    {
                        const float x=draw->Model[3]-frame.CameraPosition[0];
                        const float y=draw->Model[7]-frame.CameraPosition[1];
                        const float z=draw->Model[11]-frame.CameraPosition[2];
                        return x*x+y*y+z*z;
                    };
                    return distance(a)>distance(b);
                });
            ordered.insert(ordered.end(),transparent.begin(),transparent.end());
            for(const auto* draw:ordered)
            {
                const bool blending=draw->Material.AlphaMode==3u;
                if(draw->Material.DoubleSided)
                    Commands->SetPipelineState(blending?
                        DoubleSidedBlendPipeline.Get():DoubleSidedMeshPipeline.Get());
                else Commands->SetPipelineState(blending?
                    BlendPipeline.Get():MeshPipeline.Get());
                BindDrawTextures(*draw,frame);
                const auto constants=MakeDraw(*draw);
                DrawMesh(*draw,UploadConstants(&constants,sizeof(constants)));
            }

            if(!frame.DebugVertices.empty())
            {
                const UINT64 bytes=frame.DebugVertices.size_bytes();
                if(ConstantCursor+Align(bytes,256u)>ConstantCapacity)
                    throw std::runtime_error("Direct3D 12 debug vertices exceed upload arena.");
                std::memcpy(ConstantMapped+ConstantCursor,
                    frame.DebugVertices.data(),static_cast<std::size_t>(bytes));
                D3D12_VERTEX_BUFFER_VIEW view{};
                view.BufferLocation=ConstantUpload->GetGPUVirtualAddress()+ConstantCursor;
                view.SizeInBytes=static_cast<UINT>(bytes);
                view.StrideInBytes=sizeof(Direct3D12DebugVertex);
                ConstantCursor+=Align(bytes,256u);
                Commands->SetPipelineState(DebugPipeline.Get());
                Commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
                Commands->IASetVertexBuffers(0u,1u,&view);
                Commands->DrawInstanced(static_cast<UINT>(frame.DebugVertices.size()),
                    1u,0u,0u);
            }
            colorBarrier=Transition(ViewportColor.Get(),ColorState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            idBarrier=Transition(ViewportID.Get(),IDState,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            Commands->ResourceBarrier(1u,&colorBarrier);
            Commands->ResourceBarrier(1u,&idBarrier);
            ColorState=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            IDState=D3D12_RESOURCE_STATE_COPY_SOURCE;
        }

        void Present()
        {
            const UINT index=Swapchain->GetCurrentBackBufferIndex();
            auto& back=BackBuffers[index];
            if(Overlay)
            {
                auto barrier=Transition(back.Get(),D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                Commands->ResourceBarrier(1u,&barrier);
                const auto target=RTV(index);
                Commands->OMSetRenderTargets(1u,&target,FALSE,nullptr);
                constexpr float clear[]{0.02f,0.025f,0.035f,1.0f};
                Commands->ClearRenderTargetView(target,clear,0u,nullptr);
                ID3D12DescriptorHeap* heap=SRVs.Heap();
                Commands->SetDescriptorHeaps(1u,&heap);
                Overlay(Commands.Get());
                barrier=Transition(back.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PRESENT);
                Commands->ResourceBarrier(1u,&barrier);
            }
            else
            {
                auto barrier=Transition(back.Get(),D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                Commands->ResourceBarrier(1u,&barrier);
                const auto target=RTV(index);
                Commands->OMSetRenderTargets(1u,&target,FALSE,nullptr);
                D3D12_VIEWPORT viewport{0.0f,0.0f,
                    static_cast<float>(DrawableWidth),
                    static_cast<float>(DrawableHeight),0.0f,1.0f};
                D3D12_RECT scissor{0,0,static_cast<LONG>(DrawableWidth),
                    static_cast<LONG>(DrawableHeight)};
                Commands->RSSetViewports(1u,&viewport);
                Commands->RSSetScissorRects(1u,&scissor);
                ID3D12DescriptorHeap* heaps[]{SRVs.Heap(),Samplers.Heap()};
                Commands->SetDescriptorHeaps(2u,heaps);
                Commands->SetGraphicsRootSignature(RootSignature.Get());
                Commands->SetPipelineState(PresentPipeline.Get());
                Commands->SetGraphicsRootDescriptorTable(2u,
                    SRVs.GPU(ViewportSRV));
                Commands->SetGraphicsRootDescriptorTable(9u,
                    Samplers.GPU(White.Sampler));
                Commands->IASetPrimitiveTopology(
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                Commands->DrawInstanced(3u,1u,0u,0u);
                barrier=Transition(back.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PRESENT);
                Commands->ResourceBarrier(1u,&barrier);
            }
        }

        [[nodiscard]] static DXGI_FORMAT TextureFormat(
            Direct3D12TextureFormat format)
        {
            switch(format)
            {
                case Direct3D12TextureFormat::R8Linear:return DXGI_FORMAT_R8_UNORM;
                case Direct3D12TextureFormat::RG8Linear:return DXGI_FORMAT_R8G8_UNORM;
                case Direct3D12TextureFormat::RGBA8Linear:return DXGI_FORMAT_R8G8B8A8_UNORM;
                case Direct3D12TextureFormat::RGBA8SRGB:return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                case Direct3D12TextureFormat::RGBA16Float:return DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            throw std::invalid_argument("Unknown Direct3D 12 texture format.");
        }

        [[nodiscard]] static UINT BytesPerPixel(Direct3D12TextureFormat format)
        {
            switch(format)
            {
                case Direct3D12TextureFormat::R8Linear:return 1u;
                case Direct3D12TextureFormat::RG8Linear:return 2u;
                case Direct3D12TextureFormat::RGBA8Linear:
                case Direct3D12TextureFormat::RGBA8SRGB:return 4u;
                case Direct3D12TextureFormat::RGBA16Float:return 8u;
            }
            throw std::invalid_argument("Unknown Direct3D 12 texture format.");
        }

        [[nodiscard]] static D3D12_TEXTURE_ADDRESS_MODE Address(
            Direct3D12AddressMode mode)
        {
            switch(mode)
            {
                case Direct3D12AddressMode::Repeat:return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                case Direct3D12AddressMode::Clamp:return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                case Direct3D12AddressMode::Mirror:return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            }
            return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        }

        [[nodiscard]] GpuTexture UploadTexture(
            const Direct3D12TextureUpload& upload)
        {
            GpuTexture result;
            const DXGI_FORMAT format=TextureFormat(upload.Format);
            result.Resource=Texture2D(upload.Width,upload.Height,format,
                D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,static_cast<std::uint16_t>(upload.MipLevels));
            const auto desc=result.Resource->GetDesc();
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(upload.MipLevels);
            std::vector<UINT> rows(upload.MipLevels);
            std::vector<UINT64> rowSizes(upload.MipLevels);
            UINT64 required=0u;
            Device->GetCopyableFootprints(&desc,0u,upload.MipLevels,0u,
                footprints.data(),rows.data(),rowSizes.data(),&required);
            auto staging=CreateBuffer(required,D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ);
            std::byte* mapped=nullptr;
            Require(staging->Map(0u,nullptr,reinterpret_cast<void**>(&mapped)),
                "Direct3D 12 texture upload mapping");
            std::size_t sourceOffset=0u;
            const UINT bpp=BytesPerPixel(upload.Format);
            for(UINT level=0u;level<upload.MipLevels;++level)
            {
                const UINT width=std::max(1u,upload.Width>>level);
                const UINT height=std::max(1u,upload.Height>>level);
                const std::size_t sourcePitch=static_cast<std::size_t>(width)*bpp;
                const std::size_t levelBytes=sourcePitch*height;
                if(sourceOffset+levelBytes>upload.ByteCount)
                    throw std::invalid_argument("Direct3D 12 texture mip payload is truncated.");
                std::byte* destination=mapped+footprints[level].Offset;
                for(UINT row=0u;row<height;++row)
                    std::memcpy(destination+static_cast<std::size_t>(row)*
                        footprints[level].Footprint.RowPitch,
                        upload.Bytes+sourceOffset+static_cast<std::size_t>(row)*
                        sourcePitch,sourcePitch);
                sourceOffset+=levelBytes;
            }
            staging->Unmap(0u,nullptr);
            if(sourceOffset!=upload.ByteCount)
                throw std::invalid_argument("Direct3D 12 texture payload has trailing bytes.");
            BeginCommands();
            for(UINT level=0u;level<upload.MipLevels;++level)
            {
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource=result.Resource.Get();
                destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.SubresourceIndex=level;
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource=staging.Get();
                source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint=footprints[level];
                Commands->CopyTextureRegion(&destination,0u,0u,0u,&source,nullptr);
            }
            auto barrier=Transition(result.Resource.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Commands->ResourceBarrier(1u,&barrier);
            ExecuteCommands();
            result.SRV=SRVs.Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format=format;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels=upload.MipLevels;
            Device->CreateShaderResourceView(result.Resource.Get(),&srv,
                SRVs.CPU(result.SRV));
            result.Sampler=Samplers.Allocate();
            D3D12_SAMPLER_DESC sampler{};
            sampler.Filter=upload.MinFilter==Direct3D12Filter::Linear||
                upload.MagFilter==Direct3D12Filter::Linear?
                D3D12_FILTER_MIN_MAG_MIP_LINEAR:D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU=Address(upload.AddressU);
            sampler.AddressV=Address(upload.AddressV);
            sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD=D3D12_FLOAT32_MAX;
            Device->CreateSampler(&sampler,Samplers.CPU(result.Sampler));
            return result;
        }

        [[nodiscard]] GpuTexture SolidTexture(std::uint8_t r,std::uint8_t g,
            std::uint8_t b,std::uint8_t a)
        {
            const std::array<std::uint8_t,4> pixels{r,g,b,a};
            const Direct3D12TextureUpload upload{1u,1u,1u,
                Direct3D12TextureFormat::RGBA8Linear,
                Direct3D12AddressMode::Clamp,Direct3D12AddressMode::Clamp,
                Direct3D12Filter::Linear,Direct3D12Filter::Linear,
                reinterpret_cast<const std::byte*>(pixels.data()),pixels.size()};
            return UploadTexture(upload);
        }

        [[nodiscard]] std::vector<std::byte> ReadTexture(ID3D12Resource* texture,
            DXGI_FORMAT format, UINT x, UINT y, UINT width, UINT height,
            D3D12_RESOURCE_STATES beforeState)
        {
            D3D12_RESOURCE_DESC source=texture->GetDesc();
            source.Format=format;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT rows=0u;
            UINT64 rowSize=0u,required=0u;
            Device->GetCopyableFootprints(&source,0u,1u,0u,&footprint,&rows,
                &rowSize,&required);
            auto readback=CreateBuffer(required,D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_STATE_COPY_DEST);
            BeginCommands();
            if(beforeState!=D3D12_RESOURCE_STATE_COPY_SOURCE)
            {
                auto barrier=Transition(texture,beforeState,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
                Commands->ResourceBarrier(1u,&barrier);
            }
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource=readback.Get();
            destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint=footprint;
            D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
            sourceLocation.pResource=texture;
            sourceLocation.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box{x,y,0u,x+width,y+height,1u};
            Commands->CopyTextureRegion(&destination,0u,0u,0u,&sourceLocation,&box);
            if(beforeState!=D3D12_RESOURCE_STATE_COPY_SOURCE)
            {
                auto barrier=Transition(texture,D3D12_RESOURCE_STATE_COPY_SOURCE,
                    beforeState);
                Commands->ResourceBarrier(1u,&barrier);
            }
            ExecuteCommands();
            std::byte* mapped=nullptr;
            Require(readback->Map(0u,nullptr,reinterpret_cast<void**>(&mapped)),
                "Direct3D 12 readback mapping");
            const UINT bpp=format==ObjectIDFormat?4u:4u;
            std::vector<std::byte> result(static_cast<std::size_t>(width)*height*bpp);
            for(UINT row=0u;row<height;++row)
                std::memcpy(result.data()+static_cast<std::size_t>(row)*width*bpp,
                    mapped+static_cast<std::size_t>(row)*footprint.Footprint.RowPitch,
                    static_cast<std::size_t>(width)*bpp);
            readback->Unmap(0u,nullptr);
            return result;
        }
    };

    Direct3D12Backend::Direct3D12Backend(GLFWwindow* window,std::uint32_t width,
        std::uint32_t height):m_Impl(std::make_unique<Impl>(window,width,height)){}
    Direct3D12Backend::~Direct3D12Backend()=default;

    std::uint64_t Direct3D12Backend::CreateMesh(
        std::span<const Direct3D12Vertex> vertices,
        std::span<const std::uint32_t> indices)
    {
        if(vertices.empty()||indices.empty())
            throw std::invalid_argument("Direct3D 12 mesh upload requires vertices and indices.");
        const std::uint64_t handle=m_Impl->NextMesh++;
        GpuMesh mesh;
        mesh.Vertices=m_Impl->UploadBuffer(vertices.data(),vertices.size_bytes(),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        mesh.Indices=m_Impl->UploadBuffer(indices.data(),indices.size_bytes(),
            D3D12_RESOURCE_STATE_INDEX_BUFFER);
        mesh.VertexView={mesh.Vertices->GetGPUVirtualAddress(),
            static_cast<UINT>(vertices.size_bytes()),sizeof(Direct3D12Vertex)};
        mesh.IndexView={mesh.Indices->GetGPUVirtualAddress(),
            static_cast<UINT>(indices.size_bytes()),DXGI_FORMAT_R32_UINT};
        mesh.IndexCount=static_cast<UINT>(indices.size());
        m_Impl->Meshes.emplace(handle,std::move(mesh));
        return handle;
    }

    void Direct3D12Backend::DestroyMesh(std::uint64_t handle)
    {
        m_Impl->WaitIdle();
        if(m_Impl->Meshes.erase(handle)!=1u)
            throw std::out_of_range("Cannot destroy an unknown Direct3D 12 mesh handle.");
    }

    std::uint64_t Direct3D12Backend::CreateTexture(
        const Direct3D12TextureUpload& upload)
    {
        if(upload.Width==0u||upload.Height==0u||upload.MipLevels==0u||
            upload.Bytes==nullptr)
            throw std::invalid_argument("Direct3D 12 texture upload is incomplete.");
        const auto handle=m_Impl->NextTexture++;
        m_Impl->Textures.emplace(handle,m_Impl->UploadTexture(upload));
        return handle;
    }

    void Direct3D12Backend::DestroyTexture(std::uint64_t handle)
    {
        m_Impl->WaitIdle();
        const auto found=m_Impl->Textures.find(handle);
        if(found==m_Impl->Textures.end())
            throw std::out_of_range("Cannot destroy an unknown Direct3D 12 texture handle.");
        m_Impl->SRVs.Free(found->second.SRV);
        m_Impl->Samplers.Free(found->second.Sampler);
        m_Impl->Textures.erase(found);
    }

    void Direct3D12Backend::Resize(std::uint32_t width,std::uint32_t height)
    {
        if(width==0u||height==0u)
            throw std::invalid_argument("Direct3D 12 viewport extent must be non-zero.");
        m_Impl->BuildTargets(width,height);
    }

    void Direct3D12Backend::SetDrawableSize(std::uint32_t width,
        std::uint32_t height)
    {
        if(width>0u&&height>0u)m_Impl->ResizeSwapchain(width,height);
    }

    void Direct3D12Backend::Draw(const Direct3D12Frame& frame)
    {
        m_Impl->BeginCommands();
        m_Impl->EncodeScene(frame);
        m_Impl->Present();
        Require(m_Impl->Commands->Close(),"Direct3D 12 frame command-list close");
        ID3D12CommandList* lists[]{m_Impl->Commands.Get()};
        m_Impl->Queue->ExecuteCommandLists(1u,lists);
        Require(m_Impl->Swapchain->Present(1u,0u),"Direct3D 12 presentation");
        m_Impl->WaitIdle();
    }

    std::uint32_t Direct3D12Backend::Pick(std::uint32_t x,std::uint32_t y)
    {
        if(x>=m_Impl->ViewportWidth||y>=m_Impl->ViewportHeight)
            throw std::out_of_range("Direct3D 12 viewport pick is outside the image.");
        const auto bytes=m_Impl->ReadTexture(m_Impl->ViewportID.Get(),ObjectIDFormat,
            x,y,1u,1u,D3D12_RESOURCE_STATE_COPY_SOURCE);
        std::uint32_t result=0u;
        std::memcpy(&result,bytes.data(),sizeof(result));
        return result;
    }

    Direct3D12Capture Direct3D12Backend::Capture()
    {
        const auto bytes=m_Impl->ReadTexture(m_Impl->ViewportColor.Get(),ColorFormat,
            0u,0u,m_Impl->ViewportWidth,m_Impl->ViewportHeight,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Direct3D12Capture result;
        result.Width=m_Impl->ViewportWidth;
        result.Height=m_Impl->ViewportHeight;
        result.ByteCount=bytes.size();
        result.RGBA=std::make_unique<std::uint8_t[]>(result.ByteCount);
        std::memcpy(result.RGBA.get(),bytes.data(),result.ByteCount);
        return result;
    }

    Direct3D12Descriptor Direct3D12Backend::ViewportDescriptor()const noexcept
    {
        const auto cpu=m_Impl->SRVs.CPU(m_Impl->ViewportSRV);
        const auto gpu=m_Impl->SRVs.GPU(m_Impl->ViewportSRV);
        return {cpu.ptr,gpu.ptr};
    }
    void* Direct3D12Backend::Device()const noexcept{return m_Impl->Device.Get();}
    void* Direct3D12Backend::CommandQueue()const noexcept{return m_Impl->Queue.Get();}
    void* Direct3D12Backend::ShaderResourceHeap()const noexcept{return m_Impl->SRVs.Heap();}
    Direct3D12Descriptor Direct3D12Backend::AllocateToolingDescriptor()
    {
        const UINT index=m_Impl->SRVs.Allocate();
        return {m_Impl->SRVs.CPU(index).ptr,m_Impl->SRVs.GPU(index).ptr};
    }
    void Direct3D12Backend::FreeToolingDescriptor(Direct3D12Descriptor descriptor)
    {
        const auto start=m_Impl->SRVs.CPU(0u).ptr;
        if(descriptor.CPU<start)
            throw std::invalid_argument("Direct3D 12 tooling descriptor is outside the heap.");
        const SIZE_T delta=descriptor.CPU-start;
        const UINT increment=m_Impl->Device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if(delta%increment!=0u)
            throw std::invalid_argument("Direct3D 12 tooling descriptor is misaligned.");
        m_Impl->SRVs.Free(static_cast<UINT>(delta/increment));
    }
    std::uint32_t Direct3D12Backend::Width()const noexcept{return m_Impl->ViewportWidth;}
    std::uint32_t Direct3D12Backend::Height()const noexcept{return m_Impl->ViewportHeight;}
    void Direct3D12Backend::SetOverlayRecorder(Direct3D12OverlayRecorder recorder)
    {m_Impl->Overlay=std::move(recorder);}
}

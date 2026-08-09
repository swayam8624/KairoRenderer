#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "MetalBackend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kairo::renderer::detail
{
    static_assert(sizeof(MetalVertex) == 44u);
    static_assert(sizeof(MetalDebugVertex) == 28u);
    static_assert(sizeof(MetalLight) == 80u);

    namespace
    {
        constexpr MTLPixelFormat ViewportColorFormat = MTLPixelFormatRGBA16Float;
        constexpr MTLPixelFormat ViewportIDFormat = MTLPixelFormatR32Uint;
        constexpr MTLPixelFormat DepthFormat = MTLPixelFormatDepth32Float;
        constexpr std::uint32_t ShadowResolution = 2048u;

        struct GpuMesh final
        {
            id<MTLBuffer> Vertices = nil;
            id<MTLBuffer> Indices = nil;
            NSUInteger IndexCount = 0u;
        };

        struct GpuTexture final
        {
            id<MTLTexture> Texture = nil;
            id<MTLSamplerState> Sampler = nil;
        };

        struct alignas(16) FrameUniforms final
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
            MetalLight Lights[16]{};
        };

        struct alignas(16) DrawUniforms final
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

        [[nodiscard]] std::string ErrorText(NSString* context, NSError* error)
        {
            std::string message([context UTF8String]);
            if (error != nil)
            {
                message += ": ";
                message += [[error localizedDescription] UTF8String];
            }
            return message;
        }

        void Transpose(const float* source, float* destination, std::size_t size)
        {
            for (std::size_t row = 0u; row < size; ++row)
                for (std::size_t column = 0u; column < size; ++column)
                    destination[column * size + row] = source[row * size + column];
        }

        [[nodiscard]] float HalfToFloat(std::uint16_t half) noexcept
        {
            const std::uint32_t sign = (half & 0x8000u) << 16u;
            std::uint32_t exponent = (half >> 10u) & 0x1fu;
            std::uint32_t mantissa = half & 0x03ffu;
            std::uint32_t bits = 0u;
            if (exponent == 0u)
            {
                if (mantissa == 0u) bits = sign;
                else
                {
                    exponent = 1u;
                    while ((mantissa & 0x0400u) == 0u)
                    {
                        mantissa <<= 1u;
                        --exponent;
                    }
                    mantissa &= 0x03ffu;
                    bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
                }
            }
            else if (exponent == 31u)
                bits = sign | 0x7f800000u | (mantissa << 13u);
            else bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        [[nodiscard]] NSUInteger BytesPerPixel(MetalTextureFormat format)
        {
            switch (format)
            {
                case MetalTextureFormat::R8Linear: return 1u;
                case MetalTextureFormat::RG8Linear: return 2u;
                case MetalTextureFormat::RGBA8Linear:
                case MetalTextureFormat::RGBA8SRGB: return 4u;
                case MetalTextureFormat::RGBA16Float: return 8u;
            }
            throw std::invalid_argument("Unknown Metal texture format.");
        }

        [[nodiscard]] MTLPixelFormat PixelFormat(MetalTextureFormat format)
        {
            switch (format)
            {
                case MetalTextureFormat::R8Linear: return MTLPixelFormatR8Unorm;
                case MetalTextureFormat::RG8Linear: return MTLPixelFormatRG8Unorm;
                case MetalTextureFormat::RGBA8Linear: return MTLPixelFormatRGBA8Unorm;
                case MetalTextureFormat::RGBA8SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
                case MetalTextureFormat::RGBA16Float: return MTLPixelFormatRGBA16Float;
            }
            throw std::invalid_argument("Unknown Metal texture format.");
        }

        [[nodiscard]] MTLSamplerAddressMode AddressMode(MetalAddressMode mode)
        {
            switch (mode)
            {
                case MetalAddressMode::Repeat: return MTLSamplerAddressModeRepeat;
                case MetalAddressMode::Clamp: return MTLSamplerAddressModeClampToEdge;
                case MetalAddressMode::Mirror: return MTLSamplerAddressModeMirrorRepeat;
            }
            return MTLSamplerAddressModeClampToEdge;
        }

        constexpr const char* ShaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { packed_float3 position; packed_float3 color; packed_float3 normal; packed_float2 uv; };
struct VertexOut {
    float4 position [[position]]; float3 world; float3 color; float3 normal;
    float2 uv; float4 shadow;
};
struct Light { float4 positionType; float4 directionRange; float4 colorIntensity; float4 spotArea; uint castsShadows; };
struct Frame {
    float4x4 view; float4x4 projection; float4x4 lightViewProjection;
    float4 cameraPosition; float4 ambientExposure; float4 backgroundEnvironment;
    uint lightCount; int shadowLightIndex; uint shadingMode; uint shadowsEnabled;
    float shadowStrength; float receiverBias; float shadowTexel; float padding;
    Light lights[16];
};
struct Draw {
    float4x4 model; float4 normal0; float4 normal1; float4 normal2;
    float4 baseColor; float4 emissiveNormalScale; float4 factors;
    uint alphaMode; uint objectID; uint receiveShadows; uint padding;
};
struct FragmentOut { float4 color [[color(0)]]; uint objectID [[color(1)]]; };

vertex VertexOut mesh_vertex(uint id [[vertex_id]], const device VertexIn* vertices [[buffer(0)]],
    constant Frame& frame [[buffer(1)]], constant Draw& draw [[buffer(2)]]) {
    VertexIn input = vertices[id]; VertexOut output;
    float4 world = draw.model * float4(float3(input.position), 1.0);
    output.position = frame.projection * frame.view * world; output.world = world.xyz;
    float3x3 normalMatrix = float3x3(draw.normal0.xyz, draw.normal1.xyz, draw.normal2.xyz);
    output.normal = normalize(normalMatrix * float3(input.normal));
    output.color = float3(input.color); output.uv = float2(input.uv);
    output.shadow = frame.lightViewProjection * world; return output;
}

float3 fresnel(float cosine, float3 f0) { return f0 + (1.0 - f0) * pow(clamp(1.0-cosine,0.0,1.0),5.0); }
float2 equirectangular_uv(float3 direction) {
    float3 n=normalize(direction);
    return float2(atan2(n.z,n.x)/(2.0*M_PI_F)+0.5,
        asin(clamp(n.y,-1.0,1.0))/M_PI_F+0.5);
}
fragment FragmentOut mesh_fragment(VertexOut in [[stage_in]], constant Frame& frame [[buffer(1)]],
    constant Draw& draw [[buffer(2)]], texture2d<float> baseTex [[texture(0)]],
    texture2d<float> normalTex [[texture(1)]], texture2d<float> mrTex [[texture(2)]],
    texture2d<float> emissiveTex [[texture(3)]], texture2d<float> occlusionTex [[texture(4)]],
    texture2d<float> environmentTex [[texture(5)]], depth2d<float> shadowTex [[texture(6)]],
    sampler baseSampler [[sampler(0)]], sampler normalSampler [[sampler(1)]],
    sampler mrSampler [[sampler(2)]], sampler emissiveSampler [[sampler(3)]],
    sampler occlusionSampler [[sampler(4)]], sampler environmentSampler [[sampler(5)]],
    sampler shadowSampler [[sampler(6)]]) {
    float4 base = baseTex.sample(baseSampler, in.uv) * draw.baseColor * float4(in.color,1.0);
    if (draw.alphaMode == 2 && base.a < draw.factors.w) discard_fragment();
    float4 mr = mrTex.sample(mrSampler, in.uv); float metallic = clamp(draw.factors.x * mr.b,0.0,1.0);
    float roughness = clamp(draw.factors.y * mr.g,0.045,1.0);
    float ao = clamp(draw.factors.z * occlusionTex.sample(occlusionSampler,in.uv).r,0.0,1.0);
    float3 N = normalize(in.normal); float3 V = normalize(frame.cameraPosition.xyz-in.world);
    float3 tangentNormal = normalTex.sample(normalSampler,in.uv).xyz*2.0-1.0;
    float3 dpdx=dfdx(in.world), dpdy=dfdy(in.world);
    float2 duvdx=dfdx(in.uv), duvdy=dfdy(in.uv);
    float determinant=duvdx.x*duvdy.y-duvdx.y*duvdy.x;
    if(abs(determinant)>1.0e-8){
        float3 tangent=normalize((dpdx*duvdy.y-dpdy*duvdx.y)/determinant);
        float3 bitangent=normalize(cross(N,tangent))*sign(determinant);
        tangentNormal.xy*=draw.emissiveNormalScale.w;
        N=normalize(float3x3(tangent,bitangent,N)*tangentNormal);
    }
    if (frame.shadingMode == 2) { FragmentOut o; o.color=float4(N*0.5+0.5,1); o.objectID=draw.objectID; return o; }
    float3 f0 = mix(float3(0.04),base.rgb,metallic); float3 direct=0.0;
    for (uint index=0; index<frame.lightCount; ++index) {
        Light light=frame.lights[index]; uint type=(uint)light.positionType.w;
        float3 L; float attenuation=1.0;
        if (type==1) L=normalize(light.directionRange.xyz);
        else { float3 delta=light.positionType.xyz-in.world; float distance=length(delta); L=delta/max(distance,0.0001);
            float range=max(light.directionRange.w,0.0001); float falloff=clamp(1.0-distance/range,0.0,1.0); attenuation=falloff*falloff/max(distance*distance,0.01);
            if(type==3){float cone=dot(normalize(-light.directionRange.xyz),L); attenuation*=smoothstep(light.spotArea.y,light.spotArea.x,cone);}
            else if(type==4){attenuation*=max(dot(-L,normalize(light.directionRange.xyz)),0.0)*light.spotArea.z*light.spotArea.w;} }
        float NdotL=max(dot(N,L),0.0); float3 H=normalize(V+L); float NdotV=max(dot(N,V),0.0001); float NdotH=max(dot(N,H),0.0); float VdotH=max(dot(V,H),0.0);
        float alpha=roughness*roughness; float a2=alpha*alpha; float denominator=NdotH*NdotH*(a2-1.0)+1.0;
        float D=a2/max(M_PI_F*denominator*denominator,0.0001); float k=(roughness+1.0); k=k*k/8.0;
        float G=(NdotL/(NdotL*(1.0-k)+k))*(NdotV/(NdotV*(1.0-k)+k)); float3 F=fresnel(VdotH,f0);
        float3 specular=D*G*F/max(4.0*NdotL*NdotV,0.0001); float3 diffuse=(1.0-F)*(1.0-metallic)*base.rgb/M_PI_F;
        float visibility=1.0;
        if(frame.shadowsEnabled!=0 && draw.receiveShadows!=0 && int(index)==frame.shadowLightIndex){
            float3 sc=in.shadow.xyz/in.shadow.w;
            sc.x=sc.x*0.5+0.5;
            sc.y=0.5-sc.y*0.5;
            if(all(sc.xy>=0.0)&&all(sc.xy<=1.0)&&sc.z>=0.0&&sc.z<=1.0){float sum=0.0;
                for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)sum+=shadowTex.sample_compare(shadowSampler,sc.xy+float2(x,y)*frame.shadowTexel,sc.z-frame.receiverBias);
                visibility=mix(1.0,sum/9.0,frame.shadowStrength);}}
        direct+=(diffuse+specular)*light.colorIntensity.rgb*light.colorIntensity.w*attenuation*NdotL*visibility;
    }
    float3 emissive=draw.emissiveNormalScale.xyz*emissiveTex.sample(emissiveSampler,in.uv).rgb;
    float3 reflected=reflect(-V,N);
    float environmentLevel=roughness*float(max(environmentTex.get_num_mip_levels(),1u)-1u);
    float3 environment=environmentTex.sample(environmentSampler,
        equirectangular_uv(reflected),level(environmentLevel)).rgb*
        frame.backgroundEnvironment.w;
    float NdotV=max(dot(N,V),0.0);
    float3 ambient=(frame.ambientExposure.rgb*base.rgb+
        environment*base.rgb*(1.0-metallic)+environment*fresnel(NdotV,f0)*
        (1.0-roughness*0.65))*ao;
    float3 color = frame.shadingMode==1 ? base.rgb+emissive :
        (frame.shadingMode==3 ? direct : ambient+direct+emissive);
    color*=exp2(frame.ambientExposure.w); color=color/(color+1.0);
    FragmentOut output; output.color=float4(color,draw.alphaMode==3?base.a:1.0); output.objectID=draw.objectID; return output;
}

vertex float4 shadow_vertex(uint id [[vertex_id]], const device VertexIn* vertices [[buffer(0)]],
    constant Frame& frame [[buffer(1)]], constant Draw& draw [[buffer(2)]]) {
    return frame.lightViewProjection * draw.model *
        float4(float3(vertices[id].position),1.0);
}
struct DebugVertex { packed_float3 position; packed_float4 color; };
struct DebugOut { float4 position [[position]]; float4 color; };
vertex DebugOut debug_vertex(uint id [[vertex_id]], const device DebugVertex* vertices [[buffer(0)]], constant Frame& frame [[buffer(1)]]) {
    DebugOut o; o.position=frame.projection*frame.view*
        float4(float3(vertices[id].position),1.0);
    o.color=float4(vertices[id].color); return o;
}
fragment float4 debug_fragment(DebugOut in [[stage_in]]) { return in.color; }
struct FullscreenOut { float4 position [[position]]; float2 uv; };
vertex FullscreenOut fullscreen_vertex(uint id [[vertex_id]]) { float2 p=float2((id<<1)&2,id&2); FullscreenOut o; o.position=float4(p*2.0-1.0,0,1); o.uv=float2(p.x,1.0-p.y); return o; }
fragment float4 fullscreen_fragment(FullscreenOut in [[stage_in]], texture2d<float> source [[texture(0)]]) { constexpr sampler s(filter::linear,address::clamp_to_edge); return source.sample(s,in.uv); }
)metal";
    }

    class MetalBackend::Impl final
    {
    public:
        id<MTLDevice> Device = nil;
        id<MTLCommandQueue> Queue = nil;
        CAMetalLayer* Layer = nil;
        id<MTLRenderPipelineState> MeshPipeline = nil;
        id<MTLRenderPipelineState> BlendPipeline = nil;
        id<MTLRenderPipelineState> ShadowPipeline = nil;
        id<MTLRenderPipelineState> DebugPipeline = nil;
        id<MTLRenderPipelineState> PresentPipeline = nil;
        id<MTLDepthStencilState> DepthState = nil;
        id<MTLDepthStencilState> NoWriteDepthState = nil;
        id<MTLDepthStencilState> ShadowDepthState = nil;
        id<MTLSamplerState> ShadowSampler = nil;
        id<MTLTexture> ViewportColor = nil;
        id<MTLTexture> ViewportID = nil;
        id<MTLTexture> ViewportDepth = nil;
        id<MTLTexture> ShadowDepth = nil;
        id<MTLCommandBuffer> LastCommand = nil;
        std::unordered_map<std::uint64_t, GpuMesh> Meshes;
        std::unordered_map<std::uint64_t, GpuTexture> Textures;
        GpuTexture White;
        GpuTexture Normal;
        GpuTexture Black;
        std::uint64_t NextMesh = 1u;
        std::uint64_t NextTexture = 1u;
        std::uint32_t ViewportWidth = 1u;
        std::uint32_t ViewportHeight = 1u;
        MetalOverlayRecorder Overlay;

        Impl(GLFWwindow* window, std::uint32_t width, std::uint32_t height)
            : ViewportWidth(width), ViewportHeight(height)
        {
            @autoreleasepool
            {
                Device = MTLCreateSystemDefaultDevice();
                if (Device == nil) throw std::runtime_error("No Metal device is available.");
                Queue = [Device newCommandQueue];
                if (Queue == nil) throw std::runtime_error("Metal command queue creation failed.");
                NSWindow* cocoaWindow = glfwGetCocoaWindow(window);
                if (cocoaWindow == nil) throw std::runtime_error("GLFW did not expose a Cocoa window for Metal.");
                Layer = [CAMetalLayer layer];
                Layer.device = Device;
                Layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
                Layer.framebufferOnly = YES;
                Layer.displaySyncEnabled = YES;
                cocoaWindow.contentView.wantsLayer = YES;
                cocoaWindow.contentView.layer = Layer;
                BuildPipelines();
                BuildTargets(width, height);
                White = SolidTexture(255u,255u,255u,255u);
                Normal = SolidTexture(128u,128u,255u,255u);
                Black = SolidTexture(0u,0u,0u,255u);
            }
        }

        ~Impl()
        {
            // Metal retains encoded resources, but waiting here prevents the
            // native objects owned by this backend from disappearing while
            // the final command buffer is still executing.
            if (LastCommand != nil) [LastCommand waitUntilCompleted];
        }

        void WaitForLastCommand()
        {
            if (LastCommand == nil) return;
            [LastCommand waitUntilCompleted];
            if (LastCommand.status == MTLCommandBufferStatusError)
                throw std::runtime_error(ErrorText(
                    @"Metal command-buffer execution failed", LastCommand.error));
            LastCommand = nil;
        }

        void BuildPipelines()
        {
            NSError* error = nil;
            id<MTLLibrary> library = [Device newLibraryWithSource:
                [NSString stringWithUTF8String:ShaderSource] options:nil error:&error];
            if (library == nil) throw std::runtime_error(ErrorText(@"Metal shader compilation failed", error));
            auto function = [&](NSString* name)
            {
                id<MTLFunction> result = [library newFunctionWithName:name];
                if (result == nil) throw std::runtime_error("Metal shader entry point is missing.");
                return result;
            };
            MTLRenderPipelineDescriptor* mesh = [MTLRenderPipelineDescriptor new];
            mesh.vertexFunction = function(@"mesh_vertex"); mesh.fragmentFunction = function(@"mesh_fragment");
            mesh.colorAttachments[0].pixelFormat = ViewportColorFormat;
            mesh.colorAttachments[1].pixelFormat = ViewportIDFormat;
            mesh.depthAttachmentPixelFormat = DepthFormat;
            MeshPipeline = [Device newRenderPipelineStateWithDescriptor:mesh error:&error];
            if (MeshPipeline == nil) throw std::runtime_error(ErrorText(@"Metal mesh pipeline creation failed", error));
            MTLRenderPipelineDescriptor* blend = [mesh copy];
            blend.colorAttachments[0].blendingEnabled = YES;
            blend.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            blend.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            blend.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            blend.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            BlendPipeline = [Device newRenderPipelineStateWithDescriptor:blend error:&error];
            if (BlendPipeline == nil) throw std::runtime_error(ErrorText(@"Metal blend pipeline creation failed", error));
            MTLRenderPipelineDescriptor* shadow = [MTLRenderPipelineDescriptor new];
            shadow.vertexFunction = function(@"shadow_vertex"); shadow.depthAttachmentPixelFormat = DepthFormat;
            ShadowPipeline = [Device newRenderPipelineStateWithDescriptor:shadow error:&error];
            if (ShadowPipeline == nil) throw std::runtime_error(ErrorText(@"Metal shadow pipeline creation failed", error));
            MTLRenderPipelineDescriptor* debug = [MTLRenderPipelineDescriptor new];
            debug.vertexFunction=function(@"debug_vertex"); debug.fragmentFunction=function(@"debug_fragment");
            debug.colorAttachments[0].pixelFormat=ViewportColorFormat; debug.depthAttachmentPixelFormat=DepthFormat;
            DebugPipeline=[Device newRenderPipelineStateWithDescriptor:debug error:&error];
            if(DebugPipeline==nil) throw std::runtime_error(ErrorText(@"Metal debug pipeline creation failed",error));
            MTLRenderPipelineDescriptor* present=[MTLRenderPipelineDescriptor new];
            present.vertexFunction=function(@"fullscreen_vertex"); present.fragmentFunction=function(@"fullscreen_fragment");
            present.colorAttachments[0].pixelFormat=Layer.pixelFormat;
            PresentPipeline=[Device newRenderPipelineStateWithDescriptor:present error:&error];
            if(PresentPipeline==nil) throw std::runtime_error(ErrorText(@"Metal presentation pipeline creation failed",error));
            MTLDepthStencilDescriptor* depth=[MTLDepthStencilDescriptor new]; depth.depthCompareFunction=MTLCompareFunctionLess; depth.depthWriteEnabled=YES;
            DepthState=[Device newDepthStencilStateWithDescriptor:depth];
            ShadowDepthState=DepthState;
            depth.depthWriteEnabled=NO;
            NoWriteDepthState=[Device newDepthStencilStateWithDescriptor:depth];
            MTLSamplerDescriptor* shadowSampler=[MTLSamplerDescriptor new];
            shadowSampler.compareFunction=MTLCompareFunctionLessEqual;
            shadowSampler.minFilter=MTLSamplerMinMagFilterLinear;
            shadowSampler.magFilter=MTLSamplerMinMagFilterLinear;
            shadowSampler.sAddressMode=MTLSamplerAddressModeClampToEdge;
            shadowSampler.tAddressMode=MTLSamplerAddressModeClampToEdge;
            ShadowSampler=[Device newSamplerStateWithDescriptor:shadowSampler];
            if(DepthState==nil||NoWriteDepthState==nil||ShadowSampler==nil)
                throw std::runtime_error("Metal depth/sampler state creation failed.");
        }

        void BuildTargets(std::uint32_t width, std::uint32_t height)
        {
            WaitForLastCommand();
            ViewportWidth=width; ViewportHeight=height;
            auto texture = [&](MTLPixelFormat format, MTLTextureUsage usage, std::uint32_t w, std::uint32_t h)
            {
                MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
                desc.usage=usage; desc.storageMode=MTLStorageModeShared;
                id<MTLTexture> result=[Device newTextureWithDescriptor:desc];
                if(result==nil) throw std::runtime_error("Metal render-target allocation failed.");
                return result;
            };
            ViewportColor=texture(ViewportColorFormat,MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead,width,height);
            ViewportID=texture(ViewportIDFormat,MTLTextureUsageRenderTarget,width,height);
            ViewportDepth=texture(DepthFormat,MTLTextureUsageRenderTarget,width,height);
            if(ShadowDepth==nil) ShadowDepth=texture(DepthFormat,MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead,ShadowResolution,ShadowResolution);
            Layer.drawableSize=CGSizeMake(width,height);
        }

        GpuTexture SolidTexture(std::uint8_t r,std::uint8_t g,std::uint8_t b,std::uint8_t a)
        {
            const std::array<std::uint8_t,4> bytes{r,g,b,a};
            MetalTextureUpload upload{1u,1u,1u,MetalTextureFormat::RGBA8Linear,
                MetalAddressMode::Clamp,MetalAddressMode::Clamp,MetalFilter::Linear,MetalFilter::Linear,
                reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()};
            return UploadTexture(upload);
        }

        GpuTexture UploadTexture(const MetalTextureUpload& upload)
        {
            MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:PixelFormat(upload.Format)
                width:upload.Width height:upload.Height mipmapped:upload.MipLevels>1u];
            desc.mipmapLevelCount=upload.MipLevels; desc.usage=MTLTextureUsageShaderRead; desc.storageMode=MTLStorageModeShared;
            GpuTexture result; result.Texture=[Device newTextureWithDescriptor:desc];
            if(result.Texture==nil) throw std::runtime_error("Metal texture allocation failed.");
            const NSUInteger bpp=BytesPerPixel(upload.Format); std::size_t offset=0u;
            for(std::uint32_t level=0;level<upload.MipLevels;++level){
                const std::uint32_t w=std::max(1u,upload.Width>>level), h=std::max(1u,upload.Height>>level);
                const std::size_t levelBytes=static_cast<std::size_t>(w)*h*bpp;
                if(offset+levelBytes>upload.ByteCount) throw std::invalid_argument("Metal texture mip payload is truncated.");
                [result.Texture replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:level
                    withBytes:upload.Bytes+offset bytesPerRow:static_cast<NSUInteger>(w)*bpp]; offset+=levelBytes;
            }
            if(offset!=upload.ByteCount) throw std::invalid_argument("Metal texture payload has trailing bytes.");
            MTLSamplerDescriptor* sampler=[MTLSamplerDescriptor new];
            sampler.sAddressMode=AddressMode(upload.AddressU); sampler.tAddressMode=AddressMode(upload.AddressV);
            sampler.minFilter=upload.MinFilter==MetalFilter::Linear?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
            sampler.magFilter=upload.MagFilter==MetalFilter::Linear?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
            sampler.mipFilter=upload.MipLevels>1u?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNotMipmapped;
            result.Sampler=[Device newSamplerStateWithDescriptor:sampler];
            if(result.Sampler==nil) throw std::runtime_error("Metal sampler allocation failed.");
            return result;
        }

        const GpuTexture& TextureOr(std::uint64_t handle,const GpuTexture& fallback) const
        {
            if(handle==0u) return fallback; const auto found=Textures.find(handle);
            if(found==Textures.end()) throw std::out_of_range("Metal draw references an unknown texture handle.");
            return found->second;
        }

        static void BindTexture(id<MTLRenderCommandEncoder> encoder,NSUInteger slot,const GpuTexture& texture)
        { [encoder setFragmentTexture:texture.Texture atIndex:slot]; [encoder setFragmentSamplerState:texture.Sampler atIndex:slot]; }

        FrameUniforms MakeFrame(const MetalFrame& frame) const
        {
            FrameUniforms output; Transpose(frame.View,output.View,4u); Transpose(frame.Projection,output.Projection,4u);
            Transpose(frame.LightViewProjection,output.LightViewProjection,4u);
            std::copy_n(frame.CameraPosition,3u,output.CameraPosition);
            std::copy_n(frame.Ambient,3u,output.AmbientExposure); output.AmbientExposure[3]=frame.Exposure;
            std::copy_n(frame.Background,3u,output.BackgroundEnvironment); output.BackgroundEnvironment[3]=frame.EnvironmentIntensity;
            output.LightCount=static_cast<std::uint32_t>(std::min<std::size_t>(16u,frame.Lights.size()));
            output.ShadowLightIndex=-1; output.ShadingMode=frame.ShadingMode;
            output.ShadowStrength=frame.ShadowStrength; output.ReceiverBias=frame.ReceiverBias; output.ShadowTexel=1.0f/ShadowResolution;
            for(std::size_t i=0;i<output.LightCount;++i){output.Lights[i]=frame.Lights[i]; if(output.ShadowLightIndex<0&&frame.Lights[i].CastShadows&&static_cast<unsigned>(frame.Lights[i].PositionType[3])==1u) output.ShadowLightIndex=static_cast<int>(i);}
            output.ShadowsEnabled=frame.ShadowsEnabled&&output.ShadowLightIndex>=0?1u:0u;
            return output;
        }

        void EncodeShadow(id<MTLCommandBuffer> command,
            const MetalFrame& frame, const FrameUniforms& frameUniforms)
        {
            if (frameUniforms.ShadowsEnabled == 0u) return;
            MTLRenderPassDescriptor* pass =
                [MTLRenderPassDescriptor renderPassDescriptor];
            pass.depthAttachment.texture = ShadowDepth;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            pass.depthAttachment.clearDepth = 1.0;
            id<MTLRenderCommandEncoder> encoder =
                [command renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:ShadowPipeline];
            [encoder setDepthStencilState:ShadowDepthState];
            [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
            [encoder setCullMode:MTLCullModeBack];
            [encoder setDepthBias:frame.ConstantDepthBias
                slopeScale:frame.SlopeDepthBias clamp:0.0f];
            [encoder setVertexBytes:&frameUniforms
                length:sizeof(frameUniforms) atIndex:1];
            for (const MetalDraw& draw : frame.Draws)
            {
                if (!draw.CastShadows || draw.Material.AlphaMode == 3u) continue;
                const auto mesh = Meshes.find(draw.Mesh);
                if (mesh == Meshes.end())
                    throw std::out_of_range(
                        "Metal shadow draw references an unknown mesh handle.");
                const DrawUniforms uniforms = MakeDraw(draw);
                [encoder setVertexBuffer:mesh->second.Vertices offset:0 atIndex:0];
                [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:2];
                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:mesh->second.IndexCount indexType:MTLIndexTypeUInt32
                    indexBuffer:mesh->second.Indices indexBufferOffset:0];
            }
            [encoder endEncoding];
        }

        static DrawUniforms MakeDraw(const MetalDraw& draw)
        {
            DrawUniforms output; Transpose(draw.Model,output.Model,4u);
            output.Normal0[0]=draw.Normal[0];output.Normal0[1]=draw.Normal[3];output.Normal0[2]=draw.Normal[6];
            output.Normal1[0]=draw.Normal[1];output.Normal1[1]=draw.Normal[4];output.Normal1[2]=draw.Normal[7];
            output.Normal2[0]=draw.Normal[2];output.Normal2[1]=draw.Normal[5];output.Normal2[2]=draw.Normal[8];
            std::copy_n(draw.Material.BaseColor,4u,output.BaseColor); std::copy_n(draw.Material.Emissive,3u,output.EmissiveNormalScale);
            output.EmissiveNormalScale[3]=draw.Material.NormalScale; output.Factors[0]=draw.Material.Metallic; output.Factors[1]=draw.Material.Roughness;
            output.Factors[2]=draw.Material.AmbientOcclusion; output.Factors[3]=draw.Material.AlphaCutoff;
            output.AlphaMode=draw.Material.AlphaMode;output.ObjectID=draw.Material.ObjectID;output.ReceiveShadows=draw.Material.ReceiveShadows?1u:0u; return output;
        }

        void EncodeScene(id<MTLCommandBuffer> command,const MetalFrame& frame)
        {
            FrameUniforms frameUniforms=MakeFrame(frame);
            EncodeShadow(command, frame, frameUniforms);
            MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture=ViewportColor; pass.colorAttachments[0].loadAction=MTLLoadActionClear; pass.colorAttachments[0].storeAction=MTLStoreActionStore;
            pass.colorAttachments[0].clearColor=MTLClearColorMake(frame.Background[0],frame.Background[1],frame.Background[2],1.0);
            pass.colorAttachments[1].texture=ViewportID; pass.colorAttachments[1].loadAction=MTLLoadActionClear; pass.colorAttachments[1].storeAction=MTLStoreActionStore; pass.colorAttachments[1].clearColor=MTLClearColorMake(0,0,0,0);
            pass.depthAttachment.texture=ViewportDepth; pass.depthAttachment.loadAction=MTLLoadActionClear; pass.depthAttachment.storeAction=MTLStoreActionDontCare; pass.depthAttachment.clearDepth=1.0;
            id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:MeshPipeline];[encoder setDepthStencilState:DepthState];[encoder setCullMode:MTLCullModeBack];[encoder setFrontFacingWinding:MTLWindingCounterClockwise];
            [encoder setVertexBytes:&frameUniforms length:sizeof(frameUniforms) atIndex:1];[encoder setFragmentBytes:&frameUniforms length:sizeof(frameUniforms) atIndex:1];
            [encoder setFragmentTexture:ShadowDepth atIndex:6];[encoder setFragmentSamplerState:ShadowSampler atIndex:6];
            std::vector<const MetalDraw*> ordered;
            ordered.reserve(frame.Draws.size());
            for(const MetalDraw& draw:frame.Draws)if(draw.Material.AlphaMode!=3u)ordered.push_back(&draw);
            std::vector<const MetalDraw*> transparent;
            for(const MetalDraw& draw:frame.Draws)if(draw.Material.AlphaMode==3u)transparent.push_back(&draw);
            std::stable_sort(transparent.begin(),transparent.end(),[&](const MetalDraw* a,const MetalDraw* b){
                const auto distance=[&](const MetalDraw* draw){const float x=draw->Model[3]-frame.CameraPosition[0],y=draw->Model[7]-frame.CameraPosition[1],z=draw->Model[11]-frame.CameraPosition[2];return x*x+y*y+z*z;};
                return distance(a)>distance(b);});
            ordered.insert(ordered.end(),transparent.begin(),transparent.end());
            bool blending=false;
            for(const MetalDraw* drawPointer:ordered){const MetalDraw& draw=*drawPointer;if(draw.Material.AlphaMode==3u&&!blending){[encoder setRenderPipelineState:BlendPipeline];[encoder setDepthStencilState:NoWriteDepthState];blending=true;}const auto mesh=Meshes.find(draw.Mesh);if(mesh==Meshes.end())throw std::out_of_range("Metal draw references an unknown mesh handle.");
                const DrawUniforms uniforms=MakeDraw(draw);[encoder setVertexBuffer:mesh->second.Vertices offset:0 atIndex:0];[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:2];[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:2];
                BindTexture(encoder,0,TextureOr(draw.Material.BaseColorTexture,White));BindTexture(encoder,1,TextureOr(draw.Material.NormalTexture,Normal));BindTexture(encoder,2,TextureOr(draw.Material.MetallicRoughnessTexture,White));BindTexture(encoder,3,TextureOr(draw.Material.EmissiveTexture,White));BindTexture(encoder,4,TextureOr(draw.Material.OcclusionTexture,White));BindTexture(encoder,5,TextureOr(frame.EnvironmentTexture,Black));
                [encoder setCullMode:draw.Material.DoubleSided?MTLCullModeNone:MTLCullModeBack];
                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:mesh->second.IndexCount indexType:MTLIndexTypeUInt32 indexBuffer:mesh->second.Indices indexBufferOffset:0];}
            if(!frame.DebugVertices.empty()){[encoder setRenderPipelineState:DebugPipeline];[encoder setCullMode:MTLCullModeNone];[encoder setVertexBytes:frame.DebugVertices.data() length:frame.DebugVertices.size_bytes() atIndex:0];[encoder setVertexBytes:&frameUniforms length:sizeof(frameUniforms) atIndex:1];[encoder drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:frame.DebugVertices.size()];}
            [encoder endEncoding];
        }

        void Present(id<MTLCommandBuffer> command)
        {
            id<CAMetalDrawable> drawable=[Layer nextDrawable]; if(drawable==nil) return;
            MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0.02,0.025,0.035,1.0);
            id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
            if(Overlay) Overlay((__bridge void*)command,(__bridge void*)encoder);
            else {[encoder setRenderPipelineState:PresentPipeline];[encoder setFragmentTexture:ViewportColor atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}
            [encoder endEncoding];[command presentDrawable:drawable];
        }
    };

    MetalBackend::MetalBackend(GLFWwindow* window,std::uint32_t width,std::uint32_t height):m_Impl(std::make_unique<Impl>(window,width,height)){}
    MetalBackend::~MetalBackend()=default;

    std::uint64_t MetalBackend::CreateMesh(std::span<const MetalVertex> vertices,std::span<const std::uint32_t> indices)
    { if(vertices.empty()||indices.empty())throw std::invalid_argument("Metal mesh upload requires vertices and indices."); const std::uint64_t handle=m_Impl->NextMesh++;
      GpuMesh mesh;mesh.Vertices=[m_Impl->Device newBufferWithBytes:vertices.data() length:vertices.size_bytes() options:MTLResourceStorageModeShared];mesh.Indices=[m_Impl->Device newBufferWithBytes:indices.data() length:indices.size_bytes() options:MTLResourceStorageModeShared];mesh.IndexCount=indices.size();if(mesh.Vertices==nil||mesh.Indices==nil)throw std::runtime_error("Metal mesh-buffer allocation failed.");m_Impl->Meshes.emplace(handle,std::move(mesh));return handle;}
    void MetalBackend::DestroyMesh(std::uint64_t handle){m_Impl->WaitForLastCommand();if(m_Impl->Meshes.erase(handle)!=1u)throw std::out_of_range("Cannot destroy an unknown Metal mesh handle.");}
    std::uint64_t MetalBackend::CreateTexture(const MetalTextureUpload& upload){if(upload.Width==0u||upload.Height==0u||upload.MipLevels==0u||upload.Bytes==nullptr)throw std::invalid_argument("Metal texture upload is incomplete.");const auto handle=m_Impl->NextTexture++;m_Impl->Textures.emplace(handle,m_Impl->UploadTexture(upload));return handle;}
    void MetalBackend::DestroyTexture(std::uint64_t handle){m_Impl->WaitForLastCommand();if(m_Impl->Textures.erase(handle)!=1u)throw std::out_of_range("Cannot destroy an unknown Metal texture handle.");}
    void MetalBackend::Resize(std::uint32_t width,std::uint32_t height){if(width==0u||height==0u)throw std::invalid_argument("Metal viewport extent must be non-zero.");m_Impl->BuildTargets(width,height);}
    void MetalBackend::SetDrawableSize(std::uint32_t width,std::uint32_t height){if(width>0u&&height>0u)m_Impl->Layer.drawableSize=CGSizeMake(width,height);}
    void MetalBackend::Draw(const MetalFrame& frame){@autoreleasepool{id<MTLCommandBuffer> command=[m_Impl->Queue commandBuffer];if(command==nil)throw std::runtime_error("Metal command-buffer allocation failed.");m_Impl->EncodeScene(command,frame);m_Impl->Present(command);[command commit];m_Impl->LastCommand=command;}}
    std::uint32_t MetalBackend::Pick(std::uint32_t x,std::uint32_t y){if(x>=m_Impl->ViewportWidth||y>=m_Impl->ViewportHeight)throw std::out_of_range("Metal viewport pick is outside the image.");m_Impl->WaitForLastCommand();std::uint32_t value=0u;[m_Impl->ViewportID getBytes:&value bytesPerRow:sizeof(value) fromRegion:MTLRegionMake2D(x,y,1,1) mipmapLevel:0];return value;}
    MetalCapture MetalBackend::Capture(){m_Impl->WaitForLastCommand();MetalCapture result;result.Width=m_Impl->ViewportWidth;result.Height=m_Impl->ViewportHeight;result.ByteCount=static_cast<std::size_t>(result.Width)*result.Height*4u;result.RGBA=std::make_unique<std::uint8_t[]>(result.ByteCount);std::vector<std::uint16_t> half(static_cast<std::size_t>(result.Width)*result.Height*4u);[m_Impl->ViewportColor getBytes:half.data() bytesPerRow:static_cast<NSUInteger>(result.Width)*8u fromRegion:MTLRegionMake2D(0,0,result.Width,result.Height) mipmapLevel:0];for(std::size_t i=0;i<half.size();++i)result.RGBA[i]=static_cast<std::uint8_t>(std::clamp(HalfToFloat(half[i]),0.0f,1.0f)*255.0f+0.5f);return result;}
    void* MetalBackend::ViewportTexture()const noexcept{return (__bridge void*)m_Impl->ViewportColor;}
    void* MetalBackend::Device()const noexcept{return (__bridge void*)m_Impl->Device;}
    std::uint32_t MetalBackend::Width()const noexcept{return m_Impl->ViewportWidth;}
    std::uint32_t MetalBackend::Height()const noexcept{return m_Impl->ViewportHeight;}
    void MetalBackend::SetOverlayRecorder(MetalOverlayRecorder recorder){m_Impl->Overlay=std::move(recorder);}
}

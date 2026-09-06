module;

#include <string_view>

export module Kairo.Renderer.OpenGLShaders;

export namespace kairo::renderer::opengl_shaders
{
    /// OpenGL uses the same backend-neutral vertex contract as Vulkan. Matrix
    /// storage conversion happens on the CPU and uniforms are uploaded in
    /// column-major order, which keeps authored KairoMath transforms identical
    /// across APIs.
    inline constexpr std::string_view MeshVertex = R"GLSL(#version 410 core
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inColor;
layout(location=2) in vec3 inNormal;
layout(location=3) in vec2 inTexCoord;

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;
uniform mat4 uLightViewProjection;

out vec3 vertexColor;
out vec3 worldNormal;
out vec3 worldPosition;
out vec4 lightClipPosition;
out vec2 texCoord;

void main()
{
    vec4 world = uModel * vec4(inPosition, 1.0);
    gl_Position = uProjection * uView * world;
    vertexColor = inColor;
    worldNormal = uNormalMatrix * inNormal;
    worldPosition = world.xyz;
    lightClipPosition = uLightViewProjection * world;
    texCoord = inTexCoord;
}
)GLSL";

    /// Four-weight GPU skinning variant. The palette is supplied through a
    /// std140 uniform block rather than ordinary vertex uniforms so the full
    /// portable 255-joint ceiling fits OpenGL 4.1's guaranteed 16 KiB block.
    inline constexpr std::string_view SkinnedMeshVertex = R"GLSL(#version 410 core
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inColor;
layout(location=2) in vec3 inNormal;
layout(location=3) in vec2 inTexCoord;
layout(location=4) in uvec4 inJoints;
layout(location=5) in vec4 inWeights;

const int MaximumSkinJoints = 255;
layout(std140) uniform SkinPaletteBlock { mat4 uJoints[MaximumSkinJoints]; };

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;
uniform mat4 uLightViewProjection;

out vec3 vertexColor;
out vec3 worldNormal;
out vec3 worldPosition;
out vec4 lightClipPosition;
out vec2 texCoord;

mat4 SkinMatrix()
{
    mat4 skin = mat4(0.0);
    // Zero-weight slots are semantically inactive and may contain arbitrary
    // JOINTS_0 values. Never dereference them: out-of-range uniform-array
    // indexing is undefined even when the mathematical multiplier is zero.
    if (inWeights.x > 0.0) skin += inWeights.x * uJoints[inJoints.x];
    if (inWeights.y > 0.0) skin += inWeights.y * uJoints[inJoints.y];
    if (inWeights.z > 0.0) skin += inWeights.z * uJoints[inJoints.z];
    if (inWeights.w > 0.0) skin += inWeights.w * uJoints[inJoints.w];
    return skin;
}

void main()
{
    mat4 skin = SkinMatrix();
    vec4 assetPosition = skin * vec4(inPosition, 1.0);
    vec4 world = uModel * assetPosition;
    gl_Position = uProjection * uView * world;
    vertexColor = inColor;
    // glTF joint transforms are normally rigid/TRS. Using inverse-transpose of
    // the blended linear transform keeps normals correct under authored scale.
    vec3 assetNormal = transpose(inverse(mat3(skin))) * inNormal;
    worldNormal = uNormalMatrix * assetNormal;
    worldPosition = world.xyz;
    lightClipPosition = uLightViewProjection * world;
    texCoord = inTexCoord;
}
)GLSL";

    /// Metallic-roughness forward shading mirrors the Vulkan renderer's
    /// material and scene semantics. OpenGL 4.1 lacks descriptor sets, so the
    /// same values are explicit uniforms and fixed texture units.
    inline constexpr std::string_view MeshFragment = R"GLSL(#version 410 core
in vec3 vertexColor;
in vec3 worldNormal;
in vec3 worldPosition;
in vec4 lightClipPosition;
in vec2 texCoord;

layout(location=0) out vec4 outColor;
layout(location=1) out uint outObjectID;

const int MaximumLights = 16;
const float Pi = 3.14159265359;

uniform vec3 uCameraPosition;
uniform vec3 uAmbient;
uniform vec3 uBackground;
uniform float uExposure;
uniform float uEnvironmentIntensity;
uniform int uShadingMode;
uniform int uLightCount;
uniform int uShadowLightIndex;
uniform vec4 uLightPositionType[MaximumLights];
uniform vec4 uLightDirectionRange[MaximumLights];
uniform vec4 uLightColorIntensity[MaximumLights];
uniform vec4 uLightSpot[MaximumLights];

uniform vec4 uBaseColorFactor;
uniform vec3 uEmissiveFactor;
uniform vec4 uMaterialFactors;
uniform float uNormalScale;
uniform int uAlphaMode;
uniform uint uObjectID;
uniform bool uReceiveShadows;

uniform bool uShadowEnabled;
uniform float uShadowStrength;
uniform float uShadowTexel;
uniform float uReceiverBias;
uniform float uEnvironmentMaxLod;

uniform sampler2D uShadowMap;
uniform sampler2D uBaseColorMap;
uniform sampler2D uNormalMap;
uniform sampler2D uMetallicRoughnessMap;
uniform sampler2D uEmissiveMap;
uniform sampler2D uOcclusionMap;
uniform sampler2D uEnvironmentMap;

vec2 EquirectangularUV(vec3 direction)
{
    vec3 n = normalize(direction);
    return vec2(atan(n.z, n.x) / (2.0 * Pi) + 0.5,
        asin(clamp(n.y, -1.0, 1.0)) / Pi + 0.5);
}

float DistributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(Pi * denominator * denominator, 1.0e-6);
}

float GeometrySchlickGGX(float nDotDirection, float roughness)
{
    float k = ((roughness + 1.0) * (roughness + 1.0)) / 8.0;
    return nDotDirection / max(nDotDirection * (1.0 - k) + k, 1.0e-6);
}

vec3 FresnelSchlick(float cosTheta, vec3 reflectanceAtNormal)
{
    return reflectanceAtNormal + (1.0 - reflectanceAtNormal) *
        pow(1.0 - cosTheta, 5.0);
}

float DirectionalShadowVisibility(vec3 normal, vec3 lightDirection)
{
    if (!uShadowEnabled || !uReceiveShadows || lightClipPosition.w <= 0.0)
        return 1.0;
    vec3 projected = lightClipPosition.xyz / lightClipPosition.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    float depth = projected.z * 0.5 + 0.5;
    if (depth < 0.0 || depth > 1.0 || any(lessThan(uv, vec2(0.0))) ||
        any(greaterThan(uv, vec2(1.0)))) return 1.0;
    float bias = uReceiverBias * max(0.25,
        1.0 - max(dot(normal, lightDirection), 0.0));
    float blocked = 0.0;
    for (int y=-1; y<=1; ++y)
        for (int x=-1; x<=1; ++x)
            blocked += depth - bias > texture(uShadowMap,
                uv + vec2(x,y) * uShadowTexel).r ? 1.0 : 0.0;
    return 1.0 - uShadowStrength * blocked / 9.0;
}

void main()
{
    vec4 sampledBase = texture(uBaseColorMap, texCoord);
    vec4 baseAlpha = vec4(vertexColor, 1.0) * uBaseColorFactor * sampledBase;
    if (uAlphaMode == 2 && baseAlpha.a < uMaterialFactors.w) discard;
    vec3 baseColor = baseAlpha.rgb;
    vec4 mr = texture(uMetallicRoughnessMap, texCoord);
    float metallic = clamp(uMaterialFactors.x * mr.b, 0.0, 1.0);
    float roughness = clamp(uMaterialFactors.y * mr.g, 0.04, 1.0);
    float ao = clamp(uMaterialFactors.z * texture(uOcclusionMap, texCoord).r,
        0.0, 1.0);

    vec3 geometricNormal = normalize(worldNormal);
    vec3 dpdx = dFdx(worldPosition);
    vec3 dpdy = dFdy(worldPosition);
    vec2 duvdx = dFdx(texCoord);
    vec2 duvdy = dFdy(texCoord);
    float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    vec3 normal = geometricNormal;
    if (abs(determinant) > 1.0e-8)
    {
        vec3 tangent = normalize((dpdx * duvdy.y - dpdy * duvdx.y) /
            determinant);
        vec3 bitangent = normalize(cross(geometricNormal, tangent)) *
            sign(determinant);
        vec3 tangentNormal = texture(uNormalMap, texCoord).xyz * 2.0 - 1.0;
        tangentNormal.xy *= uNormalScale;
        normal = normalize(mat3(tangent, bitangent, geometricNormal) *
            tangentNormal);
    }

    if (uShadingMode == 1)
    {
        outColor = vec4(clamp(baseColor, 0.0, 1.0), 1.0);
        outObjectID = uObjectID;
        return;
    }
    if (uShadingMode == 2)
    {
        outColor = vec4(normal * 0.5 + 0.5, 1.0);
        outObjectID = uObjectID;
        return;
    }

    vec3 viewDirection = normalize(uCameraPosition - worldPosition);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    vec3 f0 = mix(vec3(0.04), baseColor, metallic);
    vec3 direct = vec3(0.0);
    float diagnosticLight = 0.0;
    for (int index=0; index<uLightCount; ++index)
    {
        int type = int(uLightPositionType[index].w + 0.5);
        vec3 lightDirection = normalize(uLightDirectionRange[index].xyz);
        float attenuation = 1.0;
        if (type != 1)
        {
            vec3 delta = uLightPositionType[index].xyz - worldPosition;
            float distanceToLight = length(delta);
            lightDirection = delta / max(distanceToLight, 1.0e-5);
            float range = uLightDirectionRange[index].w;
            float normalizedDistance = distanceToLight / max(range, 1.0e-5);
            attenuation = pow(clamp(1.0-pow(normalizedDistance,4.0),0.0,1.0),2.0) /
                max(distanceToLight * distanceToLight, 0.01);
            if (type == 3)
            {
                float cone = dot(-lightDirection,
                    normalize(uLightDirectionRange[index].xyz));
                attenuation *= smoothstep(uLightSpot[index].y,
                    uLightSpot[index].x, cone);
            }
            else if (type == 4)
            {
                float cosine = max(dot(-lightDirection,
                    normalize(uLightDirectionRange[index].xyz)), 0.0);
                attenuation *= cosine * uLightSpot[index].z * uLightSpot[index].w;
            }
        }
        float nDotL = max(dot(normal, lightDirection), 0.0);
        float visibility = index == uShadowLightIndex ?
            DirectionalShadowVisibility(normal, lightDirection) : 1.0;
        diagnosticLight += nDotL * attenuation * visibility *
            uLightColorIntensity[index].w;
        if (nDotL <= 0.0 || attenuation <= 0.0) continue;
        vec3 halfVector = normalize(viewDirection + lightDirection);
        vec3 fresnel = FresnelSchlick(max(dot(halfVector, viewDirection),0.0),f0);
        float distribution = DistributionGGX(normal, halfVector, roughness);
        float geometry = GeometrySchlickGGX(nDotV, roughness) *
            GeometrySchlickGGX(nDotL, roughness);
        vec3 specular = distribution * geometry * fresnel /
            max(4.0 * nDotV * nDotL, 1.0e-5);
        vec3 diffuseWeight = (vec3(1.0)-fresnel) * (1.0-metallic);
        vec3 radiance = uLightColorIntensity[index].rgb *
            uLightColorIntensity[index].w * attenuation;
        direct += (diffuseWeight * baseColor / Pi + specular) * radiance *
            nDotL * visibility;
    }
    if (uShadingMode == 3)
    {
        outColor = vec4(vec3(clamp(diagnosticLight, 0.0, 1.0)), 1.0);
        outObjectID = uObjectID;
        return;
    }

    vec3 reflected = reflect(-viewDirection, normal);
    vec3 environment = textureLod(uEnvironmentMap,
        EquirectangularUV(reflected), roughness * uEnvironmentMaxLod).rgb *
        uEnvironmentIntensity;
    vec3 ambient = (uAmbient * baseColor + environment * baseColor *
        (1.0-metallic) + environment * FresnelSchlick(nDotV, f0) *
        (1.0-roughness*0.65)) * ao;
    vec3 emissive = uEmissiveFactor * texture(uEmissiveMap, texCoord).rgb;
    vec3 hdr = (ambient + direct + emissive) * exp2(uExposure);
    outColor = vec4(hdr / (hdr + vec3(1.0)), uAlphaMode == 3 ? baseAlpha.a : 1.0);
    outObjectID = uObjectID;
}
)GLSL";

    inline constexpr std::string_view ShadowVertex = R"GLSL(#version 410 core
layout(location=0) in vec3 inPosition;
uniform mat4 uLightViewProjection;
uniform mat4 uModel;
void main() { gl_Position = uLightViewProjection * uModel * vec4(inPosition,1.0); }
)GLSL";

    inline constexpr std::string_view SkinnedShadowVertex = R"GLSL(#version 410 core
layout(location=0) in vec3 inPosition;
layout(location=4) in uvec4 inJoints;
layout(location=5) in vec4 inWeights;
const int MaximumSkinJoints = 255;
layout(std140) uniform SkinPaletteBlock { mat4 uJoints[MaximumSkinJoints]; };
uniform mat4 uLightViewProjection;
uniform mat4 uModel;
mat4 SkinMatrix()
{
    mat4 skin = mat4(0.0);
    // Zero-weight slots are semantically inactive and may contain arbitrary
    // JOINTS_0 values. Never dereference them: out-of-range uniform-array
    // indexing is undefined even when the mathematical multiplier is zero.
    if (inWeights.x > 0.0) skin += inWeights.x * uJoints[inJoints.x];
    if (inWeights.y > 0.0) skin += inWeights.y * uJoints[inJoints.y];
    if (inWeights.z > 0.0) skin += inWeights.z * uJoints[inJoints.z];
    if (inWeights.w > 0.0) skin += inWeights.w * uJoints[inJoints.w];
    return skin;
}
void main()
{
    gl_Position = uLightViewProjection * uModel *
        SkinMatrix() * vec4(inPosition, 1.0);
}
)GLSL";

    inline constexpr std::string_view ShadowFragment = R"GLSL(#version 410 core
void main() {}
)GLSL";

    inline constexpr std::string_view DebugVertex = R"GLSL(#version 410 core
layout(location=0) in vec3 inPosition;
layout(location=1) in vec4 inColor;
uniform mat4 uView;
uniform mat4 uProjection;
out vec4 color;
void main() { gl_Position=uProjection*uView*vec4(inPosition,1.0); color=inColor; }
)GLSL";

    inline constexpr std::string_view DebugFragment = R"GLSL(#version 410 core
in vec4 color;
layout(location=0) out vec4 outColor;
layout(location=1) out uint outObjectID;
void main() { outColor=color; outObjectID=0u; }
)GLSL";
}

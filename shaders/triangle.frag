#version 450

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inWorldPosition;
layout(location = 3) in vec4 inLightClipPosition;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectID;

layout(set = 0, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 projection;
    vec4 lightDirectionAndIntensity;
    vec4 ambient;
    vec4 cameraPosition;
    mat4 lightViewProjection;
    vec4 shadowParameters;
    vec4 viewportParameters;
} camera;

layout(set = 0, binding = 1) uniform sampler2D shadowMap;

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 tint;
} draw;

const float Pi = 3.14159265359;

float DistributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float nDotH = max(dot(normal, halfVector), 0.0);
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(Pi * denominator * denominator, 1.0e-6);
}

float GeometrySchlickGGX(float nDotDirection, float roughness)
{
    const float k = ((roughness + 1.0) * (roughness + 1.0)) / 8.0;
    return nDotDirection / max(nDotDirection * (1.0 - k) + k, 1.0e-6);
}

vec3 FresnelSchlick(float cosTheta, vec3 reflectanceAtNormal)
{
    return reflectanceAtNormal + (1.0 - reflectanceAtNormal) * pow(1.0 - cosTheta, 5.0);
}

// Returns direct-light visibility. Both the camera and light projection flip
// clip-space Y for Vulkan's positive-height viewport, so projected XY maps
// directly into the sampled image's normalized coordinates.
float DirectionalShadowVisibility(vec3 normal, vec3 lightDirection)
{
    if (camera.shadowParameters.x < 0.5 || inLightClipPosition.w <= 0.0)
        return 1.0;

    const vec3 projected = inLightClipPosition.xyz / inLightClipPosition.w;
    const vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z < 0.0 || projected.z > 1.0 ||
        uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;

    const float receiverBias = camera.shadowParameters.w *
        max(0.25, 1.0 - max(dot(normal, lightDirection), 0.0));
    const float texel = camera.shadowParameters.z;
    float blocked = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            const float storedDepth = texture(shadowMap, uv + vec2(x, y) * texel).r;
            blocked += projected.z - receiverBias > storedDepth ? 1.0 : 0.0;
        }
    return 1.0 - camera.shadowParameters.y * (blocked / 9.0);
}

void main()
{
    const vec3 baseColor = inColor * draw.tint.rgb;
    const float metallic = draw.normalColumn0.w;
    const float roughness = draw.normalColumn1.w;
    const float ambientOcclusion = draw.normalColumn2.w;
    const vec3 normal = normalize(inNormal);
    const vec3 viewDirection = normalize(camera.cameraPosition.xyz - inWorldPosition);
    const vec3 lightDirection = normalize(camera.lightDirectionAndIntensity.xyz);
    const vec3 halfVector = normalize(viewDirection + lightDirection);
    const float nDotL = max(dot(normal, lightDirection), 0.0);
    const float nDotV = max(dot(normal, viewDirection), 0.0);

    const vec3 f0 = mix(vec3(0.04), baseColor, metallic);
    const vec3 fresnel = FresnelSchlick(max(dot(halfVector, viewDirection), 0.0), f0);
    const float distribution = DistributionGGX(normal, halfVector, roughness);
    const float geometry = GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
    const vec3 specular = distribution * geometry * fresnel / max(4.0 * nDotV * nDotL, 1.0e-5);
    const vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    const vec3 radiance = vec3(camera.lightDirectionAndIntensity.w);
    const float shadowVisibility = DirectionalShadowVisibility(normal, lightDirection);
    const int shadingMode = int(camera.viewportParameters.x + 0.5);
    if (shadingMode == 1)
    {
        outColor = vec4(clamp(baseColor, vec3(0.0), vec3(1.0)), 1.0);
        outObjectID = floatBitsToUint(draw.tint.a);
        return;
    }
    if (shadingMode == 2)
    {
        outColor = vec4(normal * 0.5 + 0.5, 1.0);
        outObjectID = floatBitsToUint(draw.tint.a);
        return;
    }
    if (shadingMode == 3)
    {
        const float illumination = clamp(nDotL * shadowVisibility, 0.0, 1.0);
        outColor = vec4(vec3(illumination), 1.0);
        outObjectID = floatBitsToUint(draw.tint.a);
        return;
    }
    const vec3 direct = (diffuseWeight * baseColor / Pi + specular) * radiance * nDotL * shadowVisibility;
    const vec3 ambient = camera.ambient.rgb * baseColor * ambientOcclusion;
    const vec3 hdrColor = ambient + direct;
    outColor = vec4(hdrColor / (hdrColor + vec3(1.0)), 1.0);
    outObjectID = floatBitsToUint(draw.tint.a);
}

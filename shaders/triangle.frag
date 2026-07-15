#version 450

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inWorldPosition;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 projection;
    vec4 lightDirectionAndIntensity;
    vec4 ambient;
    vec4 cameraPosition;
} camera;

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
    const vec3 direct = (diffuseWeight * baseColor / Pi + specular) * radiance * nDotL;
    const vec3 ambient = camera.ambient.rgb * baseColor * ambientOcclusion;
    const vec3 hdrColor = ambient + direct;
    outColor = vec4(hdrColor / (hdrColor + vec3(1.0)), 1.0);
}

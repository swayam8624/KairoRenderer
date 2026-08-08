#version 450

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inWorldPosition;
layout(location = 3) in vec4 inLightClipPosition;
layout(location = 4) in vec2 inTexCoord;
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
    vec4 sceneParameters;
    vec4 backgroundColor;
    vec4 lightPositionType[16];
    vec4 lightDirectionRange[16];
    vec4 lightColorIntensity[16];
    vec4 lightSpot[16];
} camera;

layout(set = 0, binding = 1) uniform sampler2D shadowMap;
layout(set = 0, binding = 2) uniform sampler2D baseColorMap;
layout(set = 0, binding = 3) uniform sampler2D normalMap;
layout(set = 0, binding = 4) uniform sampler2D metallicRoughnessMap;
layout(set = 0, binding = 5) uniform sampler2D emissiveMap;
layout(set = 0, binding = 6) uniform sampler2D occlusionMap;
layout(set = 0, binding = 7) uniform MaterialData {
    vec4 baseColorFactor;
    vec4 emissiveAndNormalScale;
    vec4 factors;
    vec4 flags;
} material;
layout(set = 0, binding = 8) uniform sampler2D environmentMap;

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 tint;
} draw;

const float Pi = 3.14159265359;

vec2 EquirectangularUV(vec3 direction)
{
    const vec3 normalized = normalize(direction);
    return vec2(atan(normalized.z, normalized.x) / (2.0 * Pi) + 0.5,
        asin(clamp(normalized.y, -1.0, 1.0)) / Pi + 0.5);
}

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
    if (camera.shadowParameters.x < 0.5 || draw.tint.z < 0.5 ||
        inLightClipPosition.w <= 0.0)
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
    const vec4 sampledBase = texture(baseColorMap, inTexCoord);
    const vec4 baseColorAlpha = vec4(inColor, 1.0) *
        material.baseColorFactor * sampledBase;
    if (int(material.flags.x + 0.5) == 2 &&
        baseColorAlpha.a < material.factors.w)
        discard;
    const vec3 baseColor = baseColorAlpha.rgb;
    const vec4 metallicRoughness = texture(metallicRoughnessMap, inTexCoord);
    const float metallic = clamp(material.factors.x * metallicRoughness.b, 0.0, 1.0);
    const float roughness = clamp(material.factors.y * metallicRoughness.g, 0.04, 1.0);
    const float ambientOcclusion = clamp(material.factors.z *
        texture(occlusionMap, inTexCoord).r, 0.0, 1.0);
    const vec3 geometricNormal = normalize(inNormal);
    const vec3 dpdx = dFdx(inWorldPosition);
    const vec3 dpdy = dFdy(inWorldPosition);
    const vec2 duvdx = dFdx(inTexCoord);
    const vec2 duvdy = dFdy(inTexCoord);
    const float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    vec3 normal = geometricNormal;
    if (abs(determinant) > 1.0e-8)
    {
        const vec3 tangent = normalize((dpdx * duvdy.y - dpdy * duvdx.y) /
            determinant);
        const vec3 bitangent = normalize(cross(geometricNormal, tangent)) *
            sign(determinant);
        vec3 tangentNormal = texture(normalMap, inTexCoord).xyz * 2.0 - 1.0;
        tangentNormal.xy *= material.emissiveAndNormalScale.w;
        normal = normalize(mat3(tangent, bitangent, geometricNormal) * tangentNormal);
    }
    const vec3 viewDirection = normalize(camera.cameraPosition.xyz - inWorldPosition);
    const float nDotV = max(dot(normal, viewDirection), 0.0);
    const vec3 f0 = mix(vec3(0.04), baseColor, metallic);
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
        float illumination = 0.0;
        const int lightCount = int(camera.sceneParameters.x + 0.5);
        for (int index = 0; index < lightCount; ++index)
        {
            const int type = int(camera.lightPositionType[index].w + 0.5);
            vec3 lightDirection = normalize(camera.lightDirectionRange[index].xyz);
            float attenuation = 1.0;
            if (type != 1)
            {
                const vec3 delta = camera.lightPositionType[index].xyz - inWorldPosition;
                const float distanceToLight = length(delta);
                lightDirection = delta / max(distanceToLight, 1.0e-5);
                const float range = camera.lightDirectionRange[index].w;
                const float normalizedDistance = distanceToLight / max(range, 1.0e-5);
                attenuation = pow(clamp(1.0 - pow(normalizedDistance, 4.0), 0.0, 1.0), 2.0) /
                    max(distanceToLight * distanceToLight, 0.01);
                if (type == 3)
                {
                    const float cone = dot(-lightDirection,
                        normalize(camera.lightDirectionRange[index].xyz));
                    attenuation *= smoothstep(camera.lightSpot[index].y,
                        camera.lightSpot[index].x, cone);
                }
                else if (type == 4)
                {
                    const float emissionCosine = max(dot(-lightDirection,
                        normalize(camera.lightDirectionRange[index].xyz)), 0.0);
                    attenuation *= emissionCosine * camera.lightSpot[index].z *
                        camera.lightSpot[index].w;
                }
            }
            const float visibility = index == int(camera.sceneParameters.y + 0.5)
                ? DirectionalShadowVisibility(normal, lightDirection) : 1.0;
            illumination += max(dot(normal, lightDirection), 0.0) *
                attenuation * visibility * camera.lightColorIntensity[index].w;
        }
        outColor = vec4(vec3(clamp(illumination, 0.0, 1.0)), 1.0);
        outObjectID = floatBitsToUint(draw.tint.a);
        return;
    }
    vec3 direct = vec3(0.0);
    const int lightCount = int(camera.sceneParameters.x + 0.5);
    for (int index = 0; index < lightCount; ++index)
    {
        const int type = int(camera.lightPositionType[index].w + 0.5);
        vec3 lightDirection = normalize(camera.lightDirectionRange[index].xyz);
        float attenuation = 1.0;
        if (type != 1)
        {
            const vec3 delta = camera.lightPositionType[index].xyz - inWorldPosition;
            const float distanceToLight = length(delta);
            lightDirection = delta / max(distanceToLight, 1.0e-5);
            const float range = camera.lightDirectionRange[index].w;
            const float normalizedDistance = distanceToLight / max(range, 1.0e-5);
            attenuation = pow(clamp(1.0 - pow(normalizedDistance, 4.0), 0.0, 1.0), 2.0) /
                max(distanceToLight * distanceToLight, 0.01);
            if (type == 3)
            {
                const float cone = dot(-lightDirection,
                    normalize(camera.lightDirectionRange[index].xyz));
                attenuation *= smoothstep(camera.lightSpot[index].y,
                    camera.lightSpot[index].x, cone);
            }
            else if (type == 4)
            {
                const float emissionCosine = max(dot(-lightDirection,
                    normalize(camera.lightDirectionRange[index].xyz)), 0.0);
                attenuation *= emissionCosine * camera.lightSpot[index].z *
                    camera.lightSpot[index].w;
            }
        }
        const float nDotL = max(dot(normal, lightDirection), 0.0);
        if (nDotL <= 0.0 || attenuation <= 0.0) continue;
        const vec3 halfVector = normalize(viewDirection + lightDirection);
        const vec3 fresnel = FresnelSchlick(
            max(dot(halfVector, viewDirection), 0.0), f0);
        const float distribution = DistributionGGX(normal, halfVector, roughness);
        const float geometry = GeometrySchlickGGX(nDotV, roughness) *
            GeometrySchlickGGX(nDotL, roughness);
        const vec3 specular = distribution * geometry * fresnel /
            max(4.0 * nDotV * nDotL, 1.0e-5);
        const vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
        const float visibility = index == int(camera.sceneParameters.y + 0.5)
            ? DirectionalShadowVisibility(normal, lightDirection) : 1.0;
        const vec3 radiance = camera.lightColorIntensity[index].rgb *
            camera.lightColorIntensity[index].w * attenuation;
        direct += (diffuseWeight * baseColor / Pi + specular) * radiance *
            nDotL * visibility;
    }
    const vec3 reflectionDirection = reflect(-viewDirection, normal);
    const float environmentLod = roughness *
        float(max(textureQueryLevels(environmentMap) - 1, 0));
    const vec3 environmentRadiance = textureLod(environmentMap,
        EquirectangularUV(reflectionDirection), environmentLod).rgb *
        camera.sceneParameters.z;
    const vec3 diffuseEnvironment = environmentRadiance * baseColor * (1.0 - metallic);
    const vec3 specularEnvironment = environmentRadiance *
        FresnelSchlick(nDotV, f0) * (1.0 - roughness * 0.65);
    const vec3 ambient = (camera.ambient.rgb * baseColor +
        diffuseEnvironment + specularEnvironment) * ambientOcclusion;
    const vec3 emissive = material.emissiveAndNormalScale.rgb *
        texture(emissiveMap, inTexCoord).rgb;
    const vec3 hdrColor = (ambient + direct + emissive) *
        exp2(camera.viewportParameters.y);
    outColor = vec4(hdrColor / (hdrColor + vec3(1.0)), 1.0);
    if (int(material.flags.x + 0.5) == 3) outColor.a = baseColorAlpha.a;
    outObjectID = floatBitsToUint(draw.tint.a);
}

#version 450

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

#ifdef KAIRO_INSTANCED
// Four model columns, three normal columns and integer picking/shadow flags.
layout(location = 4) in mat4 instanceModel;
layout(location = 8) in vec4 instanceNormal0;
layout(location = 9) in vec4 instanceNormal1;
layout(location = 10) in vec4 instanceNormal2;
layout(location = 11) in uvec2 instanceFlags;
layout(location = 5) flat out uvec2 varyInstanceFlags;
#else
layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 tint;
} draw;
#endif

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 0) out vec3 varyColor;
layout(location = 1) out vec3 varyNormal;
layout(location = 2) out vec3 varyWorldPosition;
layout(location = 3) out vec4 varyLightClipPosition;
layout(location = 4) out vec2 varyTexCoord;

void main()
{
#ifdef KAIRO_INSTANCED
    const vec4 worldPosition = instanceModel * vec4(inPosition, 1.0);
    const mat3 normalMatrix = mat3(instanceNormal0.xyz, instanceNormal1.xyz, instanceNormal2.xyz);
    varyInstanceFlags = instanceFlags;
#else
    const vec4 worldPosition = draw.model * vec4(inPosition, 1.0);
    const mat3 normalMatrix = mat3(draw.normalColumn0.xyz, draw.normalColumn1.xyz, draw.normalColumn2.xyz);
#endif
    gl_Position = camera.projection * camera.view * worldPosition;
    varyColor = inColor;
    varyNormal = normalMatrix * inNormal;
    varyWorldPosition = worldPosition.xyz;
    varyLightClipPosition = camera.lightViewProjection * worldPosition;
    varyTexCoord = inTexCoord;
}

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

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 tint;
} draw;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 0) out vec3 outColor;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outWorldPosition;
layout(location = 3) out vec4 outLightClipPosition;
layout(location = 4) out vec2 outTexCoord;

void main()
{
    const vec4 worldPosition = draw.model * vec4(inPosition, 1.0);
    gl_Position = camera.projection * camera.view * worldPosition;
    outColor = inColor;
    const mat3 normalMatrix = mat3(draw.normalColumn0.xyz, draw.normalColumn1.xyz, draw.normalColumn2.xyz);
    outNormal = normalMatrix * inNormal;
    outWorldPosition = worldPosition.xyz;
    outLightClipPosition = camera.lightViewProjection * worldPosition;
    outTexCoord = inTexCoord;
}

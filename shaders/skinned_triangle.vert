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

layout(set = 0, binding = 9, std140) uniform SkinPaletteBlock {
    mat4 joints[255];
} skinPalette;

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
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;
layout(location = 0) out vec3 outColor;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outWorldPosition;
layout(location = 3) out vec4 outLightClipPosition;
layout(location = 4) out vec2 outTexCoord;

mat4 SkinMatrix()
{
    mat4 result = mat4(0.0);
    // Zero-weight JOINTS_0 slots are semantically inactive and may contain
    // arbitrary indices. Dereference only active influences.
    if (inWeights.x > 0.0) result += inWeights.x * skinPalette.joints[inJoints.x];
    if (inWeights.y > 0.0) result += inWeights.y * skinPalette.joints[inJoints.y];
    if (inWeights.z > 0.0) result += inWeights.z * skinPalette.joints[inJoints.z];
    if (inWeights.w > 0.0) result += inWeights.w * skinPalette.joints[inJoints.w];
    return result;
}

void main()
{
    const mat4 skin = SkinMatrix();
    const vec4 assetPosition = skin * vec4(inPosition, 1.0);
    const vec4 worldPosition = draw.model * assetPosition;
    gl_Position = camera.projection * camera.view * worldPosition;
    outColor = inColor;
    const mat3 modelNormal = mat3(draw.normalColumn0.xyz, draw.normalColumn1.xyz,
        draw.normalColumn2.xyz);
    const vec3 assetNormal = transpose(inverse(mat3(skin))) * inNormal;
    outNormal = modelNormal * assetNormal;
    outWorldPosition = worldPosition.xyz;
    outLightClipPosition = camera.lightViewProjection * worldPosition;
    outTexCoord = inTexCoord;
}

#version 450

layout(set = 0, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 projection;
    vec4 lightDirectionAndIntensity;
    vec4 ambient;
    vec4 cameraPosition;
    mat4 lightViewProjection;
    vec4 shadowParameters;
} camera;

layout(set = 0, binding = 9, std140) uniform SkinPaletteBlock {
    mat4 joints[255];
} skinPalette;

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 material;
} draw;

layout(location = 0) in vec3 inPosition;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

mat4 SkinMatrix()
{
    mat4 result = mat4(0.0);
    if (inWeights.x > 0.0) result += inWeights.x * skinPalette.joints[inJoints.x];
    if (inWeights.y > 0.0) result += inWeights.y * skinPalette.joints[inJoints.y];
    if (inWeights.z > 0.0) result += inWeights.z * skinPalette.joints[inJoints.z];
    if (inWeights.w > 0.0) result += inWeights.w * skinPalette.joints[inJoints.w];
    return result;
}

void main()
{
    gl_Position = camera.lightViewProjection * draw.model *
        SkinMatrix() * vec4(inPosition, 1.0);
}

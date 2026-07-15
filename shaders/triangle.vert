#version 450

layout(set = 0, binding = 0) uniform CameraMatrices {
    mat4 model;
    mat4 view;
    mat4 projection;
} camera;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 outColor;

void main()
{
    gl_Position = camera.projection * camera.view * camera.model * vec4(inPosition, 1.0);
    outColor = inColor;
}

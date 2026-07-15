#version 450

// Debug vertices are world-space so physics bounds, contacts, and axes retain
// their submitted coordinates.
layout(set = 0, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 projection;
    vec4 lightDirectionAndIntensity;
    vec4 ambient;
} camera;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
    gl_Position = camera.projection * camera.view * vec4(inPosition, 1.0);
    outColor = inColor;
}

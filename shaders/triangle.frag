#version 450

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform CameraMatrices {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 lightDirectionAndIntensity;
    vec4 ambient;
} camera;

void main()
{
    const float diffuse = max(dot(normalize(inNormal), normalize(camera.lightDirectionAndIntensity.xyz)), 0.0);
    const vec3 light = camera.ambient.rgb + vec3(diffuse * camera.lightDirectionAndIntensity.w);
    outColor = vec4(inColor * light, 1.0);
}

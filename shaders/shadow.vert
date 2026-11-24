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

#ifdef KAIRO_INSTANCED
layout(location = 4) in mat4 instanceModel;
#else
layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 material;
} draw;
#endif

layout(location = 0) in vec3 inPosition;

void main()
{
#ifdef KAIRO_INSTANCED
    gl_Position = camera.lightViewProjection * instanceModel * vec4(inPosition, 1.0);
#else
    gl_Position = camera.lightViewProjection * draw.model * vec4(inPosition, 1.0);
#endif
}

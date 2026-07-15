#version 450

// The first mesh milestone keeps the cube topology in shader constants while
// the renderer establishes the descriptor/camera/depth contract. General mesh
// buffers replace this showcase geometry in the following renderer increment.
layout(set = 0, binding = 0) uniform CameraMatrices {
    mat4 model;
    mat4 view;
    mat4 projection;
} camera;

layout(location = 0) out vec3 outColor;

void main()
{
    const vec3 positions[36] = vec3[](
        vec3(-1,-1, 1), vec3( 1,-1, 1), vec3( 1, 1, 1), vec3(-1,-1, 1), vec3( 1, 1, 1), vec3(-1, 1, 1),
        vec3( 1,-1,-1), vec3(-1,-1,-1), vec3(-1, 1,-1), vec3( 1,-1,-1), vec3(-1, 1,-1), vec3( 1, 1,-1),
        vec3(-1,-1,-1), vec3(-1,-1, 1), vec3(-1, 1, 1), vec3(-1,-1,-1), vec3(-1, 1, 1), vec3(-1, 1,-1),
        vec3( 1,-1, 1), vec3( 1,-1,-1), vec3( 1, 1,-1), vec3( 1,-1, 1), vec3( 1, 1,-1), vec3( 1, 1, 1),
        vec3(-1, 1, 1), vec3( 1, 1, 1), vec3( 1, 1,-1), vec3(-1, 1, 1), vec3( 1, 1,-1), vec3(-1, 1,-1),
        vec3(-1,-1,-1), vec3( 1,-1,-1), vec3( 1,-1, 1), vec3(-1,-1,-1), vec3( 1,-1, 1), vec3(-1,-1, 1)
    );
    const vec3 position = positions[gl_VertexIndex];
    gl_Position = camera.projection * camera.view * camera.model * vec4(position, 1.0);
    outColor = position * 0.35 + vec3(0.55);
}

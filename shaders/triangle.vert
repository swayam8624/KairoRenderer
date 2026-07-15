#version 450

// The first graphics milestone intentionally generates positions from
// gl_VertexIndex. It proves the shader/pipeline/render-pass contract without
// prematurely coupling the renderer to a vertex-buffer API.
layout(location = 0) out vec3 outColor;

void main()
{
    const vec2 positions[3] = vec2[](
        vec2(0.0, -0.62),
        vec2(0.62, 0.62),
        vec2(-0.62, 0.62)
    );
    const vec3 colors[3] = vec3[](
        vec3(0.95, 0.29, 0.22),
        vec3(0.23, 0.72, 0.95),
        vec3(0.35, 0.90, 0.46)
    );
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    outColor = colors[gl_VertexIndex];
}

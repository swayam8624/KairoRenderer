#version 450
layout(location=0) out vec2 uv;
void main()
{
    vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));
    // Positive-height Vulkan viewport maps NDC -1 to its top-left.
    uv=p;
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}

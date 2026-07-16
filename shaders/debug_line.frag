#version 450

layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectID;

void main()
{
    outColor = inColor;
    outObjectID = 0u;
}

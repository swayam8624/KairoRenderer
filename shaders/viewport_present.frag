#version 450
layout(set=0,binding=0) uniform sampler2D viewportColor;
layout(constant_id=0) const uint manualGamma=0;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
void main()
{
    vec3 linear=clamp(texture(viewportColor,uv).rgb,0.0,1.0);
    outputColor=vec4(manualGamma!=0u?pow(linear,vec3(1.0/2.2)):linear,1.0);
}

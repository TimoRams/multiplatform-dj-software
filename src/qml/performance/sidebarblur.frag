#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 stepSize;
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    vec4 blurred = texture(source, qt_TexCoord0) * 0.227027;
    blurred += texture(source, qt_TexCoord0 + stepSize * 1.384615) * 0.316216;
    blurred += texture(source, qt_TexCoord0 - stepSize * 1.384615) * 0.316216;
    blurred += texture(source, qt_TexCoord0 + stepSize * 3.230769) * 0.070270;
    blurred += texture(source, qt_TexCoord0 - stepSize * 3.230769) * 0.070270;
    fragColor = blurred * qt_Opacity;
}

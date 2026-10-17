#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 stepSize;
    float sigma;
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    vec4 blurred = texture(source, qt_TexCoord0);
    float totalWeight = 1.0;
    for (int offset = 1; offset <= 12; ++offset) {
        float distance = float(offset);
        float weight = exp(-0.5 * distance * distance / (sigma * sigma));
        vec2 delta = stepSize * distance;
        blurred += (texture(source, qt_TexCoord0 + delta)
                    + texture(source, qt_TexCoord0 - delta)) * weight;
        totalWeight += 2.0 * weight;
    }
    fragColor = blurred * (qt_Opacity / totalWeight);
}

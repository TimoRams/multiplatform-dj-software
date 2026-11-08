#version 440
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 stemEnergy;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 gains;
    vec4 background;
};
layout(binding = 1) uniform sampler2D source;
void main()
{
    vec4 fullMix = texture(source, uv);
    float total = dot(stemEnergy, vec4(1.0));
    float ratio = total > 0.000001
        ? clamp(sqrt(dot(stemEnergy, gains * gains) / total), 0.0, 1.0) : 1.0;
    float ghostAlpha = fullMix.a * (150.0 / 255.0) * clamp((1.0 - ratio) * 8.0, 0.0, 1.0);
    vec4 ghost = vec4(vec3(92.0, 95.0, 104.0) / 255.0 * ghostAlpha, ghostAlpha);
    vec4 audible = vec4(0.0);
    if (ratio > 0.0001) {
        float y = 0.5 + (uv.y - 0.5) / ratio;
        if (y >= 0.0 && y <= 1.0)
            audible = texture(source, vec2(uv.x, y));
    }
    vec4 waveform = audible + ghost * (1.0 - audible.a);
    vec4 bg = vec4(background.rgb * background.a, background.a);
    fragColor = (waveform + bg * (1.0 - waveform.a)) * qt_Opacity;
}

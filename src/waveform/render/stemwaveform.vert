#version 440
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
layout(location = 2) in vec4 energy;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 stemEnergy;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 gains;
    vec4 background;
};
void main()
{
    uv = texCoord;
    stemEnergy = energy;
    gl_Position = qt_Matrix * vec4(position, 0.0, 1.0);
}

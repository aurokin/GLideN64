#version 450

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outTexCoord0;
layout(location = 2) out vec2 outTexCoord1;

void main()
{
    gl_Position = inPosition;
    outColor = inColor;
    outTexCoord0 = inTexCoord0;
    outTexCoord1 = inTexCoord1;
}

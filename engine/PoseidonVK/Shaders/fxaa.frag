#version 450
#extension GL_GOOGLE_include_directive : require
#define FXAA_PC 1
#define FXAA_GLSL_130 1
#define FXAA_QUALITY__PRESET 29
#define FXAA_GREEN_AS_LUMA 0
#define FXAA_LUMA_SEPARATE_R8 1
#define CWR_FXAA_RGB_LUMA 1
#define FXAA_GATHER4_ALPHA 0
#include "../ThirdParty/FXAA/Fxaa3_11.h"
layout(set=0, binding=0) uniform sampler2D source;
layout(set=2, binding=0) uniform sampler2D sceneDepth;
layout(location=0) in vec2 texCoord;
layout(location=0) out vec4 outColor;
void main() {
    outColor = FxaaPixelShader(texCoord, vec4(0), source, source, source, source,
        1.0/vec2(textureSize(source,0)), vec4(0), vec4(0), vec4(0),
        0.5, 0.125, 0.0312, 0.0, 0.0, 0.0, vec4(0));
    outColor.a = 1.0;
    gl_FragDepth = texture(sceneDepth,texCoord).r;
}

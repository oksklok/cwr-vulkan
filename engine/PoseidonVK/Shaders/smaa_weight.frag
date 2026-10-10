#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa_common.glsl"
layout(set=0, binding=0) uniform sampler2D source;
layout(set=2, binding=0) uniform sampler2D area;
layout(set=3, binding=0) uniform sampler2D search;
layout(location=0) in vec2 texCoord;
layout(location=0) out vec4 outColor;
void main() {
    vec2 pixcoord; vec4 offset[3];
    SMAABlendingWeightCalculationVS(texCoord, pixcoord, offset);
    outColor = SMAABlendingWeightCalculationPS(texCoord, pixcoord, offset, source, area, search, vec4(0));
}

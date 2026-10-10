#version 450
#extension GL_GOOGLE_include_directive : require
#include "shape_uniforms.glsl"
layout(location = 0) out vec4 outColor;
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in float fogVisibility;
layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;
layout(set = 1, binding = 0) uniform sampler2D detailTexture;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; float alphaCutoff; float invGamma; float detailEnabled; vec4 lightDirection; } draw;
void main() {
    outColor = texture(diffuseTexture, texCoord) * draw.color * vertexColor;
    // Match GL33 TGDetail's 32x UV and PSDetail's alpha modulation.
    if (draw.detailEnabled > 1.5) {
        // Stock SpecularTexture path, matching GL33's decoded bump sample.
        vec3 bumpNormal = -(texture(detailTexture, texCoord).xyz * 2.0 - 1.0);
        outColor.rgb += clamp(dot(draw.lightDirection.xyz, bumpNormal), 0.0, 1.0);
    } else if (draw.detailEnabled > 0.5) outColor.rgb *= texture(detailTexture, texCoord * 32.0).a * 2.0;
    if (outColor.a < draw.alphaCutoff) discard;
    float luminance = clamp(dot(outColor.rgb, lighting.eyeCoef.rgb), 0.0, 1.0);
    float nightBlend = clamp(luminance + lighting.eyeCoef.a, 0.0, 1.0);
    outColor.rgb = mix(vec3(luminance), outColor.rgb, nightBlend);
    outColor.rgb = mix(lighting.fogColor.rgb, outColor.rgb, clamp(fogVisibility, 0.0, 1.0));
    outColor.rgb = pow(max(outColor.rgb, vec3(0)), vec3(draw.invGamma));
}

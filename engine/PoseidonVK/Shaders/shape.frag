#version 450
#extension GL_GOOGLE_include_directive : require
#include "shape_uniforms.glsl"
#include "shadow_sample.glsl"
layout(location = 0) out vec4 outColor;
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in float fogVisibility;
layout(location = 3) in vec3 specularColor;
layout(location = 4) in vec3 shadowWorld;
layout(location = 5) in vec4 previousClip;
#ifdef TEMPORAL
layout(location = 1) out vec4 motion;
#endif
layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;
layout(set = 1, binding = 0) uniform sampler2D detailTexture;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; float alphaCutoff; float reserved; float detailEnabled; vec4 lightDirection; } draw;
void main() {
    outColor = texture(diffuseTexture, texCoord) * draw.color * vertexColor;
    // Match GL33 TGDetail's 32x UV and PSDetail's alpha modulation.
    if (draw.detailEnabled > 1.5) {
        // Explicit Water shader family, not SpecularTexture alone.
        vec3 bumpNormal = -(texture(detailTexture, texCoord).xyz * 2.0 - 1.0);
        outColor.rgb += clamp(dot(draw.lightDirection.xyz, bumpNormal), 0.0, 1.0);
    } else if (draw.detailEnabled > 0.5) outColor.rgb *= texture(detailTexture, texCoord * 32.0).a * 2.0;
    // PSNormal/PSDetail add specular after diffuse/detail, before night-eye and fog.
    if (draw.detailEnabled < 1.5) outColor.rgb += specularColor;
    if (draw.reserved > 0.5) {
        // Only opaque/cutout world draws use coverage. True transparency retains
        // ordinary blending. Center the coverage ramp on the original cutoff.
        outColor.a = draw.alphaCutoff > 0.0 ?
            clamp((outColor.a-draw.alphaCutoff)/max(fwidth(outColor.a),1.0/255.0)+0.5,0.0,1.0) : 1.0;
    } else if (outColor.a < draw.alphaCutoff) discard;
    outColor.rgb *= sunlightVisibility(shadowWorld, fogVisibility);
    float luminance = clamp(dot(outColor.rgb, lighting.eyeCoef.rgb), 0.0, 1.0);
    float nightBlend = clamp(luminance + lighting.eyeCoef.a, 0.0, 1.0);
    outColor.rgb = mix(vec3(luminance), outColor.rgb, nightBlend);
    outColor.rgb = mix(lighting.fogColor.rgb, outColor.rgb, clamp(fogVisibility, 0.0, 1.0));
#ifdef TEMPORAL
    // UV displacement current -> previous (including projection jitter), expected
    // previous depth, validity. Unsupported/translucent fragments overwrite validity.
    bool reactive = lighting.temporal.w > 1.5 || (lighting.temporal.w > .5 && outColor.a < .98);
    motion = vec4(0,0,0,reactive ? -1 : 0);
    if (previousClip.w > 0.0001 && !reactive) {
        vec3 oldNdc = previousClip.xyz / previousClip.w;
        motion = vec4(oldNdc.xy * 0.5 + 0.5 - gl_FragCoord.xy * lighting.temporalExtent.xy,
                      previousClip.w, draw.alphaCutoff > 0.0 ? 0.75 : 1.0);
    }
#endif
}

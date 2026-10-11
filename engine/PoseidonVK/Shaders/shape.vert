#version 450
#extension GL_GOOGLE_include_directive : require
#include "shape_uniforms.glsl"
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out float fogVisibility;
layout(location = 3) out vec3 specularColor;
layout(location = 4) out vec3 shadowWorld;
layout(location = 5) out vec4 previousClip;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; float alphaCutoff; float reserved; float detailEnabled; float shadow; } draw;
void main() {
    gl_Position = draw.mvp * vec4(position, 1.0);
    gl_Position.xy += lighting.temporal.xy * gl_Position.w;
    previousClip = lighting.temporal.z > 0.5 ? lighting.previousMVP * vec4(position, 1.0) : vec4(0);
    texCoord = uv;
    specularColor = vec3(0.0);
    vec3 relativeWorld = (lighting.world * vec4(position, 1.0)).xyz;
    shadowWorld = relativeWorld;
    float distance = length(relativeWorld);
    fogVisibility = lighting.fogParams.z > 0.5 ?
        clamp(1.0 - (distance - lighting.fogParams.x) * lighting.fogParams.y, 0.0, 1.0) : 1.0;
    if (draw.shadow > 0.5) {
        // Scene::FogExponential, also sampled by software DoShadowLighting.
        if (lighting.fogParams.z > 0.5 && lighting.fogParams.w > 0.0)
            fogVisibility = distance >= lighting.fogParams.w ? 0.0 :
                clamp(exp(2.9957322736 * (lighting.fogParams.x - distance) / lighting.fogParams.w), 0.0, 1.0);
        vertexColor = vec4(1.0); // Material opacity and engine shadow-distance fade only.
        return;
    }
    vec3 worldNormal = normalize(lighting.normalMatrix * normal);
    float NdotL = max(0.0, dot(worldNormal, -lighting.sunDirection.xyz));
    vec3 color = lighting.emissive.rgb +
        (lighting.ambient.rgb + lighting.diffuse.rgb * NdotL) * lighting.ambient.w;
    // Engine-selected point/reflector lights, matching GL33's falloff/cone.
    for (int i = 0; i < int(lighting.localCount.x); ++i) {
        ShapeLocalLight light = lighting.localLights[i];
        vec3 toLight = light.position.xyz - relativeWorld;
        float size2 = dot(toLight, toLight);
        float start2 = light.position.w * light.position.w;
        if (size2 >= start2 * 100.0) continue;
        float cone = 1.0;
        if (light.direction.w > 0.5) {
            float inside = -dot(toLight, light.direction.xyz);
            if (inside <= 0.0) continue;
            float cos2 = inside * inside / max(size2, 1e-8);
            if (cos2 < 0.95677279) continue;
            cone = clamp((cos2 - 0.95677279) / (0.98063081 - 0.95677279), 0.0, 1.0);
        }
        float attenuation = size2 >= start2 ? start2 / max(size2, 1e-8) : 1.0;
        float cosine = dot(toLight, worldNormal);
        color += cosine > 0.0 ?
            (light.diffuse.rgb * cosine * inversesqrt(max(size2, 1e-8)) + light.ambient.rgb) * attenuation * cone :
            light.ambient.rgb * attenuation;
    }
    vertexColor = vec4(clamp(color, 0.0, 1.0), 1.0);
    // GL33 VSTransform: camera-relative half-vector, separate from diffuse.
    if (lighting.specular.w > 0.0 && lighting.ambient.w > 0.0) {
        vec3 viewDirection = normalize(-relativeWorld);
        vec3 halfVector = normalize(-lighting.sunDirection.xyz + viewDirection);
        float highlight = pow(max(dot(worldNormal, halfVector), 0.0), max(lighting.specular.w, 1.0));
        specularColor = clamp(lighting.specular.rgb * highlight * lighting.ambient.w, 0.0, 1.0);
    }
}

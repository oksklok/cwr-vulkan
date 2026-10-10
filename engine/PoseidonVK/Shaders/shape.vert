#version 450
#extension GL_GOOGLE_include_directive : require
#include "shape_uniforms.glsl"
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out float fogVisibility;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; float alphaCutoff; float invGamma; } draw;
void main() {
    gl_Position = draw.mvp * vec4(position, 1.0);
    texCoord = uv;
    vec3 worldNormal = normalize(lighting.normalMatrix * normal);
    float NdotL = max(0.0, dot(worldNormal, -lighting.sunDirection.xyz));
    vec3 color = lighting.emissive.rgb +
        (lighting.ambient.rgb + lighting.diffuse.rgb * NdotL) * lighting.ambient.w;
    vertexColor = vec4(clamp(color, 0.0, 1.0), 1.0);
    vec3 relativeWorld = (lighting.world * vec4(position, 1.0)).xyz;
    float distance = length(relativeWorld); // World is already camera-relative.
    fogVisibility = lighting.fogParams.z > 0.5 ?
        clamp(1.0 - (distance - lighting.fogParams.x) * lighting.fogParams.y, 0.0, 1.0) : 1.0;
}

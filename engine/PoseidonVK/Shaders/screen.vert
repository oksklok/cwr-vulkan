#version 450
#extension GL_GOOGLE_include_directive : require
#include "shape_uniforms.glsl"
layout(location = 0) in vec4 clipPosition;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 3) in float fog;
layout(location = 4) in vec4 oldClip;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out float fogVisibility;
layout(location = 3) out vec3 specularColor;
layout(location = 4) out vec3 shadowWorld;
layout(location = 5) out vec4 previousClip;
void main() {
    gl_Position = clipPosition;
    gl_Position.xy += lighting.temporal.xy * gl_Position.w;
    previousClip = oldClip;
    texCoord = uv;
    vertexColor = color;
    fogVisibility = fog;
    specularColor = vec3(0.0);
    shadowWorld = vec3(0.0);
    if (lighting.shadowReceiver.x > 0.5) {
        vec3 view = vec3(clipPosition.x / lighting.shadowReceiver.y,
                         -clipPosition.y / lighting.shadowReceiver.z, clipPosition.w);
        shadowWorld = (lighting.world * vec4(view, 0.0)).xyz;
    }
}

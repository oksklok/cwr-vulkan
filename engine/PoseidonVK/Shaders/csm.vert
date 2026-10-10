#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=0) out vec2 texCoord;
layout(push_constant) uniform ShadowDraw { mat4 vp; float alphaCutoff; } draw;
void main() {
    // Shared ShadowMath already produces 0..1 depth. Match receiver UV Y.
    gl_Position = draw.vp * vec4(position, 1.0);
    gl_Position.y = -gl_Position.y;
    texCoord = uv;
}

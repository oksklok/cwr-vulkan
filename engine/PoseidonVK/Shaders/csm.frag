#version 450
layout(location=0) in vec2 texCoord;
layout(set=0, binding=0) uniform sampler2D casterTexture;
layout(push_constant) uniform ShadowDraw { mat4 vp; float alphaCutoff; } draw;
void main() {
    if (draw.alphaCutoff > 0.0 && texture(casterTexture, texCoord).a < draw.alphaCutoff) discard;
}

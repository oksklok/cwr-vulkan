#version 450
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; } draw;
void main() { outColor = draw.color; }

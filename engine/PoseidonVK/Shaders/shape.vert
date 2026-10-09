#version 450
layout(location = 0) in vec3 position;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; } draw;
void main() { gl_Position = draw.mvp * vec4(position, 1.0); }

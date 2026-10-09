#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 texCoord;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; float alphaCutoff; float invGamma; } draw;
void main() { gl_Position = draw.mvp * vec4(position, 1.0); texCoord = uv; }

#version 450
layout(location = 0) out vec4 outColor;
layout(location = 0) in vec2 texCoord;
layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; } draw;
void main() { outColor = texture(diffuseTexture, texCoord) * draw.color; }

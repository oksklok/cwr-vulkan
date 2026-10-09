#version 450
layout(location = 0) in vec4 clipPosition;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
void main() {
    gl_Position = clipPosition;
    texCoord = uv;
    vertexColor = color;
}

#version 450
layout(location = 0) in vec4 clipPosition;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 3) in float fog;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out float fogVisibility;
layout(location = 3) out vec3 specularColor;
void main() {
    gl_Position = clipPosition;
    texCoord = uv;
    vertexColor = color;
    fogVisibility = fog;
    specularColor = vec3(0.0);
}

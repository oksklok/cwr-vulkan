#version 450
layout(location = 0) out vec2 texCoord;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    texCoord = p;
    // Positive Vulkan viewport: framebuffer and sampled image share Y direction.
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

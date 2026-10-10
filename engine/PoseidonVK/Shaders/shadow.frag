#version 450
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in float fogVisibility;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;
layout(push_constant) uniform ShapeDraw { mat4 mvp; vec4 color; float alphaCutoff; } draw;
void main() {
    // Late depth/stencil tests ensure discarded leaf/window holes never stamp
    // the exclusion mask. Match GL33's projected-shadow fragment path.
    gl_FragDepth = gl_FragCoord.z;
    float alpha = texture(diffuseTexture, texCoord).a * draw.color.a * vertexColor.a * fogVisibility;
    if (alpha < draw.alphaCutoff) discard;
    outColor = vec4(0.0, 0.0, 0.0, alpha);
}

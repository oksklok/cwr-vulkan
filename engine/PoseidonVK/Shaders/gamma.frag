#version 450
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(push_constant) uniform Gamma { float invGamma; } gamma;
void main() {
    vec3 c = texture(frame, texCoord).rgb;
    // All scene alpha/additive blending and projected shadows are finished.
    // Identity skips pow; alpha follows GL33's finished-frame composition.
    outColor = vec4(gamma.invGamma == 1.0 ? c : pow(max(c, vec3(0)), vec3(gamma.invGamma)), 1.0);
}

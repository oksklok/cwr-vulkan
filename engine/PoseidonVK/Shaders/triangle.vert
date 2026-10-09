// Adapted from koosoli/PoseidonVK 7523bd5 bootstrap_triangle.vert.glsl.
// GPL-3.0-or-later with additional terms: see LICENSE.
#version 450
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 vColor;
layout(push_constant) uniform BootstrapConstants
{
    vec4 viewport;
    vec4 clearColor;
} pc;
void main()
{
    float aspectComp = pc.viewport.w / max(pc.viewport.z, 1.0);
    gl_Position = vec4(inPosition.x * aspectComp, inPosition.y, 0.0, 1.0);
    vColor = inColor;
}

#version 450
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform AO {
    vec4 projection; // fx, fy, A, B: depth = A + B / viewZ
    vec4 settings;   // strength, radius metres, bias metres, fade metres
} ao;

vec3 positionAt(vec2 uv) {
    float depth = texture(sceneDepth, uv).r;
    float z = ao.projection.w / (depth - ao.projection.z);
    return vec3((uv * 2.0 - 1.0) * vec2(1.0, -1.0) / ao.projection.xy * z, z);
}

void main() {
    outColor = vec4(1.0);
    float depth = texture(sceneDepth, texCoord).r;
    if (depth >= 0.999999 || depth <= 0.0) return;
    vec3 p = positionAt(texCoord);
    if (p.z <= 0.0 || p.z >= ao.settings.w) return;
    vec2 pixel = 1.0 / vec2(textureSize(sceneDepth, 0));
    // Use the nearer derivative on each axis, avoiding silhouette normals
    // that bridge a foreground object and distant background.
    vec3 l = positionAt(texCoord - vec2(pixel.x, 0));
    vec3 r = positionAt(texCoord + vec2(pixel.x, 0));
    vec3 t = positionAt(texCoord - vec2(0, pixel.y));
    vec3 b = positionAt(texCoord + vec2(0, pixel.y));
    vec3 dx = abs(p.z-l.z) < abs(r.z-p.z) ? p-l : r-p;
    vec3 dy = abs(p.z-t.z) < abs(b.z-p.z) ? p-t : b-p;
    vec3 n = cross(dx, dy);
    float n2 = dot(n,n);
    if (n2 < 1e-12) return;
    n *= inversesqrt(n2);
    if (dot(n,p) > 0.0) n = -n;
    const vec2 kernel[8] = vec2[8](
        vec2(0.35,0), vec2(-0.35,0), vec2(0,0.35), vec2(0,-0.35),
        vec2(0.707,0.707), vec2(-0.707,0.707), vec2(0.707,-0.707), vec2(-0.707,-0.707));
    vec2 radiusUV = 0.5 * ao.projection.xy * ao.settings.y / p.z;
    float sum = 0.0;
    for (int i=0; i<8; ++i) {
        vec2 uv = texCoord + kernel[i] * radiusUV;
        if (any(lessThan(uv,pixel)) || any(greaterThan(uv,vec2(1)-pixel))) continue;
        vec3 delta = positionAt(uv) - p;
        float d = length(delta);
        if (d <= ao.settings.z || d >= ao.settings.y) continue;
        float horizon = max((dot(n,delta)-ao.settings.z) / d, 0.0);
        sum += horizon * (1.0-d/ao.settings.y);
    }
    float fade = 1.0-smoothstep(ao.settings.w*0.6,ao.settings.w,p.z);
    float visibility = 1.0-clamp(ao.settings.x*sum/8.0*fade,0.0,0.35);
    outColor = vec4(vec3(visibility),1.0);
}

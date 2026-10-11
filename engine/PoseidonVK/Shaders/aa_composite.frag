#version 450
layout(set=0,binding=0) uniform sampler2D source;
#ifdef MULTISAMPLE_DEPTH
layout(set=1,binding=0) uniform sampler2DMS sceneDepth;
#else
layout(set=1,binding=0) uniform sampler2D sceneDepth;
#endif
layout(push_constant) uniform AA { vec4 metrics; vec4 outputMetrics; } aa;
layout(location=0) in vec2 texCoord;
layout(location=0) out vec4 outColor;
void main() {
    // Exact pixel-area integration, including fractional 125/150% footprints.
    // A <= 2x scale overlaps at most three texels on either axis.
    // Divide dimensions directly: a rounded reciprocal can pull adjacent depth
    // into the footprint even at 100% scale (e.g. a 642-pixel drawable).
    vec2 ratio = aa.metrics.zw / aa.outputMetrics.zw;
    vec2 lo = floor(gl_FragCoord.xy) * ratio, hi = lo + ratio;
    ivec2 first = ivec2(floor(lo));
    vec4 color = vec4(0); float total = 0; float depth = 1;
    for (int y=0; y<3; ++y) for (int x=0; x<3; ++x) {
        ivec2 p = first + ivec2(x,y);
        vec2 overlap = max(vec2(0), min(hi,vec2(p+1))-max(lo,vec2(p)));
        float w = overlap.x*overlap.y;
        if (w == 0) continue;
        p = clamp(p, ivec2(0), ivec2(aa.metrics.zw)-1);
        color += texelFetch(source,p,0)*w; total += w;
#ifdef MULTISAMPLE_DEPTH
        for (int s=0; s<textureSamples(sceneDepth); ++s) depth = min(depth,texelFetch(sceneDepth,p,s).r);
#else
        depth = min(depth,texelFetch(sceneDepth,p,0).r);
#endif
    }
    outColor = color / total;
    gl_FragDepth = depth;
}

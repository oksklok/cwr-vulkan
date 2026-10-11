#version 450
layout(set=0,binding=0) uniform sampler2D source;
layout(set=1,binding=0) uniform sampler2D sceneDepth;
layout(set=2,binding=0) uniform sampler2D motionImage;
layout(set=3,binding=0) uniform sampler2D historyImage;
layout(push_constant) uniform AA { vec4 metrics; vec4 control; } aa;
layout(location=0) in vec2 texCoord;
layout(location=0) out vec4 outColor;
vec3 toYCoCg(vec3 c) { return vec3(dot(c,vec3(.25,.5,.25)), .5*c.r-.5*c.b, -.25*c.r+.5*c.g-.25*c.b); }
vec3 toRGB(vec3 c) { return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z); }
float viewDepth(float d) { return aa.control.y / min(d-aa.control.x,-1e-7); }
void main() {
    ivec2 size = textureSize(source,0), p = ivec2(gl_FragCoord.xy);
    vec3 current = texelFetch(source,p,0).rgb;
    float depth = viewDepth(texelFetch(sceneDepth,p,0).r);
    vec3 low=vec3(1e6), high=vec3(-1e6), mean=vec3(0), moment=vec3(0);
    vec4 motion = texelFetch(motionImage,p,0);
    float nearest = texelFetch(sceneDepth,p,0).r;
    bool reactive = motion.w < .5 && nearest < .99999;
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) {
        ivec2 q=clamp(p+ivec2(x,y),ivec2(0),size-1);
        vec3 c=toYCoCg(texelFetch(source,q,0).rgb);
        low=min(low,c); high=max(high,c); mean+=c; moment+=c*c;
        vec4 m=texelFetch(motionImage,q,0);
        // Expand invalidation by one pixel around smoke/unsupported coverage.
        float d=texelFetch(sceneDepth,q,0).r;
        reactive = reactive || (m.w < -.5 && d < .99999);
        if (d<nearest) { nearest=d; motion=m; }
    }
    mean/=9.;
    vec3 sigma=sqrt(max(moment/9.-mean*mean,vec3(0)));
    low=max(low,mean-1.25*sigma); high=min(high,mean+1.25*sigma);
    vec2 oldUV=texCoord+motion.xy;
    vec3 result=current;
    bool valid=aa.control.z>.5 && !reactive && motion.w>.5 && motion.z>0. &&
        all(greaterThan(oldUV,aa.metrics.xy*.5)) && all(lessThan(oldUV,vec2(1)-aa.metrics.xy*.5));
    if (valid) {
        vec4 old=texture(historyImage,oldUV);
        // Previous depth is measured in the previous camera, including object motion.
        valid=abs(old.a-motion.z)<max(.05,.015*motion.z);
        if (valid) {
            vec3 clipped=clamp(toYCoCg(old.rgb),low,high);
            float speed=length(motion.xy*aa.metrics.zw);
            float weight=mix(.91,.75,clamp(speed/24.,0.,1.));
            if (motion.w<.9) weight=min(weight,.85);
            float difference=abs(clipped.x-toYCoCg(current).x);
            weight*=1.-.6*clamp(difference/max(.05,high.x-low.x),0.,1.);
            result=mix(current,toRGB(clipped),weight);
        }
    }
    // Bounded detail restoration derived from current, not historical gradients.
    result=clamp(result+.08*(current-toRGB(mean)),toRGB(vec3(0,0,0)),vec3(1));
    if (aa.control.w>.5)
        result=reactive || motion.w<.5 ? vec3(1,0,1) : vec3(.5+motion.x*80.,.5+motion.y*80.,.5);
    // Negative depth marks unreliable color too: when smoke/water moves away,
    // the following frame must not reuse that color as opaque surface history.
    outColor=vec4(result,min(depth,65000.) * (reactive || motion.w<.5 ? -1. : 1.));
}

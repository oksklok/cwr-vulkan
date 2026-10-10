layout(set=3, binding=0) uniform sampler2DArray shadowMap;
// GL33's tier selection, coverage fallthrough, 3x3 PCF and transition bands.
float sunlightVisibility(vec3 relativeWorld, float fog) {
    if (lighting.shadowReceiver.x < 0.5 || lighting.cascadeControl.x < 0.5) return 1.0;
    int count = int(lighting.cascadeControl.x);
    int omni = int(lighting.cascadeControl.w);
    float eyeDepth = dot(relativeWorld, lighting.shadowForward.xyz);
    float distance3D = length(relativeWorld);
    int first = count;
    for (int i=0; i<4; ++i) {
        if (i >= count) break;
        if ((i < omni ? distance3D : eyeDepth) <= lighting.cascadeSplits[i]) { first=i; break; }
    }
    if (first == count) return 1.0;
    float previous = first > 0 ? lighting.cascadeSplits[first-1] : 0.0;
    float band = (lighting.cascadeSplits[first] - previous) * 0.15;
    float metric = first < omni ? distance3D : eyeDepth;
    float blend = first+1 < count ? clamp((metric - lighting.cascadeSplits[first] + band) / max(band, 0.001), 0.0, 1.0) : 0.0;
    float litSum=0.0, weightSum=0.0;
    for (int p=0; p<4; ++p) {
        int c=first+p;
        if (c >= count) break;
        float weight = p==0 ? 1.0-blend : (weightSum<=0.0 ? 1.0 : (p==1 ? blend : 0.0));
        if (weight<=0.0) continue;
        vec4 projected=lighting.cascadeVP[c] * vec4(relativeWorld, 1.0);
        vec3 sc=projected.xyz / projected.w;
        vec2 uv=sc.xy * vec2(0.5, -0.5) + 0.5;
        if (all(greaterThan(uv,vec2(0.0))) && all(lessThan(uv,vec2(1.0))) && sc.z>0.0 && sc.z<1.0) {
            float bias=lighting.cascadeControl.z * float((c+1)*(c+1));
            float lit=0.0;
            for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x)
                lit += sc.z-bias > texture(shadowMap,vec3(uv+vec2(x,y)*lighting.shadowStrength.y,float(c))).r ? 0.0 : 1.0;
            litSum += weight * lit / 9.0;
            weightSum += weight;
        }
    }
    if (weightSum<=0.0) return 1.0;
    float fade=clamp((lighting.cascadeSplits[count-1]-eyeDepth)/max(lighting.cascadeControl.y,0.001),0.0,1.0);
    return mix(1.0,lighting.shadowStrength.x,(1.0-litSum/weightSum)*fade*clamp(fog,0.0,1.0));
}

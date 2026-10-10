layout(push_constant) uniform AA { vec4 metrics; vec4 outputMetrics; } aa;
#define SMAA_RT_METRICS aa.metrics
#define SMAA_GLSL_4 1
#define SMAA_PRESET_HIGH 1
#include "../ThirdParty/SMAA/SMAA.hlsl"

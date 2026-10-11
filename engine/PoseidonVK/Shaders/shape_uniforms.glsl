struct ShapeLocalLight { vec4 position; vec4 direction; vec4 diffuse; vec4 ambient; };
layout(set = 2, binding = 0, std140) uniform ShapeLighting {
    mat4 world;
    mat3 normalMatrix;
    vec4 sunDirection;
    vec4 ambient;
    vec4 diffuse;
    vec4 emissive;
    vec4 specular; // Sun diffuse * material specular RGB, material power.
    vec4 fogParams;
    vec4 fogColor;
    vec4 eyeCoef;
    vec4 localCount;
    ShapeLocalLight localLights[8];
    vec4 shadowReceiver;
    mat4 cascadeVP[4];
    vec4 cascadeSplits;
    vec4 cascadeControl;
    vec4 shadowForward;
    vec4 shadowStrength;
    mat4 previousMVP;
    vec4 temporal;
    vec4 temporalExtent;
} lighting;

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
} lighting;

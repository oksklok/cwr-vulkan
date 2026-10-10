layout(set = 2, binding = 0, std140) uniform ShapeLighting {
    mat4 world;
    mat3 normalMatrix;
    vec4 sunDirection;
    vec4 ambient;
    vec4 diffuse;
    vec4 emissive;
} lighting;

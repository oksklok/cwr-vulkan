# Anti-aliasing reference sources

SMAA 1x uses the complete reference algorithm, HIGH preset, color edge detection,
diagonal/corner handling and the original AreaTex/SearchTex bytes. Source:
https://github.com/iryoku/smaa/tree/71c806a838bdd7d517df19192a20f0c61b3ca29d
The algorithm and tables are unmodified; wrappers select the GLSL path and
positive-height Vulkan viewport convention. The MIT license is in
`SMAA/LICENSE.txt` and in each source file. Tables expand losslessly to RGBA8
for the existing texture uploader; sampling uses the original R/G channels.

FXAA 3.11 is NVIDIA's quality algorithm, preset 29, subpixel factor 0.5,
contrast threshold 0.125 and dark threshold 0.0312. Retrieved from Intel's
CMAA2 comparison sample (which includes NVIDIA's BSD license):
https://github.com/GameTechDev/CMAA2/blob/master/Projects/CMAA2/FXAA/Fxaa3_11.h
The sole local header adaptation, `CWR_FXAA_RGB_LUMA`, computes RGB luminance
at the reference algorithm's existing sample sites. This avoids a separate
luma image/pass and includes chromatic edges instead of using green as luma.
The edge search and subpixel filter are unchanged. Neither reference's temporal
or console paths are enabled.

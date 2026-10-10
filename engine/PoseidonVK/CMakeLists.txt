# Vulkan is opt-in. GL33-only builds never search for, or link, the SDK.
find_package(Vulkan REQUIRED)
if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR "Vulkan shaders require glslangValidator from the installed Vulkan SDK")
endif()
set(VK_SHADER_HEADERS)
foreach(kind triangle shape screen shadow)
    foreach(stage vert frag)
        if(kind STREQUAL "screen" AND stage STREQUAL "frag")
            continue() # Screen vertices reuse the diffuse fragment shader.
        endif()
        if(kind STREQUAL "shadow" AND stage STREQUAL "vert")
            continue() # Native/screen vertices retain the engine's projection.
        endif()
        set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/PoseidonVK/Shaders/${kind}.${stage}.hpp")
        add_custom_command(OUTPUT "${header}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/PoseidonVK/Shaders"
            COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.0
                --vn "Cwr${kind}_${stage}" -o "${header}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/${kind}.${stage}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/${kind}.${stage}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/shape_uniforms.glsl" VERBATIM)
        list(APPEND VK_SHADER_HEADERS "${header}")
    endforeach()
endforeach()
add_custom_target(PoseidonVKShaders DEPENDS ${VK_SHADER_HEADERS})

add_library(PoseidonVK STATIC VulkanContext.cpp VulkanContext_Triangle.cpp VulkanContext_Mesh.cpp VulkanContext_Texture.cpp TextureVK.cpp BufferVK.cpp VertexBufferVK.cpp
    EngineVK.cpp EngineVK_Mesh.cpp EngineVK_2D.cpp EngineVK_Software.cpp EngineVK_Unsupported.cpp GraphicsBackendVK.cpp)
add_dependencies(PoseidonVK PoseidonVKShaders)
target_include_directories(PoseidonVK PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(PoseidonVK PUBLIC Poseidon Vulkan::Vulkan)
if(WIN32)
    target_compile_definitions(PoseidonVK PRIVATE __MSC__=1100 _WINDOWS)
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(PoseidonVK PRIVATE /GR /permissive
        $<$<CXX_COMPILER_ID:Clang>:-Wno-error=non-pod-varargs>
        $<$<CXX_COMPILER_ID:Clang>:-Wno-error=c++11-narrowing>)
endif()

# Driver-free policy test: also builds directly with MSVC and SDK headers when
# the full engine dependency toolchain is unavailable.
add_executable(PoseidonVKPolicyTests VulkanContext.cpp VulkanContext_Triangle.cpp VulkanContext_Mesh.cpp VulkanContext_Texture.cpp BufferVK.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Graphics/test_vulkan_buffers.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Graphics/test_vulkan_policy.cpp)
target_include_directories(PoseidonVKPolicyTests PRIVATE ${CMAKE_SOURCE_DIR}/engine)
add_dependencies(PoseidonVKPolicyTests PoseidonVKShaders)
target_include_directories(PoseidonVKPolicyTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(PoseidonVKPolicyTests PRIVATE Vulkan::Vulkan)
add_test(NAME PoseidonVKPolicyTests COMMAND PoseidonVKPolicyTests)

# Vulkan is opt-in. GL33-only builds never search for, or link, the SDK.
find_package(Vulkan REQUIRED)

add_library(PoseidonVK STATIC VulkanContext.cpp EngineVK.cpp EngineVK_Unsupported.cpp GraphicsBackendVK.cpp)
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
add_executable(PoseidonVKPolicyTests VulkanContext.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Graphics/test_vulkan_policy.cpp)
target_include_directories(PoseidonVKPolicyTests PRIVATE ${CMAKE_SOURCE_DIR}/engine)
target_link_libraries(PoseidonVKPolicyTests PRIVATE Vulkan::Vulkan)
add_test(NAME PoseidonVKPolicyTests COMMAND PoseidonVKPolicyTests)

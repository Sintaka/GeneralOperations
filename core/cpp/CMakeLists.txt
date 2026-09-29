# core/cpp —— 零 Qt 的 C++ 内核与独立后端。
# project() 与语言标准由仓库根统一声明；本目录也可独立 configure。
cmake_minimum_required(VERSION 3.16)

# 既有头文件纯函数保持 INTERFACE，调用方 target 名不变。
add_library(go_core_cpp INTERFACE)
target_include_directories(go_core_cpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(go_core_cpp_tests tests/go_core_cpp_tests.cpp)
target_link_libraries(go_core_cpp_tests PRIVATE go_core_cpp)
target_compile_features(go_core_cpp_tests PRIVATE cxx_std_17)

# Shared, UI-independent parser for Python script declaration docstrings.
add_library(go_script_manifest_core STATIC src/manifest/catalog.cpp)
target_include_directories(go_script_manifest_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinygltf
)
target_compile_features(go_script_manifest_core PUBLIC cxx_std_17)

add_executable(go_script_catalog src/manifest/main.cpp)
target_link_libraries(go_script_catalog PRIVATE go_script_manifest_core)
target_include_directories(go_script_catalog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinygltf
)
target_compile_features(go_script_catalog PRIVATE cxx_std_17)
if(MINGW)
    target_link_options(go_script_catalog PRIVATE -municode)
endif()

add_executable(go_script_manifest_tests tests/go_script_manifest_tests.cpp)
target_link_libraries(go_script_manifest_tests PRIVATE go_script_manifest_core)
target_include_directories(go_script_manifest_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinygltf
)
target_compile_features(go_script_manifest_tests PRIVATE cxx_std_17)
target_compile_definitions(go_script_manifest_tests PRIVATE
    GO_SCRIPT_MANIFEST_SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/core/python/scripts"
)
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    # GCC 8's <filesystem> is unusable; nlohmann/json exposes its optional
    # filesystem adapters unless C++14 support is selected explicitly.
    foreach(_target go_script_manifest_core go_script_catalog go_script_manifest_tests)
        target_compile_definitions(${_target} PRIVATE JSON_HAS_CPP_14)
    endforeach()
endif()

# PMX 解析、贴图处理与 GLB 组装复用同一个静态库；CLI 和测试都只链接公开接口。
add_library(go_pmx2glb_core STATIC
    src/pmx2glb/pmx.cpp
    src/pmx2glb/glb.cpp
    src/pmx2glb/texture.cpp
    3rdparty/tinygltf/tiny_gltf.cc
)
target_include_directories(go_pmx2glb_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pmx2glb
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinygltf
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/stb
)
target_compile_features(go_pmx2glb_core PUBLIC cxx_std_17)
target_compile_definitions(go_pmx2glb_core PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(go_pmx2glb src/pmx2glb/main.cpp)
target_link_libraries(go_pmx2glb PRIVATE go_pmx2glb_core)
target_compile_features(go_pmx2glb PRIVATE cxx_std_17)
if(MINGW)
    # wmain 让 argv 保持 UTF-16，日文 PMX 路径不会先被本地代码页破坏。
    target_link_options(go_pmx2glb PRIVATE -municode)
endif()

add_executable(go_pmx2glb_tests tests/go_pmx_parser_tests.cpp)
target_link_libraries(go_pmx2glb_tests PRIVATE go_pmx2glb_core)
target_include_directories(go_pmx2glb_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pmx2glb
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinygltf
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/stb
)
target_compile_features(go_pmx2glb_tests PRIVATE cxx_std_17)
target_compile_definitions(go_pmx2glb_tests PRIVATE
    GO_PMX2GLB_EXE_PATH="$<TARGET_FILE:go_pmx2glb>"
)
add_dependencies(go_pmx2glb_tests go_pmx2glb)

if(MSVC)
    foreach(_target go_core_cpp_tests go_script_manifest_core go_script_catalog go_script_manifest_tests go_pmx2glb_core go_pmx2glb go_pmx2glb_tests)
        target_compile_options(${_target} PRIVATE /utf-8 /permissive- /W4 /wd4127)
    endforeach()
else()
    foreach(_target go_core_cpp_tests go_script_manifest_core go_script_catalog go_script_manifest_tests go_pmx2glb_core go_pmx2glb go_pmx2glb_tests)
        target_compile_options(${_target} PRIVATE -Wall -Wextra)
    endforeach()
endif()

enable_testing()
add_test(NAME go_core_cpp COMMAND go_core_cpp_tests)
add_test(NAME go_script_manifest COMMAND go_script_manifest_tests)
add_test(NAME go_pmx2glb COMMAND go_pmx2glb_tests)

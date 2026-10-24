# NeoX Phase 1 — 核心运行时 + Soft/GL 后端 / 测试 / 示例
cmake_minimum_required(VERSION 3.16)
project(neox C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NEOX_SANITIZE "Build with -fsanitize=address,undefined" OFF)
if(APPLE)
  option(NEOX_BACKEND_GL "OpenGL backend (macOS CGL offscreen + GL 4.1 Core)" ON)
else()
  option(NEOX_BACKEND_GL "OpenGL backend (macOS CGL offscreen + GL 4.1 Core)" OFF)
endif()

add_library(neox STATIC src/core.c src/geom.c src/soft.c)
target_include_directories(neox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(neox PRIVATE -Wall -Wextra)

set(NEOX_TARGETS neox test_semantics ex01_scene)

# GPU 后端：仅 macOS（OpenGL.framework）；gl.c 使用已废弃 API（ intentional，
# 本后端定位为链路验证，移植目标为 GLES3/WebGL2）
if(NEOX_BACKEND_GL AND APPLE)
  target_sources(neox PRIVATE src/gl.c)
  target_compile_definitions(neox PRIVATE NEOX_ENABLE_GL=1)
  target_link_libraries(neox PRIVATE "-framework OpenGL")
  set_source_files_properties(src/gl.c PROPERTIES
    COMPILE_OPTIONS "-Wno-deprecated-declarations")
  set(NEOX_GL_ENABLED 1)
else()
  set(NEOX_GL_ENABLED 0)
endif()

# 示例：输出 BMP 到构建目录（ctest 可顺带验证示例跑通）
add_executable(ex01_scene examples/ex01_scene.c)
target_link_libraries(ex01_scene PRIVATE neox m)
target_compile_options(ex01_scene PRIVATE -Wall -Wextra)
set_target_properties(ex01_scene PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

enable_testing()
add_executable(test_semantics tests/test_semantics.c)
target_link_libraries(test_semantics PRIVATE neox m)
target_compile_options(test_semantics PRIVATE -Wall -Wextra)
add_test(NAME semantics COMMAND test_semantics)

# 示例作为冒烟测试：跑通并产出 BMP
add_test(NAME ex01_smoke COMMAND ex01_scene)
set_tests_properties(ex01_smoke PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(NEOX_GL_ENABLED)
  add_executable(test_gpu_consistency tests/test_gpu_consistency.c)
  target_link_libraries(test_gpu_consistency PRIVATE neox m)
  target_compile_options(test_gpu_consistency PRIVATE -Wall -Wextra)
  set_target_properties(test_gpu_consistency PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME gpu_consistency COMMAND test_gpu_consistency)

  add_executable(ex02_gpuscene examples/ex02_gpuscene.c)
  target_link_libraries(ex02_gpuscene PRIVATE neox m)
  target_compile_options(ex02_gpuscene PRIVATE -Wall -Wextra)
  set_target_properties(ex02_gpuscene PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME ex02_smoke COMMAND ex02_gpuscene)
  set_tests_properties(ex02_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  list(APPEND NEOX_TARGETS test_gpu_consistency ex02_gpuscene)
endif()

if(NEOX_SANITIZE)
  foreach(t ${NEOX_TARGETS})
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

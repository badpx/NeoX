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
  option(NEOX_BACKEND_METAL "Metal backend (macOS native, Objective-C++)" ON)
elseif(UNIX)
  option(NEOX_BACKEND_GL "OpenGL backend (Linux EGL surfaceless + GL 4.1 Core)" ON)
  option(NEOX_BACKEND_METAL "Metal backend (macOS native, Objective-C++)" OFF)
else()
  option(NEOX_BACKEND_GL "OpenGL backend" OFF)
  option(NEOX_BACKEND_METAL "Metal backend (macOS native, Objective-C++)" OFF)
endif()

add_library(neox STATIC src/core.c src/geom.c src/soft.c)
target_include_directories(neox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(neox PRIVATE -Wall -Wextra)

set(NEOX_TARGETS neox test_semantics ex01_scene)

# GPU 后端：gl.c 是平台无关的 GL 4.1 Core 执行器。上下文与入口按平台
# 选编译单元（docs/linux-port.md）：macOS = CGL + 框架静态链接；
# Linux = EGL surfaceless + 运行期 dlopen。定位仍是链路验证。
if(NEOX_BACKEND_GL)
  if(APPLE)
    target_sources(neox PRIVATE src/gl.c src/gl_ctx_cgl.c)
    target_compile_definitions(neox PUBLIC NEOX_ENABLE_GL=1)
    target_link_libraries(neox PRIVATE "-framework OpenGL")
    set_source_files_properties(src/gl.c src/gl_ctx_cgl.c PROPERTIES
      COMPILE_OPTIONS "-Wno-deprecated-declarations")
    set(NEOX_GL_ENABLED 1)
  elseif(UNIX)
    target_sources(neox PRIVATE src/gl.c src/gl_ctx_egl.c src/gl_load.c)
    target_compile_definitions(neox PUBLIC NEOX_ENABLE_GL=1)
    target_link_libraries(neox PRIVATE ${CMAKE_DL_LIBS})
    set(NEOX_GL_ENABLED 1)
  else()
    message(FATAL_ERROR "NEOX_BACKEND_GL is not implemented on this platform")
  endif()
else()
  set(NEOX_GL_ENABLED 0)
endif()

# Metal 后端：仅 macOS；唯一 ObjC++ 文件，MRC（C 结构内手动管理 MTL 对象）
if(NEOX_BACKEND_METAL AND APPLE)
  enable_language(OBJCXX)
  target_sources(neox PRIVATE src/metal.mm)
  target_compile_definitions(neox PUBLIC NEOX_ENABLE_METAL=1)
  target_link_libraries(neox PRIVATE "-framework Metal" "-framework Foundation"
                        "-framework QuartzCore")
  set_source_files_properties(src/metal.mm PROPERTIES
    COMPILE_OPTIONS "-fno-objc-arc;-Wno-deprecated-declarations")
  set(NEOX_MTL_ENABLED 1)
else()
  set(NEOX_MTL_ENABLED 0)
endif()

# 窗口呈现是平台胶水，不是 GL 执行器。Linux 上 X11 与 Wayland 都编进来，
# 运行期 dlopen，不把 libX11 / libwayland 写进链接行。
if(UNIX AND NOT APPLE)
  option(NEOX_PRESENT "Linux window presentation (Wayland, X11 fallback)" ON)
else()
  option(NEOX_PRESENT "Linux window presentation (Wayland, X11 fallback)" OFF)
endif()
if(NEOX_PRESENT AND (NOT UNIX OR APPLE))
  message(FATAL_ERROR "NEOX_PRESENT is implemented for Linux only")
endif()
if(NEOX_PRESENT)
  target_sources(neox PRIVATE src/present.c src/present_x11.c src/present_wl.c)
  target_compile_definitions(neox PUBLIC NEOX_ENABLE_PRESENT=1)
  target_link_libraries(neox PRIVATE ${CMAKE_DL_LIBS})
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
  # 关掉 dual-source，强制 ping-pong 回退（与 Metal 的 NEOX_MTL_FORCE_NO_DUAL 同款）
  add_test(NAME gpu_consistency_nodual COMMAND test_gpu_consistency)
  set_tests_properties(gpu_consistency_nodual PROPERTIES
    ENVIRONMENT "NEOX_GL_FORCE_NO_DUAL=1")

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

if(NEOX_MTL_ENABLED)
  add_executable(ex03_mtlscene examples/ex03_mtlscene.c)
  target_link_libraries(ex03_mtlscene PRIVATE neox m)
  target_compile_options(ex03_mtlscene PRIVATE -Wall -Wextra)
  set_target_properties(ex03_mtlscene PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME ex03_smoke COMMAND ex03_mtlscene)
  set_tests_properties(ex03_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  list(APPEND NEOX_TARGETS ex03_mtlscene)
endif()

# NeoX-Text 模块（字体加载/排版/动态 A8 图集；stb_truetype 单头文件 vendor）
option(NEOX_WITH_TEXT "NeoX-Text module (font loading, layout, A8 atlas)" ON)
if(NEOX_WITH_TEXT)
  add_library(neox_text STATIC text/text.c)
  target_include_directories(neox_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/text
                             PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                     ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
  target_link_libraries(neox_text PUBLIC neox)
  target_compile_options(neox_text PRIVATE -Wall -Wextra)

  enable_testing()
  add_executable(test_text tests/test_text.c)
  target_link_libraries(test_text PRIVATE neox_text m)
  target_compile_options(test_text PRIVATE -Wall -Wextra)
  set_target_properties(test_text PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME text COMMAND test_text)

  add_executable(ex04_text examples/ex04_text.c)
  target_link_libraries(ex04_text PRIVATE neox_text m)
  target_compile_options(ex04_text PRIVATE -Wall -Wextra)
  set_target_properties(ex04_text PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME ex04_smoke COMMAND ex04_text)
  set_tests_properties(ex04_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  list(APPEND NEOX_TARGETS neox_text test_text ex04_text)
endif()

if(NEOX_PRESENT)
  add_executable(test_present tests/test_present.c)
  target_link_libraries(test_present PRIVATE neox m)
  target_compile_options(test_present PRIVATE -Wall -Wextra)
  set_target_properties(test_present PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME present COMMAND test_present)

  add_executable(ex05_window examples/ex05_window.c)
  target_link_libraries(ex05_window PRIVATE neox m)
  target_compile_options(ex05_window PRIVATE -Wall -Wextra)
  set_target_properties(ex05_window PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME ex05_smoke COMMAND ex05_window)
  set_tests_properties(ex05_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    ENVIRONMENT "NEOX_PRESENT_FRAMES=2")

  list(APPEND NEOX_TARGETS test_present ex05_window)
endif()

if(NEOX_SANITIZE)
  foreach(t ${NEOX_TARGETS})
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

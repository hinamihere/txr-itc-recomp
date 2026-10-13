cmake_minimum_required(VERSION 3.20)
project(itc_recomp CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED XR)
  set(XR C:/itc-xr)
endif()
set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/../out)
set(XRBUILD C:/itc-xr/build_ascii)

set(COMMON_INCLUDES ${GEN} src ${XR}/thirdparty/simde ${XR}/XenonUtils)
set(COMMON_OPTS -O1 -msse4.1 -fno-strict-aliasing
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../gcc_compat.h)

# --- itc_host.dll: everything hand-written (fast to rebuild/relink) ---------------------------
add_library(itc_host SHARED src/main.cpp src/kernel.cpp src/kernel_sync.cpp src/kernel_io.cpp src/kernel_xam.cpp src/gpu.cpp src/kernel_audio.cpp src/hooks.cpp src/stubs.cpp src/xgpu_shader.cpp src/xgpu_common.cpp src/xgpu_d3d.cpp src/xgpu_vk.cpp src/prof.cpp src/xma.cpp)
target_include_directories(itc_host PRIVATE ${COMMON_INCLUDES})
# Vulkan renderer: Khronos headers (the loader vulkan-1.dll is loaded at runtime) and the DirectX Shader Compiler API (dxcompiler.dll, HLSL -> SPIR-V, loaded at runtime).
target_include_directories(itc_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../deps/vkheaders/Vulkan-Headers-1.3.290/include ${CMAKE_CURRENT_SOURCE_DIR}/../deps/dxc/inc)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg/include)
    target_include_directories(itc_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg/include)   # FFmpeg headers (libavcodec XMA2 decoder, loaded dynamically at runtime)
endif()
target_compile_options(itc_host PRIVATE ${COMMON_OPTS} -O2)   # the hand-written host code (GPU emulation, kernel) is the hot path: optimise it
target_link_libraries(itc_host PRIVATE
    ${XRBUILD}/XenonUtils/libXenonUtils.a
    ${XRBUILD}/thirdparty/disasm/libdisasm.a
    ${XRBUILD}/thirdparty/fmt/libfmt.a
    d3d11 dxgi d3dcompiler uuid gdi32 user32 winmm)
target_link_options(itc_host PRIVATE -static-libgcc -static-libstdc++ -Wl,--export-all-symbols)
set_target_properties(itc_host PROPERTIES PREFIX "")
set_source_files_properties(src/xgpu_d3d.cpp src/xgpu_vk.cpp PROPERTIES COMPILE_OPTIONS "-msse4.2;-O3")

# --- itc.exe: the recompiled game (slow to build, rarely changes) ------------------------------
file(GLOB GEN_SRC ${GEN}/ppc_recomp.*.cpp)
add_executable(itc ${GEN_SRC} ${GEN}/ppc_func_mapping.cpp src/exe_main.cpp)
target_include_directories(itc PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(itc PRIVATE ITC_TRACE=1)
target_compile_options(itc PRIVATE ${COMMON_OPTS} -O2)   # -O2 for the game code as well (guest-side frame time; -O1 was the bring-up setting)
# Stable import library for the DLL (built from itc_host.def) so the exe does not relink when the DLL changes.
set(HOST_IMPLIB ${CMAKE_BINARY_DIR}/libitc_host_stable.a)
add_custom_command(OUTPUT ${HOST_IMPLIB}
    COMMAND dlltool -d ${CMAKE_CURRENT_SOURCE_DIR}/itc_host.def -l ${HOST_IMPLIB} -D itc_host.dll
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/itc_host.def)
add_custom_target(host_implib DEPENDS ${HOST_IMPLIB})
add_dependencies(itc host_implib itc_host)
target_link_libraries(itc PRIVATE ${HOST_IMPLIB})
target_link_options(itc PRIVATE -static-libgcc -static-libstdc++ -Wl,--stack,8388608)

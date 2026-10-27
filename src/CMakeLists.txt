cmake_minimum_required(VERSION 3.20)
project(CrysisOptiPerf LANGUAGES C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "optiperf targets the MSVC ABI (thiscall/fastcall interop with the game); build with Visual Studio")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: the plugin is loaded into the game process, which must not need any extra runtime DLLs
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- third-party (pinned) -------------------------------------------------------------------------------------------
include(FetchContent)
# mimalloc: replacement for CryEngine's allocator (thread-scalable, no 32-bit pointer packing). Static, no overrides.
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_WIN_REDIRECT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mimalloc GIT_REPOSITORY https://github.com/microsoft/mimalloc.git GIT_TAG v2.5.2 GIT_SHALLOW TRUE)
# MinHook: function hooks with trampolines (needed to call the original allocator; handles x86 and x64).
# Only the sources are fetched (SOURCE_SUBDIR points nowhere); it is built below as a static library.
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG v1.3.4 GIT_SHALLOW TRUE
  SOURCE_SUBDIR _sources_only)
FetchContent_MakeAvailable(mimalloc minhook)
add_library(minhook_static STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde32.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook_static PUBLIC ${minhook_SOURCE_DIR}/include)

# --- optiperf -------------------------------------------------------------------------------------------------------
# x64 needs one assembly stub (the call tracer's entry; x86 uses inline assembly)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  enable_language(ASM_MASM)
  set(OPTIPERF_ASM optiperf/calltrace_x64.asm)
endif()
set(OPTIPERF_CXX_FLAGS $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /EHsc>)
set(OPTIPERF_CORE
  optiperf/allocator.cpp
  optiperf/allocator_owners.cpp
  optiperf/calltrace.cpp
  ${OPTIPERF_ASM}
  optiperf/census.cpp
  optiperf/config.cpp
  optiperf/console_gate.cpp
  optiperf/cvar_snapshot.cpp
  optiperf/diag.cpp
  optiperf/engine_data.cpp
  optiperf/env_snapshot.cpp
  optiperf/flush_thread.cpp
  optiperf/fog_snapshot.cpp
  optiperf/frame_phases.cpp
  optiperf/hooks.cpp
  optiperf/light_snapshot.cpp
  optiperf/log.cpp
  optiperf/material_snapshot.cpp
  optiperf/ocean_snapshot.cpp
  optiperf/scan.cpp
  optiperf/particle_batch.cpp
  optiperf/particle_prepass.cpp
  optiperf/producer_timing.cpp
  optiperf/reflection_prepass.cpp
  optiperf/render_gate.cpp
  optiperf/render_queue.cpp
  optiperf/shadow_prepass.cpp
  optiperf/skin_prepass.cpp
  optiperf/sys_snapshot.cpp
  optiperf/traversal_record.cpp
)

# The ASI plugin: a DLL with the .asi extension, loaded by an ASI loader (or injected by optiperf_launch.exe)
add_library(optiperf SHARED optiperf/dllmain.cpp ${OPTIPERF_CORE})
target_compile_definitions(optiperf PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(optiperf PRIVATE ${OPTIPERF_CXX_FLAGS})
target_link_libraries(optiperf PRIVATE mimalloc-static minhook_static)
set_target_properties(optiperf PROPERTIES SUFFIX ".asi")
add_custom_command(TARGET optiperf POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/optiperf/optiperf.ini $<TARGET_FILE_DIR:optiperf>/optiperf.ini)

# Development launcher (injects optiperf.asi without an ASI loader)
add_executable(optiperf_launch launcher/main.cpp)
target_compile_definitions(optiperf_launch PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(optiperf_launch PRIVATE /W4 /permissive- /utf-8 /EHsc)

# Allocator replacement outside the game (both architectures)
enable_testing()
add_executable(allocator_test tests/allocator_test.cpp ${OPTIPERF_CORE})
target_compile_definitions(allocator_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(allocator_test PRIVATE ${OPTIPERF_CXX_FLAGS})
target_link_libraries(allocator_test PRIVATE mimalloc-static minhook_static)
add_test(NAME allocator COMMAND allocator_test)

# Call tracer outside the game: traces a small DLL with RTTI'd classes
add_library(calltrace_testlib SHARED tests/calltrace_testlib.cpp)
target_compile_options(calltrace_testlib PRIVATE /W4 /permissive- /utf-8 /EHsc /GR)
add_executable(calltrace_test tests/calltrace_test.cpp ${OPTIPERF_CORE})
target_compile_definitions(calltrace_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(calltrace_test PRIVATE ${OPTIPERF_CXX_FLAGS})
target_link_libraries(calltrace_test PRIVATE mimalloc-static minhook_static)
add_dependencies(calltrace_test calltrace_testlib)
target_link_options(calltrace_test PRIVATE /MAP)
add_test(NAME calltrace COMMAND calltrace_test)

# Differential test against the game's own renderer code (needs the game's renderer DLLs of the same architecture)
add_executable(particle_batch_test tests/particle_batch_test.cpp ${OPTIPERF_CORE})
target_compile_definitions(particle_batch_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(particle_batch_test PRIVATE ${OPTIPERF_CXX_FLAGS})
target_link_libraries(particle_batch_test PRIVATE mimalloc-static minhook_static)
add_test(NAME particle_batch COMMAND particle_batch_test)

# Shadow pre-pass: list handling on synthetic data, and locating its code in the game's x64 DX10 renderer
add_executable(shadow_prepass_test tests/shadow_prepass_test.cpp ${OPTIPERF_CORE})
target_compile_definitions(shadow_prepass_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(shadow_prepass_test PRIVATE ${OPTIPERF_CXX_FLAGS})
target_link_libraries(shadow_prepass_test PRIVATE mimalloc-static minhook_static)
add_test(NAME shadow_prepass COMMAND shadow_prepass_test)

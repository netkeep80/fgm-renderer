cmake_minimum_required(VERSION 3.24)
project(fgm_boot LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(fgm_core
  GIT_REPOSITORY https://github.com/netkeep80/fgm-core.git
  GIT_TAG ba91a5e96d9f4f9580c9b2e9a9460b97acb7f01b
)
set(BUILD_TESTING OFF)
FetchContent_MakeAvailable(fgm_core)

set(BOOT_SIZE 128)
configure_file(src/scene-size.hpp.in generated/scene-size.hpp @ONLY)
add_library(boot_scene STATIC src/scene.cpp)
target_compile_features(boot_scene PUBLIC cxx_std_20)
target_include_directories(boot_scene PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(boot_scene PRIVATE fgm::cpu)

if(EMSCRIPTEN)
  add_executable(fgm_scene src/wasm.cpp)
  target_link_libraries(fgm_scene PRIVATE boot_scene)
  # Исключения обрабатываются внутри границы WASM.
  target_compile_options(fgm_cpu PRIVATE -fexceptions)
  target_compile_options(boot_scene PRIVATE -fexceptions)
  target_compile_options(fgm_scene PRIVATE -fexceptions)
  target_link_options(fgm_scene PRIVATE -fexceptions --no-entry
    "-sMODULARIZE=1" "-sEXPORT_ES6=1" "-sENVIRONMENT=web"
    "-sEXPORTED_RUNTIME_METHODS=['HEAPU8']"
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_fgm_sample_scene','_fgm_scene_size']"
    "-sALLOW_MEMORY_GROWTH=1")
  set_target_properties(fgm_scene PROPERTIES SUFFIX ".mjs"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../web")
else()
  set(WGPU_ROOT "" CACHE PATH "Распакованный архив wgpu-native v29.0.1.1")
  find_path(WGPU_INCLUDE webgpu/wgpu.h PATHS "${WGPU_ROOT}/include" REQUIRED NO_DEFAULT_PATH)
  if(WIN32)
    find_file(WGPU_LIBRARY NAMES wgpu_native.dll.lib PATHS "${WGPU_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
  else()
    find_library(WGPU_LIBRARY NAMES wgpu_native PATHS "${WGPU_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
  endif()
  add_executable(fgm_boot src/native.cpp)
  target_include_directories(fgm_boot PRIVATE "${WGPU_INCLUDE}")
  target_link_libraries(fgm_boot PRIVATE boot_scene "${WGPU_LIBRARY}")
  if(WIN32)
    add_custom_command(TARGET fgm_boot POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${WGPU_ROOT}/lib/wgpu_native.dll" "$<TARGET_FILE_DIR:fgm_boot>")
  endif()
endif()

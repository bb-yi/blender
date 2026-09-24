cmake_minimum_required(VERSION 3.24)
project(dlss5_vulkan_d3d12_interop_probe LANGUAGES CXX)

set(VULKAN_ROOT "" CACHE PATH "Vulkan SDK root containing include/ and lib/")
if(NOT VULKAN_ROOT)
  message(FATAL_ERROR "Pass -DVULKAN_ROOT=<Vulkan SDK root>")
endif()

find_path(VULKAN_INCLUDE_DIR
  NAMES vulkan/vulkan.h
  PATHS "${VULKAN_ROOT}/include"
  NO_DEFAULT_PATH
  REQUIRED
)

find_library(VULKAN_LIBRARY
  NAMES vulkan-1 vulkan
  PATHS "${VULKAN_ROOT}/lib"
  NO_DEFAULT_PATH
  REQUIRED
)

add_executable(dlss5_vulkan_d3d12_interop_probe vulkan_d3d12_interop_probe.cpp)
target_compile_features(dlss5_vulkan_d3d12_interop_probe PRIVATE cxx_std_17)
target_compile_definitions(dlss5_vulkan_d3d12_interop_probe PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)
target_include_directories(dlss5_vulkan_d3d12_interop_probe PRIVATE "${VULKAN_INCLUDE_DIR}")
target_link_libraries(dlss5_vulkan_d3d12_interop_probe PRIVATE
  d3d12
  dxgi
  "${VULKAN_LIBRARY}"
)

if(MSVC)
  target_compile_options(dlss5_vulkan_d3d12_interop_probe PRIVATE /W4 /EHsc)
endif()

add_executable(dlss5_vulkan_d3d12_shared_resource_test
  vulkan_d3d12_shared_resource_test.cpp
)
target_compile_features(dlss5_vulkan_d3d12_shared_resource_test PRIVATE cxx_std_17)
target_compile_definitions(dlss5_vulkan_d3d12_shared_resource_test PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)
target_include_directories(dlss5_vulkan_d3d12_shared_resource_test PRIVATE "${VULKAN_INCLUDE_DIR}")
target_link_libraries(dlss5_vulkan_d3d12_shared_resource_test PRIVATE
  d3d12
  dxgi
  "${VULKAN_LIBRARY}"
)

if(MSVC)
  target_compile_options(dlss5_vulkan_d3d12_shared_resource_test PRIVATE /W4 /EHsc)
endif()

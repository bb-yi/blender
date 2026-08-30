cmake_minimum_required(VERSION 3.24)
project(dlss5_host LANGUAGES CXX)

set(NGX_ROOT "" CACHE PATH "NVIDIA DLSS SDK root containing include/ and lib/")
if(NOT NGX_ROOT)
  message(FATAL_ERROR "Pass -DNGX_ROOT=<NVIDIA-DLSS checkout or extracted SDK>")
endif()

add_executable(dlss5_host dlss5_host.cpp)
target_compile_features(dlss5_host PRIVATE cxx_std_17)
target_compile_definitions(dlss5_host PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(dlss5_host PRIVATE "${NGX_ROOT}/include")
target_link_libraries(dlss5_host PRIVATE
  d3d12
  dxgi
  "${NGX_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
)

if(MSVC)
  target_compile_options(dlss5_host PRIVATE /W4 /EHsc)
endif()

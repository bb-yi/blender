function(configure_npr_shaderc)
  include(ExternalProject)
  find_package(Git REQUIRED)

  include(${CMAKE_SOURCE_DIR}/build_files/build_environment/cmake/versions.cmake)

  set(shaderc_patch "${CMAKE_SOURCE_DIR}/build_files/build_environment/patches/spirv_tools_6962.diff")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${shaderc_patch}")
  file(SHA256 "${shaderc_patch}" shaderc_patch_hash)
  string(SUBSTRING "${shaderc_patch_hash}" 0 12 shaderc_patch_hash)
  set(shaderc_build_root "${CMAKE_BINARY_DIR}/shaderc")

  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()

  foreach(dep SPIRV_HEADERS SPIRV_TOOLS SHADERC_GLSLANG)
    string(TOLOWER "${dep}" dep_name)
    set(dep_patch PATCH_COMMAND "")
    set(dep_source "${shaderc_build_root}/${dep_name}")
    if(dep STREQUAL "SPIRV_TOOLS")
      string(APPEND dep_source "-${shaderc_patch_hash}")
      set(dep_patch PATCH_COMMAND "${GIT_EXECUTABLE}" apply "${shaderc_patch}")
    endif()
    ExternalProject_Add(npr_${dep_name}
      PREFIX "${shaderc_build_root}/${dep_name}-project"
      SOURCE_DIR "${dep_source}"
      URL "${${dep}_URI}"
      URL_HASH "${${dep}_HASH_TYPE}=${${dep}_HASH}"
      DOWNLOAD_DIR "${shaderc_build_root}/downloads"
      DOWNLOAD_NAME "${${dep}_FILE}"
      ${dep_patch}
      CONFIGURE_COMMAND ""
      BUILD_COMMAND ""
      INSTALL_COMMAND ""
      EXCLUDE_FROM_ALL TRUE
    )
    set(shaderc_${dep}_source "${dep_source}")
  endforeach()

  set(shaderc_runtime_dir "${shaderc_build_root}/bin")
  set(shaderc_library "${shaderc_runtime_dir}/shaderc_shared.lib")
  string(REPLACE "$<$<CONFIG:Debug>:_d>" "" shaderc_python "${PYTHON_EXECUTABLE}")
  ExternalProject_Add(npr_shaderc
    PREFIX "${shaderc_build_root}/project"
    URL "${SHADERC_URI}"
    URL_HASH "${SHADERC_HASH_TYPE}=${SHADERC_HASH}"
    DOWNLOAD_DIR "${shaderc_build_root}/downloads"
    DOWNLOAD_NAME "${SHADERC_FILE}"
    DEPENDS npr_spirv_headers npr_spirv_tools npr_shaderc_glslang
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      -DCMAKE_CXX_FLAGS=/Zc:__cplusplus
      "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${shaderc_runtime_dir}"
      "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE=${shaderc_runtime_dir}"
      "-DPython_EXECUTABLE=${shaderc_python}"
      "-DPython3_EXECUTABLE=${shaderc_python}"
      -DSHADERC_SKIP_TESTS=ON
      -DSHADERC_SKIP_EXAMPLES=ON
      -DSHADERC_SKIP_EXECUTABLES=ON
      -DSHADERC_ENABLE_SHARED_CRT=ON
      "-DSHADERC_SPIRV_HEADERS_DIR=${shaderc_SPIRV_HEADERS_source}"
      "-DSHADERC_SPIRV_TOOLS_DIR=${shaderc_SPIRV_TOOLS_source}"
      "-DSHADERC_GLSLANG_DIR=${shaderc_SHADERC_GLSLANG_source}"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target shaderc_shared --parallel 4
    BUILD_BYPRODUCTS "${shaderc_library}" "${shaderc_runtime_dir}/shaderc_shared.dll"
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
  )

  set(SHADERC_RUNTIME_DIR "${shaderc_runtime_dir}" PARENT_SCOPE)
  set(SHADERC_LIBRARY "${shaderc_library}" PARENT_SCOPE)
  set(SHADERC_LIBRARIES "${shaderc_library}" PARENT_SCOPE)
endfunction()

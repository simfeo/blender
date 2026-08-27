# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Platform setup for Android (arm64), consuming the prebuilt dependency
# libraries from lib/android_arm64, as every other platform does.
#
# WIP: establishes LIBDIR and the per-package <Pkg>_ROOT hints, and disables
# desktop-only features. Full configure is being brought up incrementally.

if(NOT DEFINED BUILD_BASE)
  # All Android build artifacts live under this sibling dir (see build_apk.sh).
  set(BUILD_BASE "${CMAKE_SOURCE_DIR}/../blender_build_android")
endif()
if(NOT DEFINED LIBDIR)
  set(LIBDIR "${CMAKE_SOURCE_DIR}/lib/android_arm64")
endif()
if(NOT EXISTS "${LIBDIR}")
  message(FATAL_ERROR "Android LIBDIR not found: ${LIBDIR}\n"
    "Fetch the precompiled libraries: git submodule update --init lib/android_arm64")
endif()
message(STATUS "Android LIBDIR = ${LIBDIR}")

# Cross compiling leaves the test setup without an interpreter it can run: the
# Blender built here is an aarch64 binary. The root CMakeLists declares this as
# an empty cache entry before this file is included, so test it for a value
# rather than for being defined.
if(NOT TEST_PYTHON_EXE)
  find_program(_android_host_python NAMES python3 python NO_CMAKE_FIND_ROOT_PATH)
  if(_android_host_python)
    set(TEST_PYTHON_EXE "${_android_host_python}" CACHE PATH "" FORCE)
    message(STATUS "Android: tests will use host python ${TEST_PYTHON_EXE}")
  endif()
  unset(_android_host_python CACHE)
endif()

# Feature set: -DBLENDER_ANDROID_CONFIG=lite|full (or env), default full. Must be
# set early (CROSSCOMPILE_TOOLDIR and find_package guards depend on it). Matches
# the host codegen-tools build's feature flags exactly.
if(NOT DEFINED BLENDER_ANDROID_CONFIG)
  if(DEFINED ENV{BLENDER_ANDROID_CONFIG})
    set(BLENDER_ANDROID_CONFIG $ENV{BLENDER_ANDROID_CONFIG})
  else()
    set(BLENDER_ANDROID_CONFIG full)
  endif()
endif()
message(STATUS "Android config: ${BLENDER_ANDROID_CONFIG}")
include(${CMAKE_SOURCE_DIR}/build_files/android/android_features_${BLENDER_ANDROID_CONFIG}.cmake)

# pthread/rt are folded into bionic libc, so no such libraries exist, yet several
# dependencies still emit -lpthread/-lrt. Empty archives satisfy the linker.
# Generated into the build tree so this does not depend on the library prefix.
set(_stub_dir ${CMAKE_BINARY_DIR}/stublibs)
if(NOT EXISTS ${_stub_dir}/libpthread.a)
  file(MAKE_DIRECTORY ${_stub_dir})
  foreach(_stub pthread rt)
    execute_process(
      COMMAND ${CMAKE_AR} qc ${_stub_dir}/lib${_stub}.a
      WORKING_DIRECTORY ${_stub_dir}
      RESULT_VARIABLE _stub_result
    )
    if(NOT _stub_result EQUAL 0)
      message(FATAL_ERROR "Could not create stub lib${_stub}.a with ${CMAKE_AR}")
    endif()
  endforeach()
  unset(_stub_result)
endif()
foreach(_lf CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS)
  string(APPEND ${_lf} " -L${_stub_dir}")
endforeach()

# NDK libc++ lacks std::atomic_ref (C++20); force-include a polyfill.
string(APPEND CMAKE_CXX_FLAGS
  " -include ${CMAKE_SOURCE_DIR}/build_files/android/compat/atomic_ref_compat.hpp")

# Blender needs newer Vulkan headers than the NDK ships. NDK 28 is at header
# version 275, which predates VK_KHR_dynamic_rendering_local_read that GHOST
# uses unconditionally. Every other platform gets these from lib/<platform>/vulkan;
# the Android prefix has no such package yet, so fall back to a local one.
if(EXISTS ${LIBDIR}/vulkan/include)
  set(VULKAN_INCLUDE_DIR ${LIBDIR}/vulkan/include)
elseif(EXISTS ${BUILD_BASE}/lib/vulkan_headers/include)
  set(VULKAN_INCLUDE_DIR ${BUILD_BASE}/lib/vulkan_headers/include)
else()
  message(FATAL_ERROR
    "No Vulkan headers new enough for Blender.\n"
    "The NDK sysroot ships header version 275, which lacks "
    "VK_KHR_dynamic_rendering_local_read.\n"
    "Provide a Vulkan-Headers checkout as <LIBDIR>/vulkan/include.")
endif()
include_directories(BEFORE SYSTEM ${VULKAN_INCLUDE_DIR})
set(VULKAN_INCLUDE_DIRS ${VULKAN_INCLUDE_DIR})
find_library(VULKAN_LIBRARY vulkan REQUIRED)
set(VULKAN_LIBRARIES ${VULKAN_LIBRARY})
set(VULKAN_FOUND ON)

# Find harvested libs by rooting into their prefixes, never host paths.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
file(GLOB _android_dep_prefixes "${LIBDIR}/*")
list(APPEND CMAKE_FIND_ROOT_PATH ${_android_dep_prefixes})

# Per-package root hints for find_package().
set(ZLIB_ROOT ${LIBDIR}/zlib)
set(ZSTD_ROOT ${LIBDIR}/zstd)
set(Imath_ROOT ${LIBDIR}/imath)
set(fmt_ROOT ${LIBDIR}/fmt)
set(TBB_ROOT ${LIBDIR}/tbb)
set(OpenEXR_ROOT ${LIBDIR}/openexr)
set(PNG_ROOT ${LIBDIR}/png)
set(JPEG_ROOT ${LIBDIR}/jpeg)
set(TIFF_ROOT ${LIBDIR}/tiff)
set(WebP_ROOT ${LIBDIR}/webp)
set(Freetype_ROOT ${LIBDIR}/freetype)
set(HARFBUZZ_ROOT ${LIBDIR}/harfbuzz)
set(OpenImageIO_ROOT ${LIBDIR}/openimageio)
set(OpenColorIO_ROOT ${LIBDIR}/opencolorio)
set(OpenSubdiv_ROOT ${LIBDIR}/opensubdiv)
set(OpenVDB_ROOT ${LIBDIR}/openvdb)
set(Alembic_ROOT ${LIBDIR}/alembic)
set(MaterialX_ROOT ${LIBDIR}/materialx)
set(Embree_ROOT ${LIBDIR}/embree)
set(pxr_ROOT ${LIBDIR}/usd)
set(PYTHON_ROOT ${LIBDIR}/python)
set(LLVM_ROOT_DIR ${LIBDIR}/llvm)
set(CLANG_ROOT_DIR ${LIBDIR}/llvm)
set(SQLite3_ROOT ${LIBDIR}/sqlite)
set(Potrace_ROOT ${LIBDIR}/potrace)
set(FFMPEG_ROOT ${LIBDIR}/ffmpeg)
set(Robinmap_ROOT ${LIBDIR}/robinmap)
set(PUGIXML_ROOT ${LIBDIR}/pugixml)
set(EXPAT_ROOT ${LIBDIR}/expat)
set(yaml-cpp_ROOT ${LIBDIR}/yamlcpp)
set(Blosc_ROOT ${LIBDIR}/blosc)
set(OpenJPEG_ROOT ${LIBDIR}/openjpeg)
set(pystring_ROOT ${LIBDIR}/pystring)
set(Eigen3_ROOT ${LIBDIR}/eigen)
set(absl_ROOT ${LIBDIR}/abseil)
set(Fribidi_ROOT ${LIBDIR}/fribidi)
# Upstream ships the header at the package root; the old harvest put it under
# include/. Accept either.
if(EXISTS ${LIBDIR}/sse2neon/include/sse2neon.h)
  set(SSE2NEON_INCLUDE_DIR ${LIBDIR}/sse2neon/include)
else()
  set(SSE2NEON_INCLUDE_DIR ${LIBDIR}/sse2neon)
endif()
set(LibFFI_ROOT ${LIBDIR}/libffi)
set(OpenSSL_ROOT ${LIBDIR}/openssl)

# Vulkan surface from the NDK sysroot.
set(WITH_VULKAN_BACKEND ON)
set(WITH_GHOST_ANDROID ON)
# Global so creator.cc (GHOST_android_launch) and the GHOST backend agree.
add_definitions(-DWITH_GHOST_ANDROID)
# volk must load the Android surface entrypoint (vkCreateAndroidSurfaceKHR).
add_definitions(-DVK_USE_PLATFORM_ANDROID_KHR)

# -----------------------------------------------------------------------------
# Cross-compiled build tools (makesdna, makesrna, datatoc, msgfmt, shader_tool).
# These generate source at build time and must run on the host, so they are
# built natively (macOS arm64 == Android arm64 data model, so DNA/RNA offsets
# match) and imported here. Build them first:
#   cmake -S . -B ../build_host_tools -G Ninja -DWITH_CYCLES=OFF
#   ninja -C ../build_host_tools makesdna makesrna datatoc msgfmt shader_tool
set(WITH_CROSSCOMPILED_TOOLS ON)
if(NOT DEFINED CROSSCOMPILE_TOOLDIR)
  # Host tools are config-specific (generated code matches the feature set).
  set(CROSSCOMPILE_TOOLDIR "${BUILD_BASE}/build_host_tools_${BLENDER_ANDROID_CONFIG}/bin")
endif()
foreach(_tool makesdna makesrna datatoc msgfmt shader_tool)
  if(NOT EXISTS "${CROSSCOMPILE_TOOLDIR}/${_tool}")
    message(FATAL_ERROR "Host tool missing: ${CROSSCOMPILE_TOOLDIR}/${_tool}\n"
      "Build host tools first (see platform_android.cmake header).")
  endif()
  add_executable(${_tool} IMPORTED GLOBAL)
  set_property(TARGET ${_tool} PROPERTY IMPORTED_LOCATION "${CROSSCOMPILE_TOOLDIR}/${_tool}")
endforeach()

# Desktop-only or not-yet-ported features: keep off for Android.
set(WITH_GHOST_X11 OFF)
set(WITH_GHOST_WAYLAND OFF)
set(WITH_GHOST_SDL OFF)
set(WITH_X11 OFF)
set(WITH_OPENGL_BACKEND OFF)
set(WITH_GHOST_XDND OFF)
set(WITH_AUDASPACE ON)
set(WITH_COREAUDIO OFF)

# Feature toggles that vary by config (WITH_CYCLES, WITH_USD, WITH_OPENVDB, …)
# live in build_files/android/android_features_{common,full,lite}.cmake, included
# near the top of this file. Do NOT re-set them here: a normal variable would
# shadow the cache and break the lite config. Only Android-invariant extras below.
set(WITH_LIBMV_SCHUR_SPECIALIZATIONS OFF)
set(WITH_PYTHON_INSTALL OFF)
set(WITH_PYTHON_MODULE OFF)
set(WITH_DOC_MANPAGE OFF)
# Offline GLSL-as-C++ shader validation (dev feature); libc++ name clashes
# (e.g. hypot). Runtime shaders still compile via shaderc.
set(WITH_GPU_SHADER_CPP_COMPILATION OFF)
set(WITH_CYCLES_HYDRA_RENDER_DELEGATE OFF)


# -----------------------------------------------------------------------------
# Locate the harvested dependencies (mirrors platform_unix for our subset).

macro(find_package_wrapper)
  find_package(${ARGV})
endmacro()

find_package_wrapper(JPEG REQUIRED)
find_package_wrapper(PNG REQUIRED)
find_package_wrapper(ZLIB REQUIRED)
find_package_wrapper(Zstd REQUIRED)
find_package_wrapper(fmt REQUIRED)
find_package(Eigen3 REQUIRED)
find_package_wrapper(Freetype REQUIRED)
find_package_wrapper(Brotli REQUIRED)
find_package_wrapper(Harfbuzz)
find_package_wrapper(Fribidi)

if(WITH_PYTHON)
  set(PYTHON_VERSION 3.13)
  set(PYTHON_INCLUDE_DIR ${LIBDIR}/python/include/python3.13)
  set(PYTHON_INCLUDE_CONFIG_DIR ${LIBDIR}/python/include/python3.13)
  set(PYTHON_LIBRARY ${LIBDIR}/python/lib/libpython3.13.so)
  set(PYTHON_LIBPATH ${LIBDIR}/python/lib)
  find_package(PythonLibsUnix REQUIRED)
  # Build-time scripts (discover_nodes.py, etc.) must run on the HOST, so
  # PYTHON_EXECUTABLE points to a host interpreter, not the arm64 target one.
  find_program(HOST_PYTHON_EXECUTABLE NAMES python3.13 python3 python
    NO_CMAKE_FIND_ROOT_PATH)
  if(NOT HOST_PYTHON_EXECUTABLE)
    message(FATAL_ERROR "No host python3 found for build-time scripts")
  endif()
  set(PYTHON_EXECUTABLE "${HOST_PYTHON_EXECUTABLE}" CACHE FILEPATH "" FORCE)

  # numpy (cross-compiled into the target site-packages).
  set(_np ${LIBDIR}/python/lib/python3.13/site-packages/numpy/_core/include)
  if(EXISTS ${_np}/numpy/ndarrayobject.h)
    set(WITH_PYTHON_NUMPY ON)
    set(PYTHON_NUMPY_INCLUDE_DIRS ${_np})
    set(PYTHON_NUMPY_PATH ${LIBDIR}/python/lib/python3.13/site-packages)
  endif()
endif()

find_package_wrapper(OpenEXR REQUIRED)
find_package_wrapper(OpenJPEG)
find_package_wrapper(WebP)
find_package_wrapper(PugiXML)
find_package_wrapper(TBB)
# Blender uses tbbmalloc's scalable_allocation_mode, so link it too.
set(TBB_LIBRARIES TBB::tbb TBB::tbbmalloc)
find_package_wrapper(OpenImageIO REQUIRED)
# OIIO built without tools; stub the tool target (not executed in this config).
if(NOT TARGET OpenImageIO::oiiotool)
  add_executable(OpenImageIO::oiiotool IMPORTED)
  set_target_properties(OpenImageIO::oiiotool PROPERTIES
    IMPORTED_LOCATION "${LIBDIR}/openimageio/bin/oiiotool")
endif()
find_package_wrapper(OpenColorIO 2.0.0 REQUIRED)
test_neon_support()  # sets SUPPORTS_NEON_BUILD, so Cycles uses sse2neon not -msse
find_package_wrapper(sse2neon REQUIRED)
if(WITH_OPENVDB)
  find_package_wrapper(OpenVDB)
  find_package_wrapper(NanoVDB)
endif()
if(WITH_ALEMBIC)
  find_package_wrapper(Alembic)
endif()
if(WITH_USD)
  find_package_wrapper(USD)
endif()
if(WITH_MATERIALX)
  find_package_wrapper(MaterialX)
endif()
find_package_wrapper(OpenSubdiv)
find_package_wrapper(Potrace)
set(meshoptimizer_ROOT ${LIBDIR}/meshoptimizer)
find_package_wrapper(meshoptimizer)

if(WITH_VULKAN_BACKEND)
  set(SHADERC_ROOT_DIR ${LIBDIR}/shaderc)
  find_package_wrapper(ShaderC REQUIRED)
endif()

if(WITH_CYCLES_EMBREE)
  find_package(Embree 4.0.0 REQUIRED)
endif()

if(WITH_LLVM)
  find_package_wrapper(LLVM)
endif()

if(WITH_CODEC_FFMPEG)
  find_package(FFmpeg)
endif()

if(WITH_RUBBERBAND)
  set(RUBBERBAND_ROOT_DIR ${LIBDIR}/rubberband)
  find_package(Rubberband REQUIRED)
  # The prebuilt rubberband uses the FFTW backend, which ships as its own
  # package rather than being linked in.
  if(EXISTS ${LIBDIR}/fftw3/lib/libfftw3.a)
    list(APPEND RUBBERBAND_LIBRARIES
      ${LIBDIR}/fftw3/lib/libfftw3.a
      ${LIBDIR}/fftw3/lib/libfftw3_threads.a
    )
  endif()
endif()

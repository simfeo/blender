# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# FULL Android config — everything, for flagship devices (modern Qualcomm etc.).
# Cycles path tracer, video (ffmpeg), USD/OpenVDB/Alembic/MaterialX, LLVM
# (OSL to be enabled once its host-clang bitcode step is wired up).

include(${CMAKE_CURRENT_LIST_DIR}/android_features_common.cmake)

set(WITH_CYCLES ON CACHE BOOL "" FORCE)
set(WITH_CYCLES_EMBREE ON CACHE BOOL "" FORCE)
set(WITH_CODEC_FFMPEG ON CACHE BOOL "" FORCE)
set(WITH_USD ON CACHE BOOL "" FORCE)
set(WITH_MATERIALX ON CACHE BOOL "" FORCE)
set(WITH_OPENVDB ON CACHE BOOL "" FORCE)
set(WITH_ALEMBIC ON CACHE BOOL "" FORCE)
set(WITH_LLVM ON CACHE BOOL "" FORCE)
set(WITH_RUBBERBAND ON CACHE BOOL "" FORCE)

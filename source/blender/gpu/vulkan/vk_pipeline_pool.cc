/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <sstream>

#include "BKE_appdir.hh"
#include "BKE_blender_version.h"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"
#include "BLI_time.hh"

#include "CLG_log.h"

#include "GPU_capabilities.hh"

#ifdef __ANDROID__
#  include <cstdlib>
#  include <sys/system_properties.h>
#endif
#include "vk_backend.hh"
#include "vk_graphics_pipeline.hh"
#include "vk_pipeline_pool.hh"
#include "vk_to_string.hh"

namespace blender {

#ifdef WITH_BUILDINFO
extern "C" char build_hash[];
#endif
static CLG_LogRef LOG = {"gpu.vulkan"};

namespace gpu {

void VKPipelinePool::init()
{
  VKDevice &device = VKBackend::get().device;
  VkPipelineCacheCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  device.functions.vkCreatePipelineCache(
      device.vk_handle(), &create_info, nullptr, &vk_pipeline_cache_static_);
  debug::object_label(vk_pipeline_cache_static_, "VkPipelineCache.Static");
  device.functions.vkCreatePipelineCache(
      device.vk_handle(), &create_info, nullptr, &vk_pipeline_cache_non_static_);
  debug::object_label(vk_pipeline_cache_non_static_, "VkPipelineCache.Dynamic");
}

/* -------------------------------------------------------------------- */
/** \name Compute pipelines
 * \{ */

/**
 * `void main() {}` with a 1x1x1 local size, as SPIR-V.
 *
 * Generated offline so no shader compiler is needed on this path:
 *   glslangValidator -V noop.comp -o noop.spv
 * A compute shader is free to leave every binding in the pipeline layout unused, so this module
 * can be paired with any layout.
 */
static const uint32_t VK_NOOP_COMPUTE_SPIRV[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000000a, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00040047, 0x00000009, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017,
    0x00000007, 0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x00000008, 0x00000001, 0x0006002c,
    0x00000007, 0x00000009, 0x00000008, 0x00000008, 0x00000008, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038,
};

/**
 * Build a pipeline that runs the do-nothing shader above, using the layout the real shader would
 * have had.
 *
 * `vkCreateComputePipelines` can refuse a shader that every desktop driver accepts and that
 * `spirv-val` considers valid — Qualcomm's Adreno driver answers VK_ERROR_UNKNOWN for several of
 * EEVEE's and the subdivision evaluator's compute shaders, and its own log says only "Shader
 * compilation failed". Returning a null handle from here is worse than it sounds: callers hold a
 * shader they believe is ready, `vkCmdBindPipeline` rejects VK_NULL_HANDLE as invalid usage, and
 * dropping the dispatch instead leaves the render graph without the resource transitions the
 * following nodes wait for. Either way the frame never finishes, and on Android a frame that never
 * finishes is an ANR that kills the process.
 *
 * A valid pipeline that writes nothing keeps all of that intact. The pass produces no output — the
 * viewport loses that effect — but the frame completes and the application stays alive.
 */
/**
 * Is the substitute pipeline wanted?
 *
 * On by default so an untested driver heals itself. Turning it off makes a refused pipeline
 * fail where it happens, which is what telling a driver bug from a Blender bug needs:
 *   adb shell setprop debug.blender.compute_fallback 0
 */
static bool vk_compute_pipeline_fallback_enabled()
{
  static const bool enabled = []() {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.blender.compute_fallback", value) > 0 && value[0] != 0) {
      return atoi(value) != 0;
    }
#endif
    return true;
  }();
  return enabled;
}

static VkPipeline vk_compute_pipeline_noop(VKDevice &device,
                                           VkPipelineLayout vk_pipeline_layout,
                                           StringRefNull name)
{
  VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          nullptr,
                                          0,
                                          sizeof(VK_NOOP_COMPUTE_SPIRV),
                                          VK_NOOP_COMPUTE_SPIRV};
  VkShaderModule vk_module = VK_NULL_HANDLE;
  if (device.functions.vkCreateShaderModule(
          device.vk_handle(), &module_info, nullptr, &vk_module) != VK_SUCCESS)
  {
    return VK_NULL_HANDLE;
  }

  VkComputePipelineCreateInfo pipeline_info = {
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      nullptr,
      0,
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       nullptr,
       0,
       VK_SHADER_STAGE_COMPUTE_BIT,
       vk_module,
       "main",
       nullptr},
      vk_pipeline_layout,
      VK_NULL_HANDLE,
      0};
  VkPipeline vk_pipeline = VK_NULL_HANDLE;
  const VkResult result = device.functions.vkCreateComputePipelines(
      device.vk_handle(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &vk_pipeline);

  /* The module is only needed while the pipeline is built. */
  device.functions.vkDestroyShaderModule(device.vk_handle(), vk_module, nullptr);

  if (result != VK_SUCCESS) {
    CLOG_ERROR(&LOG,
               "Could not even build a do-nothing pipeline for `%s`: %s",
               name.c_str(),
               to_string(result));
    return VK_NULL_HANDLE;
  }
  CLOG_WARN(&LOG,
            "Substituted a do-nothing pipeline for `%s`. This pass will produce no output.",
            name.c_str());

  /* Subdivision is the one consumer that has a working alternative: the surface can be
   * evaluated on the CPU. A do-nothing pipeline would leave its buffers empty and the mesh
   * would be drawn as collapsed geometry, so tell the subdivision code to stop asking the GPU. */
  if (name.startswith("subdiv_")) {
    GPU_subdivision_evaluation_set_broken();
  }
  return vk_pipeline;
}

VkPipeline VKPipelinePool::get_or_create_compute_pipeline(const VKComputeInfo &compute_info,
                                                          const bool is_static_shader,
                                                          VkPipeline vk_pipeline_base,
                                                          StringRefNull name)
{
  bool created = false;
  VkPipelineCache vk_pipeline_cache = is_static_shader ? vk_pipeline_cache_static_ :
                                                         vk_pipeline_cache_non_static_;
  return compute_.get_or_create(compute_info, vk_pipeline_cache, vk_pipeline_base, name, created);
}

template<>
VkPipeline VKPipelineMap<VKComputeInfo>::create(const VKComputeInfo &compute_info,
                                                VkPipelineCache vk_pipeline_cache,
                                                VkPipeline vk_pipeline_base,
                                                StringRefNull name)
{
  /* Building compute pipeline create info */
  const bool do_specialization_constants = !compute_info.specialization_constants.is_empty();
  VkComputePipelineCreateInfo vk_compute_pipeline_create_info = {
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      nullptr,
      0,
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       nullptr,
       0,
       VK_SHADER_STAGE_COMPUTE_BIT,
       compute_info.vk_shader_module,
       "main",
       nullptr},
      compute_info.vk_pipeline_layout,
      vk_pipeline_base,
      0};

  /* Specialization constants */
  VkSpecializationInfo vk_specialization_info;
  Array<VkSpecializationMapEntry> vk_specialization_map_entries(
      compute_info.specialization_constants.size());
  if (do_specialization_constants) {
    vk_compute_pipeline_create_info.stage.pSpecializationInfo = &vk_specialization_info;
    for (uint32_t index : IndexRange(compute_info.specialization_constants.size())) {
      vk_specialization_map_entries[index] = {
          index, uint32_t(index * sizeof(uint32_t)), sizeof(uint32_t)};
    }
    vk_specialization_info = {uint32_t(vk_specialization_map_entries.size()),
                              vk_specialization_map_entries.data(),
                              compute_info.specialization_constants.size() * sizeof(uint32_t),
                              compute_info.specialization_constants.data()};
  }

  /* Create pipeline. */
  VKBackend &backend = VKBackend::get();
  VKDevice &device = backend.device;

  double start_time = BLI_time_now_seconds();
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult result = device.functions.vkCreateComputePipelines(device.vk_handle(),
                                                                    vk_pipeline_cache,
                                                                    1,
                                                                    &vk_compute_pipeline_create_info,
                                                                    nullptr,
                                                                    &pipeline);
  if (result != VK_SUCCESS) {
    CLOG_ERROR(&LOG,
               "Failed to compile compute pipeline `%s`: %s (specialization constants: %d, "
               "base pipeline: %s, cache: %s)",
               name.c_str(),
               to_string(result),
               int(compute_info.specialization_constants.size()),
               vk_pipeline_base == VK_NULL_HANDLE ? "no" : "yes",
               vk_pipeline_cache == VK_NULL_HANDLE ? "no" : "yes");

    /* Qualcomm's Adreno driver answers VK_ERROR_UNKNOWN for compute pipelines that every desktop
     * driver accepts, and the message carries no reason. Narrow it down by retrying with one
     * input removed at a time: the shared pipeline cache first (the shader compile workers use it
     * concurrently), then the base pipeline handle. Whichever retry succeeds is both the diagnosis
     * and a usable pipeline, so keep it instead of returning a null handle that would leave the
     * caller to fail again further away from the cause. */
    if (vk_pipeline_cache != VK_NULL_HANDLE) {
      const VkResult retry = device.functions.vkCreateComputePipelines(
          device.vk_handle(), VK_NULL_HANDLE, 1, &vk_compute_pipeline_create_info, nullptr,
          &pipeline);
      CLOG_ERROR(&LOG, "  retry `%s` without pipeline cache: %s", name.c_str(), to_string(retry));
      if (retry == VK_SUCCESS) {
        return pipeline;
      }
    }
    if (vk_pipeline_base != VK_NULL_HANDLE) {
      vk_compute_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
      const VkResult retry = device.functions.vkCreateComputePipelines(
          device.vk_handle(), VK_NULL_HANDLE, 1, &vk_compute_pipeline_create_info, nullptr,
          &pipeline);
      CLOG_ERROR(&LOG, "  retry `%s` without base pipeline: %s", name.c_str(), to_string(retry));
      if (retry == VK_SUCCESS) {
        return pipeline;
      }
    }
    if (!vk_compute_pipeline_fallback_enabled()) {
      return VK_NULL_HANDLE;
    }
    device.workarounds_get_for_write().compute_pipeline_fallback = true;
    return vk_compute_pipeline_noop(device, compute_info.vk_pipeline_layout, name);
  }
  double end_time = BLI_time_now_seconds();
  debug::object_label(pipeline, name);
  CLOG_DEBUG(&LOG,
             "Compiled compute pipeline %s in %fms ",
             name.c_str(),
             (end_time - start_time) * 1000.0);

  return pipeline;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Graphics pipelines
 * \{ */

VkPipeline VKPipelinePool::get_or_create_graphics_pipeline(const VKGraphicsInfo &graphics_info,
                                                           const bool is_static_shader,
                                                           VkPipeline vk_pipeline_base,
                                                           StringRefNull name,
                                                           bool &r_created)
{
  BLI_assert_msg(
      graphics_info.shaders.state == graphics_info.fragment_out.state,
      "VKGraphicsInfo.shader.state and VKGraphicsInfo.fragment_out.state should be identical, "
      "otherwise an incorrect fragment output library will be linked.");
  VkPipelineCache vk_pipeline_cache = is_static_shader ? vk_pipeline_cache_static_ :
                                                         vk_pipeline_cache_non_static_;
  return graphics_.get_or_create(
      graphics_info, vk_pipeline_cache, vk_pipeline_base, name, r_created);
}

static VkPipeline create_graphics_pipeline_no_libs(const VKGraphicsInfo &graphics_info,
                                                   VkPipelineCache vk_pipeline_cache,
                                                   VkPipeline vk_pipeline_base,
                                                   StringRefNull name)
{
  VKDevice &device = VKBackend::get().device;
  VKGraphicsPipelineCreateInfoBuilder builder;
  builder.build_full(device, graphics_info, vk_pipeline_base);

  /* Build pipeline. */
  VkPipeline pipeline = VK_NULL_HANDLE;
  double start_time = BLI_time_now_seconds();
  const VkResult result = device.functions.vkCreateGraphicsPipelines(
      device.vk_handle(),
      vk_pipeline_cache,
      1,
      &builder.vk_graphics_pipeline_create_info,
      nullptr,
      &pipeline);
  if (result != VK_SUCCESS) {
    CLOG_ERROR(&LOG,
               "Failed to compile graphics pipeline `%s`: %s",
               name.c_str(),
               to_string(result));
  }
  double end_time = BLI_time_now_seconds();
  debug::object_label(pipeline, name);
  CLOG_DEBUG(&LOG,
             "Compiled graphics pipeline %s in %fms ",
             name.c_str(),
             (end_time - start_time) * 1000.0);
  return pipeline;
}

static VkPipeline create_graphics_pipeline_libs(const VKGraphicsInfo &graphics_info,
                                                VkPipelineCache vk_pipeline_cache,
                                                VkPipeline vk_pipeline_base,
                                                StringRefNull name)
{
  double start_time = BLI_time_now_seconds();
  VKDevice &device = VKBackend::get().device;

  VkPipeline vertex_input_lib = device.pipelines.get_or_create_vertex_input_lib(
      graphics_info.vertex_in);
  VkPipeline shaders_lib = device.pipelines.get_or_create_shaders_lib(graphics_info.shaders);
  VkPipeline fragment_output_lib = device.pipelines.get_or_create_fragment_output_lib(
      graphics_info.fragment_out);

  std::array<VkPipeline, 3> pipeline_libraries = {
      vertex_input_lib, shaders_lib, fragment_output_lib};

  /* Linking */
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLibraryCreateInfoKHR vk_pipeline_library_create_info = {
      VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
      nullptr,
      uint32_t(pipeline_libraries.size()),
      pipeline_libraries.data()};
  VkGraphicsPipelineCreateInfo linking_pipeline_create_info = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      &vk_pipeline_library_create_info,
      VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT,
      0,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      graphics_info.shaders.vk_pipeline_layout,
      VK_NULL_HANDLE,
      0,
      vk_pipeline_base,
      0};
  double start_link_time = BLI_time_now_seconds();
  device.functions.vkCreateGraphicsPipelines(
      device.vk_handle(), vk_pipeline_cache, 1, &linking_pipeline_create_info, nullptr, &pipeline);
  double end_time = BLI_time_now_seconds();
  debug::object_label(pipeline, name);
  CLOG_TRACE(&LOG,
             "Linking graphics pipeline %s in %fms ",
             name.c_str(),
             (end_time - start_link_time) * 1000.0);
  CLOG_DEBUG(&LOG,
             "Compiling graphics pipeline %s in %fms ",
             name.c_str(),
             (end_time - start_time) * 1000.0);
  return pipeline;
}

template<>
VkPipeline VKPipelineMap<VKGraphicsInfo>::create(const VKGraphicsInfo &graphics_info,
                                                 VkPipelineCache vk_pipeline_cache,
                                                 VkPipeline vk_pipeline_base,
                                                 StringRefNull name)
{
  VKDevice &device = VKBackend::get().device;
  const VKExtensions &extensions = device.extensions_get();
  if (extensions.graphics_pipeline_library) {
    return create_graphics_pipeline_libs(graphics_info, vk_pipeline_cache, vk_pipeline_base, name);
  }
  return create_graphics_pipeline_no_libs(
      graphics_info, vk_pipeline_cache, vk_pipeline_base, name);
}

std::string VKGraphicsInfo::pipeline_info_source() const
{
  std::stringstream result;
  result << "info.pipeline_state()\n";
  result << "  .primitive(";
  switch (vertex_in.vk_topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      result << "GPU_PRIM_POINTS";
      break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      result << "GPU_PRIM_LINES";
      break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      result << "GPU_PRIM_LINE_STRIP";
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      result << "GPU_PRIM_TRIS";
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      result << "GPU_PRIM_TRI_STRIP";
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      result << "GPU_PRIM_TRI_FAN";
      break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      result << "GPU_PRIM_LINES_ADJ";
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
      result << "GPU_PRIM_TRIS_ADJ";
      break;
    default:
      BLI_assert_unreachable();
  };
  result << ")\n";
  result << "  .state(";
  /* Write mask */
  Vector<std::string> write_masks;
  if (fragment_out.state.write_mask & GPU_WRITE_COLOR) {
    if ((fragment_out.state.write_mask & GPU_WRITE_COLOR) == GPU_WRITE_COLOR) {
      write_masks.append("GPU_WRITE_COLOR");
    }
    else {
      if (fragment_out.state.write_mask & GPU_WRITE_RED) {
        write_masks.append("GPU_WRITE_RED");
      }
      if (fragment_out.state.write_mask & GPU_WRITE_GREEN) {
        write_masks.append("GPU_WRITE_GREEN");
      }
      if (fragment_out.state.write_mask & GPU_WRITE_BLUE) {
        write_masks.append("GPU_WRITE_BLUE");
      }
      if (fragment_out.state.write_mask & GPU_WRITE_ALPHA) {
        write_masks.append("GPU_WRITE_ALPHA");
      }
    }
  }
  if (fragment_out.state.write_mask & GPU_WRITE_DEPTH) {
    write_masks.append("GPU_WRITE_DEPTH");
  }
  if (fragment_out.state.write_mask & GPU_WRITE_STENCIL) {
    write_masks.append("GPU_WRITE_STENCIL");
  }
  if (write_masks.is_empty()) {
    write_masks.append("GPU_WRITE_NONE");
  }
  for (const std::string &write_mask : write_masks) {
    result << write_mask;
    if (&write_mask != &write_masks.last()) {
      result << " | ";
    }
  }
  /* Blending mode */
  result << ",\n         ";
  switch (fragment_out.state.blend) {
    case GPU_BLEND_NONE:
      result << "GPU_BLEND_NONE";
      break;
    case GPU_BLEND_ALPHA:
      result << "GPU_BLEND_ALPHA";
      break;
    case GPU_BLEND_ALPHA_PREMULT:
      result << "GPU_BLEND_ALPHA_PREMULT";
      break;
    case GPU_BLEND_ADDITIVE:
      result << "GPU_BLEND_ADDITIVE";
      break;
    case GPU_BLEND_ADDITIVE_PREMULT:
      result << "GPU_BLEND_ADDITIVE_PREMULT";
      break;
    case GPU_BLEND_MULTIPLY:
      result << "GPU_BLEND_MULTIPLY";
      break;
    case GPU_BLEND_SUBTRACT:
      result << "GPU_BLEND_SUBTRACT";
      break;
    case GPU_BLEND_INVERT:
      result << "GPU_BLEND_INVERT";
      break;
    case GPU_BLEND_MIN:
      result << "GPU_BLEND_MIN";
      break;
    case GPU_BLEND_MAX:
      result << "GPU_BLEND_MAX";
      break;
    case GPU_BLEND_OIT:
      result << "GPU_BLEND_OIT";
      break;
    case GPU_BLEND_BACKGROUND:
      result << "GPU_BLEND_BACKGROUND";
      break;
    case GPU_BLEND_CUSTOM:
      result << "GPU_BLEND_CUSTOM";
      break;
    case GPU_BLEND_ALPHA_UNDER_PREMUL:
      result << "GPU_BLEND_ALPHA_UNDER_PREMUL";
      break;
    case GPU_BLEND_OVERLAY_MASK_FROM_ALPHA:
      result << "GPU_BLEND_OVERLAY_MASK_FROM_ALPHA";
      break;
    case GPU_BLEND_TRANSPARENCY:
      result << "GPU_BLEND_TRANSPARENCY";
      break;
    default:
      BLI_assert_unreachable();
  }
  /* Culling test */
  result << ",\n         ";
  switch (shaders.state.culling_test) {
    case GPU_CULL_NONE:
      result << "GPU_CULL_NONE";
      break;
    case GPU_CULL_FRONT:
      result << "GPU_CULL_FRONT";
      break;
    case GPU_CULL_BACK:
      result << "GPU_CULL_BACK";
      break;
    default:
      BLI_assert_unreachable();
  }
  /* Depth test */
  result << ",\n         ";
  switch (shaders.state.depth_test) {
    case GPU_DEPTH_NONE:
      result << "GPU_DEPTH_NONE";
      break;
    case GPU_DEPTH_LESS:
      result << "GPU_DEPTH_LESS";
      break;
    case GPU_DEPTH_LESS_EQUAL:
      result << "GPU_DEPTH_LESS_EQUAL";
      break;
    case GPU_DEPTH_EQUAL:
      result << "GPU_DEPTH_EQUAL";
      break;
    case GPU_DEPTH_GREATER:
      result << "GPU_DEPTH_GREATER";
      break;
    case GPU_DEPTH_GREATER_EQUAL:
      result << "GPU_DEPTH_GREATER_EQUAL";
      break;
    case GPU_DEPTH_ALWAYS:
      result << "GPU_DEPTH_ALWAYS";
      break;
    default:
      BLI_assert_unreachable();
  }
  /* Stencil test */
  result << ",\n         ";
  switch (shaders.state.stencil_test) {
    case GPU_STENCIL_NONE:
      result << "GPU_STENCIL_NONE";
      break;
    case GPU_STENCIL_ALWAYS:
      result << "GPU_STENCIL_ALWAYS";
      break;
    case GPU_STENCIL_EQUAL:
      result << "GPU_STENCIL_EQUAL";
      break;
    case GPU_STENCIL_NEQUAL:
      result << "GPU_STENCIL_NEQUAL";
      break;
    default:
      BLI_assert_unreachable();
  }
  /* Stencil operation */
  result << ",\n         ";
  switch (shaders.state.stencil_op) {
    case GPU_STENCIL_OP_NONE:
      result << "GPU_STENCIL_OP_NONE";
      break;
    case GPU_STENCIL_OP_REPLACE:
      result << "GPU_STENCIL_OP_REPLACE";
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_PASS:
      result << "GPU_STENCIL_OP_COUNT_DEPTH_PASS";
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_FAIL:
      result << "GPU_STENCIL_OP_COUNT_DEPTH_FAIL";
      break;
    default:
      BLI_assert_unreachable();
  }
  /* Provoking vertex */
  result << ",\n         ";
  switch (shaders.state.provoking_vert) {
    case GPU_VERTEX_FIRST:
      result << "GPU_VERTEX_FIRST";
      break;
    case GPU_VERTEX_LAST:
      result << "GPU_VERTEX_LAST";
      break;
    default:
      BLI_assert_unreachable();
  }
  result << ")\n";
  /* viewports */
  result << "  .viewports(" << shaders.viewport_count << ")\n";
  /* Depth format */
  if (fragment_out.depth_attachment_format != VK_FORMAT_UNDEFINED) {
    result << "  .depth_format(gpu::TextureTargetFormat::"
           << to_gpu_format_string(fragment_out.depth_attachment_format) << ")\n";
  }
  /* Stencil format */
  if (fragment_out.stencil_attachment_format != VK_FORMAT_UNDEFINED) {
    result << "  .stencil_format(gpu::TextureTargetFormat::"
           << to_gpu_format_string(fragment_out.stencil_attachment_format) << ")\n";
  }
  /* Color formats */
  for (const VkFormat format : fragment_out.color_attachment_formats) {
    result << "  .color_format(gpu::TextureTargetFormat::" << to_gpu_format_string(format)
           << ")\n";
  }
  result << ";";
  return result.str();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex input library
 * \{ */

VkPipeline VKPipelinePool::get_or_create_vertex_input_lib(
    const VKGraphicsInfo::VertexIn &vertex_input_info)
{
  bool created = false;
  return vertex_input_libs_.get_or_create(
      vertex_input_info, vk_pipeline_cache_static_, VK_NULL_HANDLE, "VertexInLib", created);
}

template<>
VkPipeline VKPipelineMap<VKGraphicsInfo::VertexIn>::create(
    const VKGraphicsInfo::VertexIn &vertex_input_info,
    VkPipelineCache vk_pipeline_cache,
    VkPipeline vk_pipeline_base,
    StringRefNull name)
{
  VKDevice &device = VKBackend::get().device;
  VKGraphicsPipelineCreateInfoBuilder builder;
  builder.build_vertex_input_lib(device, vertex_input_info, vk_pipeline_base);

  /* Build pipeline. */
  VkPipeline pipeline = VK_NULL_HANDLE;
  double start_time = BLI_time_now_seconds();
  device.functions.vkCreateGraphicsPipelines(device.vk_handle(),
                                             vk_pipeline_cache,
                                             1,
                                             &builder.vk_graphics_pipeline_create_info,
                                             nullptr,
                                             &pipeline);
  double end_time = BLI_time_now_seconds();
  debug::object_label(pipeline, name);
  CLOG_TRACE(&LOG, "Compiled vertex input library in %fms ", (end_time - start_time) * 1000.0);
  return pipeline;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shaders library
 * \{ */

VkPipeline VKPipelinePool::get_or_create_shaders_lib(const VKGraphicsInfo::Shaders &shaders_info)
{
  bool created = false;
  return shaders_libs_.get_or_create(
      shaders_info, vk_pipeline_cache_non_static_, VK_NULL_HANDLE, "ShadersLib", created);
}

template<>
VkPipeline VKPipelineMap<VKGraphicsInfo::Shaders>::create(
    const VKGraphicsInfo::Shaders &shaders_info,
    VkPipelineCache vk_pipeline_cache,
    VkPipeline vk_pipeline_base,
    StringRefNull name)
{
  VKDevice &device = VKBackend::get().device;
  const VKExtensions &extensions = device.extensions_get();
  VKGraphicsPipelineCreateInfoBuilder builder;
  builder.build_shaders_lib(shaders_info, extensions, vk_pipeline_base);

  /* Build pipeline. */
  VkPipeline pipeline = VK_NULL_HANDLE;
  double start_time = BLI_time_now_seconds();
  device.functions.vkCreateGraphicsPipelines(device.vk_handle(),
                                             vk_pipeline_cache,
                                             1,
                                             &builder.vk_graphics_pipeline_create_info,
                                             nullptr,
                                             &pipeline);
  double end_time = BLI_time_now_seconds();
  debug::object_label(pipeline, name);
  CLOG_TRACE(&LOG, "Compiled shaders library in %fms ", (end_time - start_time) * 1000.0);
  return pipeline;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Fragment output library
 * \{ */

VkPipeline VKPipelinePool::get_or_create_fragment_output_lib(
    const VKGraphicsInfo::FragmentOut &fragment_output_info)
{
  bool created = false;
  return fragment_output_libs_.get_or_create(
      fragment_output_info, vk_pipeline_cache_static_, VK_NULL_HANDLE, "FragmentOutLib", created);
}

template<>
VkPipeline VKPipelineMap<VKGraphicsInfo::FragmentOut>::create(
    const VKGraphicsInfo::FragmentOut &fragment_output_info,
    VkPipelineCache vk_pipeline_cache,
    VkPipeline vk_pipeline_base,
    StringRefNull name)
{
  VKDevice &device = VKBackend::get().device;
  const VKExtensions &extensions = device.extensions_get();
  VKGraphicsPipelineCreateInfoBuilder builder;
  builder.build_fragment_output_lib(fragment_output_info, extensions, vk_pipeline_base);

  /* Build pipeline. */
  VkPipeline pipeline = VK_NULL_HANDLE;
  double start_time = BLI_time_now_seconds();
  device.functions.vkCreateGraphicsPipelines(device.vk_handle(),
                                             vk_pipeline_cache,
                                             1,
                                             &builder.vk_graphics_pipeline_create_info,
                                             nullptr,
                                             &pipeline);
  double end_time = BLI_time_now_seconds();
  debug::object_label(pipeline, name);
  CLOG_TRACE(&LOG, "Compiled fragment output library in %fms ", (end_time - start_time) * 1000.0);
  return pipeline;
}

/** \} */

void VKPipelinePool::discard(VKDiscardPool &discard_pool, VkPipelineLayout vk_pipeline_layout)
{
  graphics_.discard(discard_pool, vk_pipeline_layout);
  compute_.discard(discard_pool, vk_pipeline_layout);
  shaders_libs_.discard(discard_pool, vk_pipeline_layout);
  /* vertex_input_libs_ and fragment_output_libs_ are NOT dependent on vk_pipeline_layout. */
}

void VKPipelinePool::free_data(const VKDevice &device)
{
  const VkDevice vk_device = device.vk_handle();

  graphics_.free_data(vk_device, device.functions);
  compute_.free_data(vk_device, device.functions);
  vertex_input_libs_.free_data(vk_device, device.functions);
  shaders_libs_.free_data(vk_device, device.functions);
  fragment_output_libs_.free_data(vk_device, device.functions);

  device.functions.vkDestroyPipelineCache(device.vk_handle(), vk_pipeline_cache_static_, nullptr);
  device.functions.vkDestroyPipelineCache(
      device.vk_handle(), vk_pipeline_cache_non_static_, nullptr);
}

/* -------------------------------------------------------------------- */
/** \name Persistent cache
 * \{ */

#ifdef WITH_BUILDINFO
struct VKPipelineCachePrefixHeader {
  /* `BC` stands for "Blender Cache" + 2 bytes for file versioning. */
  uint32_t magic = 0xBC00;
  uint32_t blender_version = BLENDER_VERSION;
  uint32_t blender_version_patch = BLENDER_VERSION_PATCH;
  char commit_hash[8];
  uint32_t data_size;
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t driver_version;
  uint8_t pipeline_cache_uuid[VK_UUID_SIZE];

  VKPipelineCachePrefixHeader()
  {
    const VKDevice &device = VKBackend::get().device;
    data_size = 0;
    const VkPhysicalDeviceProperties &properties = device.physical_device_properties_get();
    vendor_id = properties.vendorID;
    device_id = properties.deviceID;
    driver_version = properties.driverVersion;
    memcpy(&pipeline_cache_uuid, &properties.pipelineCacheUUID, VK_UUID_SIZE);

    memset(commit_hash, 0, sizeof(commit_hash));
    STRNCPY(commit_hash, build_hash);
  }
};

static std::string pipeline_cache_filepath_get()
{
  static char tmp_dir_buffer[1024];
  BKE_appdir_folder_caches(tmp_dir_buffer, sizeof(tmp_dir_buffer));

  std::string cache_dir = std::string(tmp_dir_buffer) + "vk-pipeline-cache" + SEP_STR;
  BLI_dir_create_recursive(cache_dir.c_str());
  std::string cache_file = cache_dir + "static.bin";
  return cache_file;
}
#endif

void VKPipelinePool::read_from_disk()
{
#ifdef WITH_BUILDINFO
  /* Don't read the shader cache when GPU debugging is enabled. When enabled we use different
   * shaders and compilation settings. Previous generated pipelines will not be used. */
  if (bool(G.debug & G_DEBUG_GPU)) {
    return;
  }

  std::string cache_file = pipeline_cache_filepath_get();
  if (!BLI_exists(cache_file.c_str())) {
    return;
  }

  /* Prevent old cache files from being deleted if they're still being used. */
  BLI_file_touch(cache_file.c_str());
  /* Read cached binary. */
  fstream file(cache_file, std::ios::binary | std::ios::in | std::ios::ate);
  std::streamsize data_size = file.tellg();
  file.seekg(0, std::ios::beg);
  Vector<char> buffer(data_size);
  file.read(buffer.data(), data_size);
  file.close();

  /* Validate the prefix header. */
  VKPipelineCachePrefixHeader prefix;
  VKPipelineCachePrefixHeader &read_prefix = *reinterpret_cast<VKPipelineCachePrefixHeader *>(
      buffer.data());
  prefix.data_size = read_prefix.data_size;
  if (memcmp(&read_prefix, &prefix, sizeof(VKPipelineCachePrefixHeader)) != 0) {
    /* Headers are different, most likely the cache will not work and potentially crash the driver.
     * [https://medium.com/@zeuxcg/creating-a-robust-pipeline-cache-with-vulkan-961d09416cda]
     */
    CLOG_INFO(&LOG,
              "Pipeline cache on disk [%s] is ignored as it was written by a different driver or "
              "Blender version. Cache will be overwritten when exiting.",
              cache_file.c_str());
    return;
  }

  CLOG_INFO(&LOG, "Initialize static pipeline cache from disk [%s].", cache_file.c_str());
  VKDevice &device = VKBackend::get().device;
  VkPipelineCacheCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  create_info.initialDataSize = read_prefix.data_size;
  create_info.pInitialData = buffer.data() + sizeof(VKPipelineCachePrefixHeader);
  VkPipelineCache vk_pipeline_cache = VK_NULL_HANDLE;
  device.functions.vkCreatePipelineCache(
      device.vk_handle(), &create_info, nullptr, &vk_pipeline_cache);

  device.functions.vkMergePipelineCaches(
      device.vk_handle(), vk_pipeline_cache_static_, 1, &vk_pipeline_cache);
  device.functions.vkDestroyPipelineCache(device.vk_handle(), vk_pipeline_cache, nullptr);
#endif
}

void VKPipelinePool::write_to_disk()
{
#ifdef WITH_BUILDINFO
  /* Don't write the pipeline cache when GPU debugging is enabled. When enabled we use different
   * shaders and compilation settings. Writing them to disk will clutter the pipeline cache. */
  if (bool(G.debug & G_DEBUG_GPU)) {
    return;
  }

  VKDevice &device = VKBackend::get().device;
  size_t data_size;
  device.functions.vkGetPipelineCacheData(
      device.vk_handle(), vk_pipeline_cache_static_, &data_size, nullptr);
  Vector<char> buffer(data_size);
  device.functions.vkGetPipelineCacheData(
      device.vk_handle(), vk_pipeline_cache_static_, &data_size, buffer.data());

  std::string cache_file = pipeline_cache_filepath_get();
  CLOG_INFO(&LOG, "Writing static pipeline cache to disk [%s].", cache_file.c_str());

  fstream file(cache_file, std::ios::binary | std::ios::out);

  VKPipelineCachePrefixHeader header;
  header.data_size = data_size;
  file.write(reinterpret_cast<char *>(&header), sizeof(VKPipelineCachePrefixHeader));
  file.write(buffer.data(), data_size);
#endif
}

/** \} */

}  // namespace gpu
}  // namespace blender

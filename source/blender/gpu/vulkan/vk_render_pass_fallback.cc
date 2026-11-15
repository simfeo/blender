/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <algorithm>

#include "vk_render_pass_fallback.hh"

#include "render_graph/nodes/vk_begin_rendering_node.hh"
#include "render_graph/vk_command_buffer_wrapper.hh"
#include "vk_backend.hh"
#include "vk_device.hh"

#ifdef __ANDROID__
#  include <android/log.h>
#  include <cstdlib>
#  include <sys/system_properties.h>
#endif

namespace blender::gpu {

VkRenderPass VKRenderPassFallback::render_pass_from_key(const RenderPassKey &key)
{
  std::scoped_lock lock(mutex_);
  return render_passes_.lookup_or_add_cb(key, [&]() {
    VkAttachmentDescription attachments[9] = {};
    uint32_t attachment_count = 0;
    VkAttachmentReference color_refs[8];

    for (uint32_t i = 0; i < key.color_count; i++) {
      if (key.color_formats[i] == VK_FORMAT_UNDEFINED) {
        color_refs[i] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
        continue;
      }
      VkAttachmentDescription &desc = attachments[attachment_count];
      desc.format = key.color_formats[i];
      desc.samples = VK_SAMPLE_COUNT_1_BIT;
      desc.loadOp = VkAttachmentLoadOp(key.color_load[i]);
      desc.storeOp = VkAttachmentStoreOp(key.color_store[i]);
      desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      desc.initialLayout = key.color_layouts[i];
      desc.finalLayout = key.color_layouts[i];
      color_refs[i] = {attachment_count, key.color_layouts[i]};
      attachment_count++;
    }

    VkAttachmentReference depth_ref = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    const bool has_depth = key.depth_format != VK_FORMAT_UNDEFINED ||
                           key.stencil_format != VK_FORMAT_UNDEFINED;
    if (has_depth) {
      const bool has_d = key.depth_format != VK_FORMAT_UNDEFINED;
      const bool has_s = key.stencil_format != VK_FORMAT_UNDEFINED;
      VkAttachmentDescription &desc = attachments[attachment_count];
      desc.format = has_d ? key.depth_format : key.stencil_format;
      desc.samples = VK_SAMPLE_COUNT_1_BIT;
      desc.loadOp = has_d ? VkAttachmentLoadOp(key.depth_load) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      desc.storeOp = has_d ? VkAttachmentStoreOp(key.depth_store) : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      desc.stencilLoadOp = has_s ? VkAttachmentLoadOp(key.stencil_load) :
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      desc.stencilStoreOp = has_s ? VkAttachmentStoreOp(key.stencil_store) :
                                    VK_ATTACHMENT_STORE_OP_DONT_CARE;
      desc.initialLayout = key.depth_layout;
      desc.finalLayout = key.depth_layout;
      depth_ref = {attachment_count, key.depth_layout};
      attachment_count++;
    }

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = key.color_count;
    subpass.pColorAttachments = key.color_count ? color_refs : nullptr;
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

    /* With initialLayout == finalLayout Vulkan inserts no implicit external
     * dependency, so unlike dynamic rendering the render pass isn't synchronized
     * against surrounding commands. Add explicit external dependencies covering
     * the attachment read/write stages so prior writes are visible and results
     * are flushed (missing sync manifests as a GPU hang on Adreno).
     * These must not be by-region: the consumers include compute (HiZ downsampling
     * reads neighbouring texels), which is not a framebuffer-space stage. */
    const VkPipelineStageFlags gfx_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags gfx_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency deps[2] = {};
    deps[0] = {VK_SUBPASS_EXTERNAL,
               0,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               gfx_stages,
               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
               gfx_access,
               0};
    deps[1] = {0,
               VK_SUBPASS_EXTERNAL,
               gfx_stages,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               gfx_access,
               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
               0};

    VkRenderPassCreateInfo create_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    create_info.attachmentCount = attachment_count;
    create_info.pAttachments = attachments;
    create_info.subpassCount = 1;
    create_info.pSubpasses = &subpass;
    create_info.dependencyCount = 2;
    create_info.pDependencies = deps;

    VkRenderPass vk_render_pass = VK_NULL_HANDLE;
    functions_->vkCreateRenderPass(vk_device_, &create_info, nullptr, &vk_render_pass);
    return vk_render_pass;
  });
}

VkRenderPass VKRenderPassFallback::render_pass_get(const render_graph::VKBeginRenderingData &data)
{
  RenderPassKey key = {};
  key.color_count = data.vk_rendering_info.colorAttachmentCount;
  for (uint32_t i = 0; i < key.color_count; i++) {
    key.color_formats[i] = data.color_formats[i];
    key.color_load[i] = uint8_t(data.color_attachments[i].loadOp);
    key.color_store[i] = uint8_t(data.color_attachments[i].storeOp);
    key.color_layouts[i] = data.color_attachments[i].imageView != VK_NULL_HANDLE ?
                               data.color_attachments[i].imageLayout :
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  key.depth_format = data.depth_format;
  key.stencil_format = data.stencil_format;
  key.depth_load = uint8_t(data.depth_attachment.loadOp);
  key.depth_store = uint8_t(data.depth_attachment.storeOp);
  key.stencil_load = uint8_t(data.stencil_attachment.loadOp);
  key.stencil_store = uint8_t(data.stencil_attachment.storeOp);
  key.depth_layout = (data.depth_format != VK_FORMAT_UNDEFINED) ?
                         data.depth_attachment.imageLayout :
                         data.stencil_attachment.imageLayout;
  if (key.depth_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
    key.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }
  return render_pass_from_key(key);
}

VkRenderPass VKRenderPassFallback::compat_render_pass_get(Span<VkFormat> color_formats,
                                                          VkFormat depth_format,
                                                          VkFormat stencil_format)
{
  RenderPassKey key = {};
  key.color_count = uint32_t(color_formats.size());
  for (uint32_t i = 0; i < key.color_count && i < 8; i++) {
    key.color_formats[i] = color_formats[i];
    key.color_layouts[i] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    /* Compatibility ignores load/store, use generic values so pipelines share
     * a render pass regardless of clear/load behavior. */
    key.color_load[i] = uint8_t(VK_ATTACHMENT_LOAD_OP_LOAD);
    key.color_store[i] = uint8_t(VK_ATTACHMENT_STORE_OP_STORE);
  }
  key.depth_format = depth_format;
  key.stencil_format = stencil_format;
  key.depth_load = uint8_t(VK_ATTACHMENT_LOAD_OP_LOAD);
  key.depth_store = uint8_t(VK_ATTACHMENT_STORE_OP_STORE);
  key.stencil_load = uint8_t(VK_ATTACHMENT_LOAD_OP_LOAD);
  key.stencil_store = uint8_t(VK_ATTACHMENT_STORE_OP_STORE);
  key.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  return render_pass_from_key(key);
}

VkFramebuffer VKRenderPassFallback::framebuffer_get(
    VkRenderPass vk_render_pass, const render_graph::VKBeginRenderingData &data)
{
  FramebufferKey key = {};
  key.render_pass = vk_render_pass;
  const VkRect2D &area = data.vk_rendering_info.renderArea;
  key.width = area.offset.x + area.extent.width;
  key.height = area.offset.y + area.extent.height;
  key.layers = std::max(1u, data.vk_rendering_info.layerCount);

  /* Same attachment order as the render pass: real color views, then depth. */
  for (uint32_t i = 0; i < data.vk_rendering_info.colorAttachmentCount; i++) {
    if (data.color_formats[i] == VK_FORMAT_UNDEFINED) {
      continue;
    }
    key.views[key.view_count++] = data.color_attachments[i].imageView;
  }
  if (data.depth_format != VK_FORMAT_UNDEFINED) {
    key.views[key.view_count++] = data.depth_attachment.imageView;
  }
  else if (data.stencil_format != VK_FORMAT_UNDEFINED) {
    key.views[key.view_count++] = data.stencil_attachment.imageView;
  }

  std::scoped_lock lock(mutex_);
  return framebuffers_.lookup_or_add_cb(key, [&]() {
    VkFramebufferCreateInfo create_info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    create_info.renderPass = vk_render_pass;
    create_info.attachmentCount = key.view_count;
    create_info.pAttachments = key.views;
    create_info.width = std::max(1u, key.width);
    create_info.height = std::max(1u, key.height);
    create_info.layers = key.layers;

    VkFramebuffer vk_framebuffer = VK_NULL_HANDLE;
    functions_->vkCreateFramebuffer(vk_device_, &create_info, nullptr, &vk_framebuffer);
    return vk_framebuffer;
  });
}

Vector<VkFramebuffer> VKRenderPassFallback::take_framebuffers_for_view(VkImageView vk_image_view)
{
  Vector<VkFramebuffer> taken;
  if (vk_device_ == VK_NULL_HANDLE || vk_image_view == VK_NULL_HANDLE) {
    return taken;
  }
  Vector<FramebufferKey> to_remove;
  std::scoped_lock lock(mutex_);
  for (auto item : framebuffers_.items()) {
    for (uint32_t i = 0; i < item.key.view_count; i++) {
      if (item.key.views[i] == vk_image_view) {
        taken.append(item.value);
        to_remove.append(item.key);
        break;
      }
    }
  }
  for (const FramebufferKey &key : to_remove) {
    framebuffers_.remove_contained(key);
  }
  return taken;
}

void VKRenderPassFallback::deinit()
{
  if (vk_device_ == VK_NULL_HANDLE) {
    return;
  }
  std::scoped_lock lock(mutex_);
  for (VkFramebuffer vk_framebuffer : framebuffers_.values()) {
    functions_->vkDestroyFramebuffer(vk_device_, vk_framebuffer, nullptr);
  }
  framebuffers_.clear();
  for (VkRenderPass vk_render_pass : render_passes_.values()) {
    functions_->vkDestroyRenderPass(vk_device_, vk_render_pass, nullptr);
  }
  render_passes_.clear();
}

namespace render_graph {

void begin_rendering_fallback(VKCommandBufferInterface &command_buffer,
                              const VKBeginRenderingData &data)
{
  VKRenderPassFallback &fallback = VKBackend::get().device.render_pass_fallback;
  VkRenderPass vk_render_pass = fallback.render_pass_get(data);
  VkFramebuffer vk_framebuffer = fallback.framebuffer_get(vk_render_pass, data);
#ifdef __ANDROID__
  {
    static const bool log_enabled = []() {
      char value[PROP_VALUE_MAX] = {};
      return __system_property_get("debug.blender.log", value) > 0 && atoi(value) != 0;
    }();
    static int counter = 0;
    if (log_enabled && (counter++ % 50) == 0) {
      __android_log_print(ANDROID_LOG_INFO,
                          "blender-rpfb",
                          "caches: render_passes=%d framebuffers=%d (begin #%d)",
                          int(fallback.render_pass_count()),
                          int(fallback.framebuffer_count()),
                          counter);
    }
  }
#endif

  /* Clear values in the same order as the render pass attachments: real color
   * attachments first, then depth/stencil. */
  VkClearValue clear_values[9];
  uint32_t clear_count = 0;
  for (uint32_t i = 0; i < data.vk_rendering_info.colorAttachmentCount; i++) {
    if (data.color_formats[i] == VK_FORMAT_UNDEFINED) {
      continue;
    }
    clear_values[clear_count++] = data.color_attachments[i].clearValue;
  }
  if (data.depth_format != VK_FORMAT_UNDEFINED) {
    clear_values[clear_count++] = data.depth_attachment.clearValue;
  }
  else if (data.stencil_format != VK_FORMAT_UNDEFINED) {
    clear_values[clear_count++] = data.stencil_attachment.clearValue;
  }

  command_buffer.begin_render_pass(vk_render_pass,
                                   vk_framebuffer,
                                   data.vk_rendering_info.renderArea,
                                   Span<VkClearValue>(clear_values, clear_count));
}

void end_rendering_fallback(VKCommandBufferInterface &command_buffer)
{
  command_buffer.end_render_pass();
}

}  // namespace render_graph

}  // namespace blender::gpu

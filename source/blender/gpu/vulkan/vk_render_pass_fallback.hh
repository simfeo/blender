/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Emulates VK_KHR_dynamic_rendering using classic VkRenderPass/VkFramebuffer for
 * devices that don't support it (e.g. Vulkan 1.1 Adreno GPUs).
 *
 * All images in the Vulkan backend are single-sample, so there is no multisample
 * resolve to emulate. Render passes are layout-neutral (initialLayout ==
 * finalLayout == the attachment layout the render graph already transitioned to),
 * so the render graph keeps owning every barrier - the render pass performs no
 * layout transitions of its own.
 */

#pragma once

#include <cstring>
#include <mutex>

#include "xxhash.h"

#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "vk_common.hh"

namespace blender::gpu {

namespace render_graph {
struct VKBeginRenderingData;
}

class VKRenderPassFallback {
 private:
  VkDevice vk_device_ = VK_NULL_HANDLE;
  const VolkDeviceTable *functions_ = nullptr;

  /* POD keys: zero-initialized then filled, hashed/compared over their bytes so
   * padding stays deterministic. */
  struct RenderPassKey {
    uint32_t color_count;
    VkFormat color_formats[8];
    uint8_t color_load[8];
    uint8_t color_store[8];
    /** Per attachment: attachments in one pass can be in different layouts. */
    VkImageLayout color_layouts[8];
    VkFormat depth_format;
    VkFormat stencil_format;
    uint8_t depth_load, depth_store, stencil_load, stencil_store;
    VkImageLayout depth_layout;

    uint64_t hash() const
    {
      return XXH3_64bits(this, sizeof(*this));
    }
    bool operator==(const RenderPassKey &o) const
    {
      return memcmp(this, &o, sizeof(*this)) == 0;
    }
  };

  struct FramebufferKey {
    VkRenderPass render_pass;
    VkImageView views[9];
    uint32_t view_count;
    uint32_t width, height, layers;

    uint64_t hash() const
    {
      return XXH3_64bits(this, sizeof(*this));
    }
    bool operator==(const FramebufferKey &o) const
    {
      return memcmp(this, &o, sizeof(*this)) == 0;
    }
  };

  Map<RenderPassKey, VkRenderPass> render_passes_;
  Map<FramebufferKey, VkFramebuffer> framebuffers_;
  /** Guards the caches (accessed from the submission thread and view-discard). */
  std::mutex mutex_;

  VkRenderPass render_pass_from_key(const RenderPassKey &key);

 public:
  void init(VkDevice vk_device, const VolkDeviceTable &functions)
  {
    vk_device_ = vk_device;
    functions_ = &functions;
  }
  void deinit();

  /** Render pass matching the actual load/store ops, for vkCmdBeginRenderPass. */
  VkRenderPass render_pass_get(const render_graph::VKBeginRenderingData &data);

  /**
   * A render pass compatible (same formats/attachment layout) with the one above,
   * built with generic LOAD/STORE ops, for graphics-pipeline creation.
   */
  VkRenderPass compat_render_pass_get(Span<VkFormat> color_formats,
                                      VkFormat depth_format,
                                      VkFormat stencil_format);

  /** Framebuffer wrapping the attachment image views for `data`. */
  VkFramebuffer framebuffer_get(VkRenderPass vk_render_pass,
                                const render_graph::VKBeginRenderingData &data);

  /**
   * Remove and return the cached framebuffers that reference `vk_image_view`, so a recycled
   * view handle can't match a stale framebuffer. Every path that retires a view must call
   * this and destroy the result no later than the view. Entries are removed as they are
   * returned, so a framebuffer is handed out once however many paths see its view.
   *
   * Only `mutex_` is taken. Callers may hold the discard pool lock, so this must never call
   * into a discard pool.
   */
  Vector<VkFramebuffer> take_framebuffers_for_view(VkImageView vk_image_view);

  int64_t render_pass_count() const
  {
    return render_passes_.size();
  }
  int64_t framebuffer_count() const
  {
    return framebuffers_.size();
  }
};

}  // namespace blender::gpu

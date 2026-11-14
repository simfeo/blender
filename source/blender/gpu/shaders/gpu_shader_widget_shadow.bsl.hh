/* SPDX-FileCopyrightText: 2018-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

namespace builtin::widget {

struct VertInShadow {
  [[attribute(0)]] uint vflag;
};

struct VertOutShadow {
  [[smooth]] float shadowFalloff;
  [[smooth]] float innerMask;
};

struct FragOutShadow {
  [[frag_color(0)]] float4 color;
};

struct WidgetShadow {
  [[push_constant]] float4x4 ModelViewProjectionMatrix;
  [[push_constant]] float alpha;
  [[push_constant]] float4 parameters[4];
};

/**
 * Return one of the nine points of the bottom-left corner arc.
 *
 * The four corner arcs are the same nine points rotated by 90 degrees. Keeping only one arc and
 * selecting it with a switch avoids indexing a constant array with the vertex attribute: Adreno
 * 740's Vulkan compiler lowers that to a private array and then fails to link the pipeline.
 */
float2 shadow_corner_point(uint index)
{
  switch (index) {
    case 0u: return float2(0.0f, 1.0f);
    case 1u: return float2(0.02f, 0.805f);
    case 2u: return float2(0.067f, 0.617f);
    case 3u: return float2(0.169f, 0.45f);
    case 4u: return float2(0.293f, 0.293f);
    case 5u: return float2(0.45f, 0.169f);
    case 6u: return float2(0.617f, 0.076f);
    case 7u: return float2(0.805f, 0.02f);
    case 8u: return float2(1.0f, 0.0f);
    default: return float2(0.0f, 0.0f);
  }
}

[[vertex]] void shadow_vert([[resource_table]] const WidgetShadow &srt,
                            [[in]] const VertInShadow &v_in,
                            [[out]] VertOutShadow &v_out,
                            [[position]] float4 &out_pos)
{
  /* Unpack parameters. */
  float4 recti = srt.parameters[0];
  float4 rect = srt.parameters[1];
  float radsi = srt.parameters[2].x;
  float rads = srt.parameters[2].y;
  float4 roundCorners = srt.parameters[3];

  /* 2 bits for corner. */
  /* Attention! Not the same order as in UI_interface.hh!
   * Ordered by drawing order. */
  constexpr uint BOTTOM_LEFT = 0u;
  constexpr uint BOTTOM_RIGHT = 1u;
  constexpr uint TOP_RIGHT = 2u;
  // constexpr uint TOP_LEFT = 3u; /* Unused. */

  constexpr uint CNR_FLAG_RANGE = uint((1 << 2) - 1);
  /* 4bits for corner id */
  constexpr uint CORNER_VEC_OFS = 2u;
  constexpr uint CORNER_VEC_RANGE = uint((1 << 4) - 1);
  /* is inner vert */
  constexpr uint INNER_FLAG = uint(1 << 10);

  uint cflag = v_in.vflag & CNR_FLAG_RANGE;
  uint vofs = (v_in.vflag >> CORNER_VEC_OFS) & CORNER_VEC_RANGE;

  bool is_inner = (v_in.vflag & INNER_FLAG) != 0u;

  float shadow_width = rads - radsi;
  float shadow_width_top = rect.w - recti.w;

  /* `cflag` is a vertex attribute; select the corner data without indexing an array or vector
   * dynamically (Adreno 740 fails to link such shaders). */
  float round_corner = (cflag == 0u) ? roundCorners.x :
                       (cflag == 1u) ? roundCorners.y :
                       (cflag == 2u) ? roundCorners.z :
                                       roundCorners.w;
  float2 c = float2((cflag == 0u || cflag == 3u) ? 1.0f : -1.0f,
                    (cflag == 0u || cflag == 1u) ? 1.0f : -1.0f);

  float rad_inner = radsi * round_corner;
  float rad_outer = rad_inner + shadow_width;
  float radius = (is_inner) ? rad_inner : rad_outer;

  float shadow_offset = (is_inner && (cflag > BOTTOM_RIGHT)) ? (shadow_width - shadow_width_top) :
                                                               0.0f;

  float2 center_outer = rad_outer * c;
  float2 center = radius * c;

  /* First expand all vertices to the outer shadow border. */
  float2 corner = shadow_corner_point(vofs);
  float2 rotated_corner;
  switch (cflag) {
    case 0u:
      rotated_corner = corner;
      break;
    case 1u:
      rotated_corner = float2(-corner.y, corner.x);
      break;
    case 2u:
      rotated_corner = float2(-corner.x, -corner.y);
      break;
    default:
      rotated_corner = float2(corner.y, -corner.x);
      break;
  }
  float2 v = rad_outer * rotated_corner;

  /* Now shrink the inner vertices onto the inner rectangle.
   * At the top corners we keep the vertical offset to distribute a few of the vertices along the
   * straight part of the rectangle. This allows us to get a better falloff at the top. */
  if (is_inner && (cflag > BOTTOM_RIGHT) && (v.y < (shadow_offset - rad_outer))) {
    v.y += shadow_width_top;
    v.x = 0.0f;
  }
  else {
    v = radius * normalize(v - (center_outer + float2(0.0f, shadow_offset))) + center;
  }

  /* Position to corner */
  float4 rct = (is_inner) ? recti : rect;
  if (cflag == BOTTOM_LEFT) {
    v += rct.xz;
  }
  else if (cflag == BOTTOM_RIGHT) {
    v += rct.yz;
  }
  else if (cflag == TOP_RIGHT) {
    v += rct.yw;
  }
  else /* (cflag == TOP_LEFT) */ {
    v += rct.xw;
  }

  float inner_shadow_strength = min((rect.w - v.y) / rad_outer + 0.1f, 1.0f);
  v_out.shadowFalloff = (is_inner) ? inner_shadow_strength : 0.0f;
  v_out.innerMask = (is_inner) ? 0.0f : 1.0f;

  out_pos = srt.ModelViewProjectionMatrix * float4(v, 0.0f, 1.0f);
}

[[fragment]] void shadow_frag([[in]] const VertOutShadow &v_out,
                              [[out]] FragOutShadow &frag_out,
                              [[resource_table]] WidgetShadow &srt)
{
  /* Manual curve fit of the falloff curve of previous drawing method. */
  float falloff = v_out.shadowFalloff;
  float falloff_sqr = falloff * falloff;
  float shadow_alpha = srt.alpha * (falloff_sqr * 0.722f + falloff * 0.277f);
  float inner_alpha = smoothstep(0.0f, 0.05f, v_out.innerMask);

  frag_out.color = float4(0.0f, 0.0f, 0.0f, inner_alpha * shadow_alpha);
}

}  // namespace builtin::widget

PipelineGraphic gpu_shader_2D_widget_shadow(builtin::widget::shadow_vert,
                                            builtin::widget::shadow_frag);

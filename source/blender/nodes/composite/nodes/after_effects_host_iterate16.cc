/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "after_effects_host.hh"

#include <algorithm>
#include <cmath>

#if __has_include("AE_Effect.h")
#  include "AE_Effect.h"
#  include "AE_EffectCBSuites.h"

namespace blender::nodes::after_effects {

static PF_Pixel16 *pixel16_at_local(PF_EffectWorld *world, int x, int y)
{
  return reinterpret_cast<PF_Pixel16 *>(reinterpret_cast<char *>(world->data) + y * world->rowbytes) + x;
}

static const PF_Pixel16 *pixel16_at_local(const PF_EffectWorld *world, int x, int y)
{
  return reinterpret_cast<const PF_Pixel16 *>(reinterpret_cast<const char *>(world->data) +
                                              y * world->rowbytes) +
         x;
}

static PF_Err iterate16(PF_InData *in_data,
                        A_long /*progress_base*/,
                        A_long /*progress_final*/,
                        PF_EffectWorld *src,
                        const PF_Rect *area,
                        void *refcon,
                        PF_Err (*pix_fn)(void *refcon, A_long x, A_long y, PF_Pixel16 *in, PF_Pixel16 *out),
                        PF_EffectWorld *dst)
{
  if (!in_data || !dst || !pix_fn || !dst->data || !PF_WORLD_IS_DEEP(dst)) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  if (src && (!src->data || !PF_WORLD_IS_DEEP(src))) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  PF_Rect rect = area ? *area : PF_Rect{0, 0, dst->width, dst->height};
  rect.left = std::max<A_long>(0, rect.left);
  rect.top = std::max<A_long>(0, rect.top);
  rect.right = std::min<A_long>(dst->width, rect.right);
  rect.bottom = std::min<A_long>(dst->height, rect.bottom);

  if (src) {
    rect.left = std::max<A_long>(rect.left, 0);
    rect.top = std::max<A_long>(rect.top, 0);
    rect.right = std::min<A_long>(rect.right, src->width);
    rect.bottom = std::min<A_long>(rect.bottom, src->height);
  }

  if (rect.left >= rect.right || rect.top >= rect.bottom) {
    return PF_Err_NONE;
  }

  for (int y = rect.top; y < rect.bottom; y++) {
    PF_Pixel16 *dst_row = pixel16_at_local(dst, 0, y);
    PF_Pixel16 *src_row = src ? pixel16_at_local(src, 0, y) : dst_row;
    for (int x = rect.left; x < rect.right; x++) {
      PF_Err err = pix_fn(refcon, x, y, &src_row[x], &dst_row[x]);
      if (err != PF_Err_NONE) {
        return err;
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err iterate16_origin(PF_InData *in_data,
                                A_long /*progress_base*/,
                                A_long /*progress_final*/,
                                PF_EffectWorld *src,
                                const PF_Rect *area,
                                const PF_Point *origin,
                                void *refcon,
                                PF_Err (*pix_fn)(void *refcon, A_long x, A_long y, PF_Pixel16 *in, PF_Pixel16 *out),
                                PF_EffectWorld *dst)
{
  if (!in_data || !src || !dst || !src->data || !dst->data || !pix_fn || !PF_WORLD_IS_DEEP(src) ||
      !PF_WORLD_IS_DEEP(dst))
  {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const A_long ox = origin ? origin->h : 0;
  const A_long oy = origin ? origin->v : 0;
  PF_Rect rect = area ? *area : PF_Rect{0, 0, dst->width, dst->height};
  rect.left = std::max<A_long>(0, rect.left);
  rect.top = std::max<A_long>(0, rect.top);
  rect.right = std::min<A_long>(dst->width, rect.right);
  rect.bottom = std::min<A_long>(dst->height, rect.bottom);

  for (A_long y = rect.top; y < rect.bottom; y++) {
    const A_long sy = y + oy;
    if (sy < 0 || sy >= src->height) {
      continue;
    }
    PF_Pixel16 *dst_row = pixel16_at_local(dst, 0, y);
    PF_Pixel16 *src_row = pixel16_at_local(src, 0, sy);
    for (A_long x = rect.left; x < rect.right; x++) {
      const A_long sx = x + ox;
      if (sx < 0 || sx >= src->width) {
        continue;
      }
      const PF_Err err = pix_fn(refcon, x, y, &src_row[sx], &dst_row[x]);
      if (err != PF_Err_NONE) {
        return err;
      }
    }
  }

  return PF_Err_NONE;
}

static PF_Err iterate16_origin_non_clip_src(PF_InData *in_data,
                                             A_long /*progress_base*/,
                                             A_long /*progress_final*/,
                                             PF_EffectWorld *src,
                                             const PF_Rect *area,
                                             const PF_Point *origin,
                                             void *refcon,
                                             PF_Err (*pix_fn)(void *refcon, A_long x, A_long y, PF_Pixel16 *in, PF_Pixel16 *out),
                                             PF_EffectWorld *dst)
{
  if (!in_data || !src || !dst || !src->data || !dst->data || !pix_fn || !PF_WORLD_IS_DEEP(src) ||
      !PF_WORLD_IS_DEEP(dst))
  {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const A_long ox = origin ? origin->h : 0;
  const A_long oy = origin ? origin->v : 0;
  PF_Rect rect = area ? *area : PF_Rect{0, 0, dst->width, dst->height};
  rect.left = std::max<A_long>(0, rect.left);
  rect.top = std::max<A_long>(0, rect.top);
  rect.right = std::min<A_long>(dst->width, rect.right);
  rect.bottom = std::min<A_long>(dst->height, rect.bottom);

  const PF_Pixel16 black = {0, 0, 0, 0};

  for (A_long y = rect.top; y < rect.bottom; y++) {
    const A_long sy = y + oy;
    const bool sy_valid = (sy >= 0 && sy < src->height);
    PF_Pixel16 *dst_row = pixel16_at_local(dst, 0, y);
    PF_Pixel16 *src_row = sy_valid ? pixel16_at_local(src, 0, sy) : nullptr;

    for (A_long x = rect.left; x < rect.right; x++) {
      const A_long sx = x + ox;
      const bool in_bounds = sy_valid && (sx >= 0 && sx < src->width);
      PF_Pixel16 temp_in = in_bounds ? src_row[sx] : black;
      const PF_Err err = pix_fn(refcon, x, y, &temp_in, &dst_row[x]);
      if (err != PF_Err_NONE) {
        return err;
      }
    }
  }

  return PF_Err_NONE;
}

void initialize_iterate16_suite1(PF_Iterate16Suite1 &iterate_suite)
{
  iterate_suite.iterate = iterate16;
  iterate_suite.iterate_origin = iterate16_origin;
  iterate_suite.iterate_origin_non_clip_src = iterate16_origin_non_clip_src;
}

void initialize_iterate16_suite2(PF_Iterate16Suite2 &iterate_suite)
{
  iterate_suite.iterate = iterate16;
  iterate_suite.iterate_origin = iterate16_origin;
  iterate_suite.iterate_origin_non_clip_src = iterate16_origin_non_clip_src;
}

}  // namespace blender::nodes::after_effects

#endif

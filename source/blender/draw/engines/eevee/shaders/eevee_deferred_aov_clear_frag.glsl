/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Clear per-layer outline data for secondary deferred layers.
 *
 * AOVs intentionally stay untouched here. Refraction and transmission layers can sample AOVs from
 * the already-rendered surface behind them, and clearing those buffers would make NPR AOV Input see
 * black through transparent surfaces.
 */

#include "infos/eevee_deferred_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_deferred_aov_clear)

#include "eevee_renderpass_lib.glsl"

void main()
{
  clear_outline();
}

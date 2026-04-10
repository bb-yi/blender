#pragma once

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string>
#include <unordered_map>

#if __has_include("AE_Effect.h") && __has_include("AE_PluginData.h")
#  include "AE_Effect.h"
#  include "AE_EffectCBSuites.h"
#  include "AE_PluginData.h"
#  include "SP/SPBasic.h"
#  define WITH_AFTER_EFFECTS_SDK_HEADERS 1
#else
#  define WITH_AFTER_EFFECTS_SDK_HEADERS 0
#endif

namespace blender::nodes::after_effects {

#if WITH_AFTER_EFFECTS_SDK_HEADERS

struct SuiteRegistry {
  std::unordered_map<std::string, const void *> suites;

  void register_suite(const char *name, int32_t version, const void *suite);
  const void *find_suite(const char *name, int32_t version) const;
};

class ScopedActiveSuiteRegistry {
 private:
  const SuiteRegistry *previous_registry_ = nullptr;

 public:
  explicit ScopedActiveSuiteRegistry(const SuiteRegistry &registry);
  ~ScopedActiveSuiteRegistry();
};

void initialize_pica_basic_suite(SPBasicSuite &pica_basic);
void initialize_color_suite(PF_ColorCallbacksSuite1 &color_suite);
void initialize_iterate16_suite1(PF_Iterate16Suite1 &iterate_suite);
void initialize_iterate16_suite2(PF_Iterate16Suite2 &iterate_suite);

#endif

}  // namespace blender::nodes::after_effects

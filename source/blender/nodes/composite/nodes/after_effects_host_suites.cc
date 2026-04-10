/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "after_effects_host_suites.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace blender::nodes::after_effects {

#if WITH_AFTER_EFFECTS_SDK_HEADERS

#  ifndef kSPNoError
#    define kSPNoError 0
#  endif
#  ifndef kSPBadParameterError
#    define kSPBadParameterError 'Parm'
#  endif
#  ifndef kSPOutOfMemoryError
#    define kSPOutOfMemoryError (int32_t(0xFFFFFF6c))
#  endif
#  ifndef kSPSuiteNotFoundError
#    define kSPSuiteNotFoundError '!Fnd'
#  endif

static constexpr double kFixedScale = 65536.0;

static thread_local const SuiteRegistry *active_suite_registry = nullptr;

void SuiteRegistry::register_suite(const char *name, int32_t version, const void *suite)
{
  if (!name || !suite) {
    return;
  }
  const std::string key = std::string(name) + "_v" + std::to_string(version);
  suites[key] = suite;
}

const void *SuiteRegistry::find_suite(const char *name, int32_t version) const
{
  if (!name) {
    return nullptr;
  }
  const std::string key = std::string(name) + "_v" + std::to_string(version);
  const auto it = suites.find(key);
  return (it != suites.end()) ? it->second : nullptr;
}

ScopedActiveSuiteRegistry::ScopedActiveSuiteRegistry(const SuiteRegistry &registry)
{
  previous_registry_ = active_suite_registry;
  active_suite_registry = &registry;
}

ScopedActiveSuiteRegistry::~ScopedActiveSuiteRegistry()
{
  active_suite_registry = previous_registry_;
}

static SPErr suite_acquire_suite(const char *name, int32_t version, const void **suite)
{
  if (!name || !suite) {
    return kSPBadParameterError;
  }

  const void *resolved_suite = nullptr;
  if (active_suite_registry) {
    resolved_suite = active_suite_registry->find_suite(name, version);
  }

  *suite = resolved_suite;
  return resolved_suite ? kSPNoError : kSPSuiteNotFoundError;
}

static SPErr suite_release_suite(const char * /*name*/, int32_t /*version*/)
{
  return kSPNoError;
}

static SPBoolean suite_is_equal(const char *token1, const char *token2)
{
  if (!token1 || !token2) {
    return false;
  }
  return std::strcmp(token1, token2) == 0;
}

static SPErr suite_allocate_block(size_t size, void **block)
{
  if (!block) {
    return kSPBadParameterError;
  }
  *block = std::malloc(size);
  return *block ? kSPNoError : kSPOutOfMemoryError;
}

static SPErr suite_free_block(void *block)
{
  if (block) {
    std::free(block);
  }
  return kSPNoError;
}

static SPErr suite_reallocate_block(void *block, size_t new_size, void **new_block)
{
  if (!new_block) {
    return kSPBadParameterError;
  }
  *new_block = std::realloc(block, new_size);
  return *new_block ? kSPNoError : kSPOutOfMemoryError;
}

static SPErr suite_undefined()
{
  return kSPSuiteNotFoundError;
}

void initialize_pica_basic_suite(SPBasicSuite &pica_basic)
{
  pica_basic.AcquireSuite = suite_acquire_suite;
  pica_basic.ReleaseSuite = suite_release_suite;
  pica_basic.IsEqual = suite_is_equal;
  pica_basic.AllocateBlock = suite_allocate_block;
  pica_basic.FreeBlock = suite_free_block;
  pica_basic.ReallocateBlock = suite_reallocate_block;
  pica_basic.Undefined = suite_undefined;
}

static double fixed_to_double(PF_Fixed value)
{
  return double(value) / kFixedScale;
}

static PF_Fixed double_to_fixed(double value)
{
  return PF_Fixed(std::lround(value * kFixedScale));
}

static void rgb_to_hls_values(double r, double g, double b, double &h, double &l, double &s)
{
  const double max_v = std::max({r, g, b});
  const double min_v = std::min({r, g, b});
  const double delta = max_v - min_v;

  l = (max_v + min_v) * 0.5;
  if (delta <= 1e-12) {
    h = 0.0;
    s = 0.0;
    return;
  }

  s = delta / (1.0 - std::abs(2.0 * l - 1.0));
  if (max_v == r) {
    h = std::fmod(((g - b) / delta), 6.0);
  }
  else if (max_v == g) {
    h = ((b - r) / delta) + 2.0;
  }
  else {
    h = ((r - g) / delta) + 4.0;
  }
  h /= 6.0;
  if (h < 0.0) {
    h += 1.0;
  }
}

static double hue_to_rgb(double p, double q, double t)
{
  if (t < 0.0) {
    t += 1.0;
  }
  if (t > 1.0) {
    t -= 1.0;
  }
  if (t < 1.0 / 6.0) {
    return p + (q - p) * 6.0 * t;
  }
  if (t < 0.5) {
    return q;
  }
  if (t < 2.0 / 3.0) {
    return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  }
  return p;
}

static PF_Err color_rgb_to_hls(PF_ProgPtr /*effect_ref*/, PF_Pixel *rgb, PF_HLS_Pixel hls)
{
  if (!rgb) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const double r = std::clamp(double(rgb->red) / 255.0, 0.0, 1.0);
  const double g = std::clamp(double(rgb->green) / 255.0, 0.0, 1.0);
  const double b = std::clamp(double(rgb->blue) / 255.0, 0.0, 1.0);

  double h = 0.0;
  double l = 0.0;
  double s = 0.0;
  rgb_to_hls_values(r, g, b, h, l, s);

  hls[0] = double_to_fixed(h);
  hls[1] = double_to_fixed(l);
  hls[2] = double_to_fixed(s);
  return PF_Err_NONE;
}

static PF_Err color_hls_to_rgb(PF_ProgPtr /*effect_ref*/, PF_HLS_Pixel hls, PF_Pixel *rgb)
{
  if (!rgb) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const double h = fixed_to_double(hls[0]);
  const double l = std::clamp(fixed_to_double(hls[1]), 0.0, 1.0);
  const double s = std::clamp(fixed_to_double(hls[2]), 0.0, 1.0);

  double r = l;
  double g = l;
  double b = l;

  if (s > 1e-12) {
    const double q = (l < 0.5) ? (l * (1.0 + s)) : (l + s - l * s);
    const double p = 2.0 * l - q;
    r = hue_to_rgb(p, q, h + 1.0 / 3.0);
    g = hue_to_rgb(p, q, h);
    b = hue_to_rgb(p, q, h - 1.0 / 3.0);
  }

  rgb->red = A_u_char(std::clamp(std::lround(r * 255.0), 0L, 255L));
  rgb->green = A_u_char(std::clamp(std::lround(g * 255.0), 0L, 255L));
  rgb->blue = A_u_char(std::clamp(std::lround(b * 255.0), 0L, 255L));
  return PF_Err_NONE;
}

static PF_Err color_rgb_to_yiq(PF_ProgPtr /*effect_ref*/, PF_Pixel *rgb, PF_YIQ_Pixel yiq)
{
  if (!rgb) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const double r = std::clamp(double(rgb->red) / 255.0, 0.0, 1.0);
  const double g = std::clamp(double(rgb->green) / 255.0, 0.0, 1.0);
  const double b = std::clamp(double(rgb->blue) / 255.0, 0.0, 1.0);

  const double y = 0.299 * r + 0.587 * g + 0.114 * b;
  const double i = 0.596 * r - 0.274 * g - 0.322 * b;
  const double q = 0.211 * r - 0.523 * g + 0.312 * b;

  yiq[0] = double_to_fixed(y);
  yiq[1] = double_to_fixed(i);
  yiq[2] = double_to_fixed(q);
  return PF_Err_NONE;
}

static PF_Err color_yiq_to_rgb(PF_ProgPtr /*effect_ref*/, PF_HLS_Pixel yiq, PF_Pixel *rgb)
{
  if (!rgb) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const double y = fixed_to_double(yiq[0]);
  const double i = fixed_to_double(yiq[1]);
  const double q = fixed_to_double(yiq[2]);

  const double r = std::clamp(y + 0.956 * i + 0.621 * q, 0.0, 1.0);
  const double g = std::clamp(y - 0.272 * i - 0.647 * q, 0.0, 1.0);
  const double b = std::clamp(y - 1.106 * i + 1.703 * q, 0.0, 1.0);

  rgb->red = A_u_char(std::clamp(std::lround(r * 255.0), 0L, 255L));
  rgb->green = A_u_char(std::clamp(std::lround(g * 255.0), 0L, 255L));
  rgb->blue = A_u_char(std::clamp(std::lround(b * 255.0), 0L, 255L));
  return PF_Err_NONE;
}

static PF_Err color_luminance(PF_ProgPtr /*effect_ref*/, PF_Pixel *rgb, A_long *lum100)
{
  if (!rgb || !lum100) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }
  *lum100 = A_long((0.299 * rgb->red + 0.587 * rgb->green + 0.114 * rgb->blue) * 100.0 / 255.0);
  return PF_Err_NONE;
}

static PF_Err color_hue(PF_ProgPtr /*effect_ref*/, PF_Pixel *rgb, A_long *hue)
{
  if (!rgb || !hue) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const double r = std::clamp(double(rgb->red) / 255.0, 0.0, 1.0);
  const double g = std::clamp(double(rgb->green) / 255.0, 0.0, 1.0);
  const double b = std::clamp(double(rgb->blue) / 255.0, 0.0, 1.0);

  double h = 0.0;
  double l = 0.0;
  double s = 0.0;
  rgb_to_hls_values(r, g, b, h, l, s);
  if (s <= 1e-12) {
    *hue = PF_HUE_UNDEFINED;
    return PF_Err_NONE;
  }

  *hue = A_long(std::clamp(std::lround(h * 255.0), 0L, 255L));
  return PF_Err_NONE;
}

static PF_Err color_lightness(PF_ProgPtr /*effect_ref*/, PF_Pixel *rgb, A_long *lightness)
{
  if (!rgb || !lightness) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const double max_v = std::max({double(rgb->red), double(rgb->green), double(rgb->blue)});
  const double min_v = std::min({double(rgb->red), double(rgb->green), double(rgb->blue)});
  const double l = (max_v + min_v) * 0.5 / 255.0;
  *lightness = A_long(std::clamp(std::lround(l * 255.0), 0L, 255L));
  return PF_Err_NONE;
}

static PF_Err color_saturation(PF_ProgPtr /*effect_ref*/, PF_Pixel *rgb, A_long *saturation)
{
  if (!rgb || !saturation) {
    return PF_Err_BAD_CALLBACK_PARAM;
  }

  const double r = std::clamp(double(rgb->red) / 255.0, 0.0, 1.0);
  const double g = std::clamp(double(rgb->green) / 255.0, 0.0, 1.0);
  const double b = std::clamp(double(rgb->blue) / 255.0, 0.0, 1.0);

  double h = 0.0;
  double l = 0.0;
  double s = 0.0;
  rgb_to_hls_values(r, g, b, h, l, s);
  *saturation = A_long(std::clamp(std::lround(s * 255.0), 0L, 255L));
  return PF_Err_NONE;
}

void initialize_color_suite(PF_ColorCallbacksSuite1 &color_suite)
{
  color_suite.RGBtoHLS = color_rgb_to_hls;
  color_suite.HLStoRGB = color_hls_to_rgb;
  color_suite.RGBtoYIQ = color_rgb_to_yiq;
  color_suite.YIQtoRGB = color_yiq_to_rgb;
  color_suite.Luminance = color_luminance;
  color_suite.Hue = color_hue;
  color_suite.Lightness = color_lightness;
  color_suite.Saturation = color_saturation;
}

#endif

}  // namespace blender::nodes::after_effects

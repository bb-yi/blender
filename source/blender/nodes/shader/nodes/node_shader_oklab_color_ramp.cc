/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <cmath>
#include <memory>
#include <utility>

#include "DNA_colorband_types.h"
#include "DNA_texture_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_colorband.hh"

#include "BLI_color.hh"

#include "NOD_multi_function.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_oklab_color_ramp_cc {

/* OKLab color space conversion functions. */

static float3 linear_srgb_to_oklab(const float3 &c)
{
  /* Convert Linear sRGB to LMS (cone response). */
  const float l = 0.4122214708f * c.x + 0.5363325363f * c.y + 0.0514459929f * c.z;
  const float m = 0.2119034982f * c.x + 0.6806995451f * c.y + 0.1073969566f * c.z;
  const float s = 0.0883024619f * c.x + 0.2817188376f * c.y + 0.6299787005f * c.z;

  /* Apply cube root. */
  const float l_ = (l >= 0.0f) ? powf(l, 1.0f / 3.0f) : -powf(-l, 1.0f / 3.0f);
  const float m_ = (m >= 0.0f) ? powf(m, 1.0f / 3.0f) : -powf(-m, 1.0f / 3.0f);
  const float s_ = (s >= 0.0f) ? powf(s, 1.0f / 3.0f) : -powf(-s, 1.0f / 3.0f);

  /* Convert to OKLab. */
  return float3(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

static float3 oklab_to_linear_srgb(const float3 &c)
{
  /* Convert OKLab back to LMS cone response. */
  const float l_ = c.x + 0.3963377774f * c.y + 0.2158037573f * c.z;
  const float m_ = c.x - 0.1055613458f * c.y - 0.0638541728f * c.z;
  const float s_ = c.x - 0.0894841775f * c.y - 1.2914855480f * c.z;

  /* Apply cube (power of 3). */
  const float l = l_ * l_ * l_;
  const float m = m_ * m_ * m_;
  const float s = s_ * s_ * s_;

  /* Convert back to Linear sRGB. */
  return float3(+4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);
}

static float3 srgb_to_linear(const float3 &color)
{
  const float3 a = color / 12.92f;
  const float3 temp = (color + 0.055f) / 1.055f;
  const float3 b = float3(powf(temp.x, 2.4f), powf(temp.y, 2.4f), powf(temp.z, 2.4f));
  const float3 selector = float3(color.x > 0.04045f ? 1.0f : 0.0f,
                                 color.y > 0.04045f ? 1.0f : 0.0f,
                                 color.z > 0.04045f ? 1.0f : 0.0f);
  return a * (float3(1.0f) - selector) + b * selector;
}

static float3 linear_to_srgb(const float3 &color)
{
  const float3 a = color * 12.92f;
  const float3 powered = float3(
      powf(color.x, 1.0f / 2.4f), powf(color.y, 1.0f / 2.4f), powf(color.z, 1.0f / 2.4f));
  const float3 b = 1.055f * powered - 0.055f;
  const float3 selector = float3(color.x > 0.0031308f ? 1.0f : 0.0f,
                                 color.y > 0.0031308f ? 1.0f : 0.0f,
                                 color.z > 0.0031308f ? 1.0f : 0.0f);
  return a * (float3(1.0f) - selector) + b * selector;
}

/* OKLab-based colorband evaluation. */
static void oklab_colorband_evaluate(const ColorBand *coba, float in, ColorGeometry4f &out)
{
  in = math::clamp(in, 0.0f, 1.0f);

  if (coba->tot == 0) {
    out = ColorGeometry4f(0.0f, 0.0f, 0.0f, 0.0f);
    return;
  }

  if (coba->tot == 1) {
    const CBData &cbd = coba->data[0];
    out = ColorGeometry4f(cbd.r, cbd.g, cbd.b, cbd.a);
    return;
  }

  int left_index = 0;
  int right_index = 0;

  for (int i = 0; i < coba->tot; i++) {
    if (coba->data[i].pos <= in) {
      left_index = i;
    }
    else {
      right_index = i;
      break;
    }
  }

  if (left_index == right_index) {
    const CBData &cbd = (in <= coba->data[0].pos) ? coba->data[0] : coba->data[coba->tot - 1];
    out = ColorGeometry4f(cbd.r, cbd.g, cbd.b, cbd.a);
    return;
  }

  const CBData &left = coba->data[left_index];
  const CBData &right = coba->data[right_index];
  const float factor = math::clamp((in - left.pos) / (right.pos - left.pos), 0.0f, 1.0f);

  /* Convert colors to Linear sRGB first. */
  const float3 left_linear = srgb_to_linear(float3(left.r, left.g, left.b));
  const float3 right_linear = srgb_to_linear(float3(right.r, right.g, right.b));

  /* Convert to OKLab, interpolate in OKLab space, then convert back to sRGB. */
  const float3 left_oklab = linear_srgb_to_oklab(left_linear);
  const float3 right_oklab = linear_srgb_to_oklab(right_linear);
  const float3 mixed_oklab = math::interpolate(left_oklab, right_oklab, factor);
  const float3 mixed_srgb = linear_to_srgb(oklab_to_linear_srgb(mixed_oklab));

  const float mixed_alpha = left.a + factor * (right.a - left.a);
  out = ColorGeometry4f(math::clamp(mixed_srgb.x, 0.0f, 1.0f),
                        math::clamp(mixed_srgb.y, 0.0f, 1.0f),
                        math::clamp(mixed_srgb.z, 0.0f, 1.0f),
                        math::clamp(mixed_alpha, 0.0f, 1.0f));
}

static void sh_node_oklab_valtorgb_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Factor"_ustr, "Fac"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("The value used to map onto the OKLab color gradient");
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Alpha"_ustr);
}

static void node_shader_init_oklab_valtorgb(bNodeTree * /*ntree*/, bNode *node)
{
  ColorBand *coba = BKE_colorband_add(true);
  coba->color_mode = COLBAND_BLEND_OKLAB;
  node->storage = coba;
}

static int gpu_shader_oklab_valtorgb(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack *in,
                                     GPUNodeStack *out)
{
  ColorBand *coba = static_cast<ColorBand *>(node->storage);

  if (coba->tot == 1) {
    return GPU_stack_link(mat,
                          node,
                          "oklab_valtorgb_opti_constant",
                          in,
                          out,
                          GPU_constant(&coba->data[0].pos),
                          GPU_uniform(&coba->data[0].r),
                          GPU_uniform(&coba->data[0].r));
  }

  if (coba->tot == 2) {
    float mul_bias[2];
    mul_bias[0] = 1.0f / (coba->data[1].pos - coba->data[0].pos);
    mul_bias[1] = -mul_bias[0] * coba->data[0].pos;
    return GPU_stack_link(mat,
                          node,
                          "oklab_valtorgb_opti_ease",
                          in,
                          out,
                          GPU_uniform(mul_bias),
                          GPU_uniform(&coba->data[0].r),
                          GPU_uniform(&coba->data[1].r));
  }

  const int size = CM_TABLE + 1;
  float *array = MEM_new_array_uninitialized<float>(size * 4, "OKLab Colorband Array");

  const eColorBand_Interp original_ipotype = coba->ipotype;
  coba->ipotype = COLBAND_INTERP_EASE;

  for (int i = 0; i < size; i++) {
    const float pos = float(i) / float(CM_TABLE);
    float color[4];
    BKE_colorband_evaluate_oklab(coba, pos, color);
    array[i * 4 + 0] = color[0];
    array[i * 4 + 1] = color[1];
    array[i * 4 + 2] = color[2];
    array[i * 4 + 3] = color[3];
  }

  coba->ipotype = original_ipotype;

  float layer;
  GPUNodeLink *tex = GPU_color_band(mat, size, array, &layer);
  return GPU_stack_link(mat, node, "oklab_valtorgb", in, out, tex, GPU_constant(&layer));
}

class OKLabColorBandFunction : public mf::MultiFunction {
 private:
  std::shared_ptr<const bNodeTree> tree_;
  const ColorBand &color_band_;

 public:
  OKLabColorBandFunction(const ColorBand &color_band, std::shared_ptr<const bNodeTree> tree)
      : tree_(std::move(tree)), color_band_(color_band)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"OKLab Color Band", signature};
      builder.single_input<float>("Value");
      builder.single_output<ColorGeometry4f>("Color");
      builder.single_output<float>("Alpha");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &values = params.readonly_single_input<float>(0, "Value");
    MutableSpan<ColorGeometry4f> colors = params.uninitialized_single_output<ColorGeometry4f>(
        1, "Color");
    MutableSpan<float> alphas = params.uninitialized_single_output<float>(2, "Alpha");

    mask.foreach_index([&](const int64_t i) {
      ColorGeometry4f color;
      oklab_colorband_evaluate(&color_band_, values[i], color);
      colors[i] = color;
      alphas[i] = color.a;
    });
  }
};

static void sh_node_oklab_valtorgb_build_multi_function(
    nodes::NodeMultiFunctionBuilder &builder)
{
  const bNode &bnode = builder.node();
  const ColorBand *color_band = static_cast<const ColorBand *>(bnode.storage);
  builder.construct_and_set_matching_fn<OKLabColorBandFunction>(*color_band,
                                                                builder.shared_tree());
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* TODO: Implement */
  NodeItem res = empty();
  return res;
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_oklab_color_ramp_cc

void register_node_type_sh_oklab_color_ramp()
{
  namespace file_ns = nodes::node_shader_oklab_color_ramp_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeOKLabColorRamp"_ustr, SH_NODE_OKLAB_COLOR_RAMP);
  ntype.ui_name = "OKLab Color Ramp";
  ntype.ui_description = "Map values to colors using an OKLab color gradient";
  ntype.enum_name_legacy = "OKLAB_COLOR_RAMP";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::sh_node_oklab_valtorgb_declare;
  ntype.initfunc = file_ns::node_shader_init_oklab_valtorgb;
  ntype.default_width = bke::NodeWidth::_240;
  ntype.minwidth = bke::NodeWidth::_140;
  bke::node_type_storage(
      ntype, "ColorBand", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::gpu_shader_oklab_valtorgb;
  ntype.build_multi_function = file_ns::sh_node_oklab_valtorgb_build_multi_function;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender

/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_defs.h"
#include "DNA_listBase.h"

#include "BLI_enum_flags.hh"

namespace blender {

#ifndef MAX_MTEX
#  define MAX_MTEX 18
#endif

struct AnimData;
struct Image;
struct bNodeTree;

/* MaterialGPencilStyle->flag */
enum eMaterialGPencilStyle_Flag : short {
  /* Fill Texture is a pattern */
  GP_MATERIAL_FILL_PATTERN = (1 << 0),
  /* don't display color */
  GP_MATERIAL_HIDE = (1 << 1),
  /* protected from further editing */
  GP_MATERIAL_LOCKED = (1 << 2),
  /* do onion skinning */
  GP_MATERIAL_HIDE_ONIONSKIN = (1 << 3),
  /* clamp texture */
  GP_MATERIAL_TEX_CLAMP = (1 << 4),
  /* mix fill texture */
  GP_MATERIAL_FILL_TEX_MIX = (1 << 5),
  /* Flip fill colors */
  GP_MATERIAL_FLIP_FILL = (1 << 6),
  /* Stroke Texture is a pattern */
  GP_MATERIAL_STROKE_PATTERN = (1 << 7),
  GP_MATERIAL_STROKE_SHOW = (1 << 8), /* Deprecated. Only used for compatibility. */
  GP_MATERIAL_FILL_SHOW = (1 << 9),   /* Deprecated. Only used for compatibility. */
  /* mix stroke texture */
  GP_MATERIAL_STROKE_TEX_MIX = (1 << 11),
  /* disable stencil clipping (overlap) */
  GP_MATERIAL_DISABLE_STENCIL = (1 << 12),
  /* Material used as stroke masking. */
  GP_MATERIAL_IS_STROKE_HOLDOUT = (1 << 13),
  /* Material used as fill masking. */
  GP_MATERIAL_IS_FILL_HOLDOUT = (1 << 14),
  /* Material use randomization. */
  GP_MATERIAL_USE_DOTS_RANDOMIZATION = static_cast<short>(1 << 15),
};
ENUM_OPERATORS(eMaterialGPencilStyle_Flag)

enum eMaterialGPencilStyle_Mode : int {
  GP_MATERIAL_MODE_LINE = 0,
  GP_MATERIAL_MODE_DOT = 1,
  GP_MATERIAL_MODE_SQUARE = 2,
};

enum eMaterialLineArtFlags : int {
  LRT_MATERIAL_MASK_ENABLED = (1 << 0),
  LRT_MATERIAL_CUSTOM_OCCLUSION_EFFECTIVENESS = (1 << 1),
  LRT_MATERIAL_CUSTOM_INTERSECTION_PRIORITY = (1 << 2),
};
ENUM_OPERATORS(eMaterialLineArtFlags)

/* maximum number of materials per material array.
 * (on object, mesh, light, etc.). limited by
 * short mat_nr in verts, faces.
 * -1 because for active material we store the index + 1 */
#define MAXMAT (32767 - 1)

/** #Material::flag */
enum eMaterial_Flag : short {
  /** For render. */
  MA_IS_USED = 1 << 0, /* UNUSED */
  /** For dope-sheet. */
  MA_DS_EXPAND = 1 << 1,
  /**
   * For dope-sheet (texture stack expander)
   * NOTE: this must have the same value as other texture stacks,
   * otherwise anim-editors will not read correctly.
   */
  MA_DS_SHOW_TEXS = 1 << 2,
};
ENUM_OPERATORS(eMaterial_Flag)

/* ramps */
enum eMaterial_RampBlend : int {
  MA_RAMP_BLEND = 0,
  MA_RAMP_ADD = 1,
  MA_RAMP_MULT = 2,
  MA_RAMP_SUB = 3,
  MA_RAMP_SCREEN = 4,
  MA_RAMP_DIV = 5,
  MA_RAMP_DIFF = 6,
  MA_RAMP_DARK = 7,
  MA_RAMP_LIGHT = 8,
  MA_RAMP_OVERLAY = 9,
  MA_RAMP_DODGE = 10,
  MA_RAMP_BURN = 11,
  MA_RAMP_HUE = 12,
  MA_RAMP_SAT = 13,
  MA_RAMP_VAL = 14,
  MA_RAMP_COLOR = 15,
  MA_RAMP_SOFT = 16,
  MA_RAMP_LINEAR = 17,
  MA_RAMP_EXCLUSION = 18,
};

/** #MTex::texco */
enum eMTex_TexCo : int {
  TEXCO_ORCO = 1 << 0,
  // TEXCO_REFL = 1 << 1, /* Deprecated. */
  // TEXCO_NORM = 1 << 2, /* Deprecated. */
  TEXCO_GLOB = 1 << 3,
  TEXCO_UV = 1 << 4,
  TEXCO_OBJECT = 1 << 5,
  // TEXCO_LAVECTOR = 1 << 6, /* Deprecated. */
  // TEXCO_VIEW = 1 << 7,     /* Deprecated. */
  // TEXCO_STICKY = 1 << 8,   /* Deprecated. */
  // TEXCO_OSA = 1 << 9,      /* Deprecated. */
  TEXCO_WINDOW = 1 << 10,
  // NEED_UV = 1 << 11,       /* Deprecated. */
  // TEXCO_TANGENT = 1 << 12, /* Deprecated. */
  /** still stored in `vertex->accum`, 1 D. */
  TEXCO_STRAND = 1 << 13,
  /** strand is used for normal materials, particle for halo materials */
  TEXCO_PARTICLE = 1 << 13,
  // TEXCO_STRESS = 1 << 14, /* Deprecated. */
  // TEXCO_SPEED = 1 << 15,  /* Deprecated. */
};
ENUM_OPERATORS(eMTex_TexCo)

/** #MTex::mapto */
enum eMTex_MapTo : int {
  MAP_COL = 1 << 0,
  MAP_ALPHA = 1 << 7,
};
ENUM_OPERATORS(eMTex_MapTo)

/** #Material::pr_type */
enum ePreviewType : char {
  MA_FLAT = 0,
  MA_SPHERE = 1,
  MA_CUBE = 2,
  MA_SHADERBALL = 3,
  MA_SPHERE_A = 4, /* Used for icon renders only. */
  MA_TEXTURE = 5,
  MA_LAMP = 6,
  MA_SKY = 7,
  MA_HAIR = 10,
  MA_ATMOS = 11,
  MA_CLOTH = 12,
  MA_FLUID = 13,
};

/** #Material::pr_flag */
enum eMaterial_PreviewFlag : short {
  MA_PREVIEW_WORLD = 1 << 0,
};
ENUM_OPERATORS(eMaterial_PreviewFlag)

/** #Material::surface_render_method */
enum eMaterial_SurfaceRenderMethod : char {
  MA_SURFACE_METHOD_DEFERRED = 0,
  MA_SURFACE_METHOD_FORWARD = 1,
};

/** #Material::eevee_domain */
enum {
  MA_EEVEE_DOMAIN_SURFACE = 0,
  MA_EEVEE_DOMAIN_FILTER = 1,
};

/** #Material::volume_intersection_method */
enum eMaterial_VolumeIntersectionMethod : char {
  MA_VOLUME_ISECT_FAST = 0,
  MA_VOLUME_ISECT_ACCURATE = 1,
};

/** #Material::blend_method */
enum eMaterial_BlendMethod : char {
  MA_BM_SOLID = 0,
  // MA_BM_ADD = 1, /* deprecated */
  // MA_BM_MULTIPLY = 2,  /* deprecated */
  MA_BM_CLIP = 3,
  MA_BM_HASHED = 4,
  MA_BM_BLEND = 5,
};

/** #Material::blend_flag */
enum eMaterial_BlendFlag : char {
  MA_BL_HIDE_BACKFACE = (1 << 0),
  MA_BL_SS_REFRACTION = (1 << 1),
  MA_BL_CULL_BACKFACE = (1 << 2),
  MA_BL_TRANSLUCENCY = (1 << 3),
  MA_BL_LIGHTPROBE_VOLUME_DOUBLE_SIDED = (1 << 4),
  MA_BL_CULL_BACKFACE_SHADOW = (1 << 5),
  MA_BL_TRANSPARENT_SHADOW = (1 << 6),
  MA_BL_THICKNESS_FROM_SHADOW = static_cast<char>(1 << 7),
};
ENUM_OPERATORS(eMaterial_BlendFlag)

/** #Material::surface_cull_method */
enum eMaterialCullMethod {
  MA_SURFACE_CULL_NONE = 0,
  MA_SURFACE_CULL_BACK = 1,
  MA_SURFACE_CULL_FRONT = 2,
};

/** #Material::ztest_mode */
enum eMaterialZTestMode {
  MA_ZTEST_LESS_EQUAL = 0,
  MA_ZTEST_LESS = 1,
  MA_ZTEST_GREATER = 2,
  MA_ZTEST_GREATER_EQUAL = 3,
  MA_ZTEST_EQUAL = 4,
  MA_ZTEST_NOT_EQUAL = 5,
  MA_ZTEST_ALWAYS = 6,
  MA_ZTEST_NEVER = 7,
};

/** #Material::stencil_test */
enum eMaterialStencilTest {
  MA_STENCIL_ALWAYS = 0,
  MA_STENCIL_NEVER = 1,
  MA_STENCIL_EQUAL = 2,
  MA_STENCIL_NOT_EQUAL = 3,
  MA_STENCIL_LESS = 4,
  MA_STENCIL_LESS_EQUAL = 5,
  MA_STENCIL_GREATER = 6,
  MA_STENCIL_GREATER_EQUAL = 7,
};

/** #Material::stencil_pass_op, #Material::stencil_fail_op, #Material::stencil_zfail_op */
enum eMaterialStencilOp {
  MA_STENCIL_OP_KEEP = 0,
  MA_STENCIL_OP_ZERO = 1,
  MA_STENCIL_OP_REPLACE = 2,
  MA_STENCIL_OP_INCREMENT_CLAMP = 3,
  MA_STENCIL_OP_DECREMENT_CLAMP = 4,
  MA_STENCIL_OP_INVERT = 5,
  MA_STENCIL_OP_INCREMENT_WRAP = 6,
  MA_STENCIL_OP_DECREMENT_WRAP = 7,
};

/** #Material::blend_shadow */
enum eMaterial_BlendShadow : char {
  MA_BS_NONE = 0,
  MA_BS_SOLID = 1,
  MA_BS_CLIP = 2,
  MA_BS_HASHED = 3,
};

/** #Material::displacement_method */
enum eMaterial_DisplacementMethod : char {
  MA_DISPLACEMENT_BUMP = 0,
  MA_DISPLACEMENT_DISPLACE = 1,
  MA_DISPLACEMENT_BOTH = 2,
};

/** #Material::thickness_mode */
enum eMaterial_ThicknessMode : char {
  MA_THICKNESS_SPHERE = 0,
  MA_THICKNESS_SLAB = 1,
};

/* Grease Pencil Stroke styles */
enum eMaterialGPencilStyle_StrokeStyle : short {
  GP_MATERIAL_STROKE_STYLE_SOLID = 0,
  GP_MATERIAL_STROKE_STYLE_TEXTURE = 1,
};

/* Grease Pencil Fill styles */
enum eMaterialGPencilStyle_FillStyle : short {
  GP_MATERIAL_FILL_STYLE_SOLID = 0,
  GP_MATERIAL_FILL_STYLE_GRADIENT = 1,
  GP_MATERIAL_FILL_STYLE_CHECKER = 2, /* DEPRECATED (only for convert old files) */
  GP_MATERIAL_FILL_STYLE_TEXTURE = 3,
};

/* Grease Pencil Gradient Types */
enum eMaterialGPencilStyle_GradientType : int {
  GP_MATERIAL_GRADIENT_LINEAR = 0,
  GP_MATERIAL_GRADIENT_RADIAL = 1,
};

/* Grease Pencil Follow Drawing Modes */
enum eMaterialGPencilStyle_FollowMode : int {
  GP_MATERIAL_FOLLOW_PATH = 0,
  GP_MATERIAL_FOLLOW_OBJ = 1,
  GP_MATERIAL_FOLLOW_FIXED = 2,
};

/* Grease Pencil Placement Drawing Modes */
enum eMaterialGPencilPlacementMode : int {
  GP_MATERIAL_PLACEMENT_COUNT = 0,
  GP_MATERIAL_PLACEMENT_RADIUS = 1,
  GP_MATERIAL_PLACEMENT_DENSITY = 2,
};

struct TexPaintSlot {
  DNA_DEFINE_CXX_METHODS(TexPaintSlot)

  /** Image to be painted on. Mutual exclusive with attribute_name. */
  struct Image *ima = nullptr;
  struct ImageUser *image_user = nullptr;

  /**
   * Custom-data index for uv layer, #MAX_CUSTOMDATA_LAYER_NAME_NO_PREFIX.
   * May reference #NodeShaderUVMap::uv_name.
   */
  char *uvname = nullptr;
  /**
   * Color attribute name when painting using color attributes. Mutual exclusive with ima.
   * Points to the name of a CustomDataLayer.
   */
  char *attribute_name = nullptr;
  /** Do we have a valid image and UV map or attribute. */
  int valid = 0;
  /** Copy of node interpolation setting. */
  int interp = 0;
};

struct MaterialGPencilStyle {
  DNA_DEFINE_CXX_METHODS(MaterialGPencilStyle)

  /** Texture image for strokes. */
  struct Image *sima = nullptr;
  /** Texture image for filling. */
  struct Image *ima = nullptr;
  /** Color for paint and strokes (alpha included). */
  float stroke_rgba[4] = {};
  /** Color that should be used for drawing "fills" for strokes (alpha included). */
  float fill_rgba[4] = {};
  /** Secondary color used for gradients and other stuff. */
  float mix_rgba[4] = {};
  /** Settings. */
  eMaterialGPencilStyle_Flag flag = {};
  /** Custom index for passes. */
  short index = 0;
  /** Style for drawing strokes (used to select shader type). */
  eMaterialGPencilStyle_StrokeStyle stroke_style = GP_MATERIAL_STROKE_STYLE_SOLID;
  /** Style for filling areas (used to select shader type). */
  eMaterialGPencilStyle_FillStyle fill_style = GP_MATERIAL_FILL_STYLE_SOLID;
  /** Factor used to define shader behavior (several uses). */
  float mix_factor = 0;
  /** Angle used for gradients orientation. */
  DNA_DEPRECATED float gradient_angle = 0;
  /** Radius for radial gradients. */
  DNA_DEPRECATED float gradient_radius = 0;
  char _pad2[4] = {};
  /** UV coordinates scale. */
  DNA_DEPRECATED float gradient_scale[2] = {};
  /** Factor to shift filling in 2d space. */
  DNA_DEPRECATED float gradient_shift[2] = {};
  /** Angle used for texture orientation. */
  float texture_angle = 0;
  /** Texture scale (separated of uv scale). */
  float texture_scale[2] = {};
  /** Factor to shift texture in 2d space. */
  float texture_offset[2] = {};
  /** Texture opacity. */
  DNA_DEPRECATED float texture_opacity = 0;
  /** Pixel size for uv along the stroke. */
  float texture_pixsize = 0;
  /** Drawing mode (line or dots). */
  eMaterialGPencilStyle_Mode mode = GP_MATERIAL_MODE_LINE;

  /** Type of gradient. */
  eMaterialGPencilStyle_GradientType gradient_type = GP_MATERIAL_GRADIENT_LINEAR;

  /** Factor used to mix texture and stroke color. */
  float mix_stroke_factor = 0;
  /** Mode used to align Dots and Boxes with stroke drawing path and object rotation */
  eMaterialGPencilStyle_FollowMode alignment_mode = GP_MATERIAL_FOLLOW_PATH;
  /** Rotation for texture for Dots and Squares. */
  float alignment_rotation = 0;
  /** Placement mode for Dots and Squares. */
  eMaterialGPencilPlacementMode placement_mode = GP_MATERIAL_PLACEMENT_COUNT;
  /* Number of points per segment when placement mode is `GP_MATERIAL_PLACEMENT_COUNT` */
  int placement_count = 0;
  /* Radius factor for points when placement mode is `GP_MATERIAL_PLACEMENT_RADIUS` */
  float placement_radius_spacing = 0;
  /* Point density per unit when placement mode is `GP_MATERIAL_PLACEMENT_DENSITY` */
  float placement_density = 0;

  float random_size_factor = 0;
  float random_strength_factor = 0;
  float random_rotation_factor = 0;

  float random_hue_factor = 0;
  float random_saturation_factor = 0;
  float random_value_factor = 0;

  float random_noise_scale = 0;
  char _pad3[4] = {};
};

struct MaterialLineArt {
  eMaterialLineArtFlags flags = {};

  /* Used to filter line art occlusion edges */
  unsigned char material_mask_bits = 0;

  /** Maximum 255 levels of equivalent occlusion. */
  unsigned char mat_occlusion = 1;

  unsigned char intersection_priority = 0;

  char _pad = {};
};

struct Material {
#ifdef __cplusplus
  DNA_DEFINE_CXX_METHODS(Material)
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_MA;
#endif

  ID id;
  /** Animation data (must be immediately after id for utilities to use it). */
  struct AnimData *adt = nullptr;

  eMaterial_Flag flag = {};
  /** Rendering modes for EEVEE. */
  eMaterial_SurfaceRenderMethod surface_render_method = MA_SURFACE_METHOD_DEFERRED;
  char eevee_domain = MA_EEVEE_DOMAIN_SURFACE;

  /* Colors from Blender Internal that we are still using. */
  float r = 0.8, g = 0.8, b = 0.8, a = 1.0f;
  float specr = 1.0, specg = 1.0, specb = 1.0;
  DNA_DEPRECATED float alpha = 0;
  DNA_DEPRECATED float ray_mirror = 0;
  float spec = 0.5;
  /** Renamed and inversed to roughness. */
  DNA_DEPRECATED float gloss_mir = 0;
  float roughness = 0.4f;
  float metallic = 0;

  /** Nodes */
  DNA_DEPRECATED char use_nodes = 0;

  /** Preview render. */
  ePreviewType pr_type = MA_SPHERE;
  short pr_texture = 0;
  eMaterial_PreviewFlag pr_flag = {};

  /** Index for render passes. */
  short index = 0;

  /* #Material::use_nodes is deprecated so it's not possible to create an embedded node tree from
   * the UI or Python API by setting `use_nodes = True`. Therefore, #nodetree is required to never
   * be nullptr. */
  struct bNodeTree *nodetree = nullptr;
  struct PreviewImage *preview = nullptr;

  /* Freestyle line settings. */
  float line_col[4] = {};
  short line_priority = 0;
  short vcol_alpha = 0;

  /* Texture painting slots. */
  short paint_active_slot = 0;
  short paint_clone_slot = 0;
  short tot_slots = 0;

  /* Displacement. */
  eMaterial_DisplacementMethod displacement_method = MA_DISPLACEMENT_BUMP;

  /* Thickness. */
  eMaterial_ThicknessMode thickness_mode = MA_THICKNESS_SPHERE;

  /* Transparency. */
  float alpha_threshold = 0.5f;
  float refract_depth = 0;
  eMaterial_BlendMethod blend_method =
      MA_BM_SOLID; /* TODO(fclem): Deprecate once we remove legacy EEVEE. */
  eMaterial_BlendShadow blend_shadow =
      MA_BS_SOLID; /* TODO(fclem): Deprecate once we remove legacy EEVEE. */
  eMaterial_BlendFlag blend_flag = MA_BL_TRANSPARENT_SHADOW;

  /* Volume. */
  eMaterial_VolumeIntersectionMethod volume_intersection_method = MA_VOLUME_ISECT_FAST;

  /* Displacement. */
  float inflate_bounds = 0;

  char surface_cull_method = MA_SURFACE_CULL_NONE;
  char depth_offset_affect_lighting = false;
  char ztest_mode = MA_ZTEST_LESS_EQUAL;
  char _pad3[1] = {};

  /**
   * Cached slots for texture painting, must be refreshed via
   * BKE_texpaint_slot_refresh_cache before using.
   */
  struct TexPaintSlot *texpaintslot = nullptr;

  /** Runtime cache for GLSL materials. */
  ListBaseT<LinkData> gpumaterial = {nullptr, nullptr};

  /** Grease pencil color. */
  struct MaterialGPencilStyle *gp_style = nullptr;
  struct MaterialLineArt lineart;

  char stencil_enabled = false;
  char stencil_reference = 0;
  char stencil_read_mask = 15;
  char stencil_write_mask = 15;
  char stencil_test = MA_STENCIL_ALWAYS;
  char stencil_pass_op = MA_STENCIL_OP_KEEP;
  char stencil_fail_op = MA_STENCIL_OP_KEEP;
  char stencil_zfail_op = MA_STENCIL_OP_KEEP;
  short stencil_order = 0;
  char color_write = true;
  char depth_write = true;
  char _pad4[4] = {};
};

#ifdef __cplusplus
inline eMaterialCullMethod material_surface_cull_method_get(const Material &material)
{
  switch (eMaterialCullMethod(material.surface_cull_method)) {
    case MA_SURFACE_CULL_BACK:
    case MA_SURFACE_CULL_FRONT:
      return eMaterialCullMethod(material.surface_cull_method);
    case MA_SURFACE_CULL_NONE:
    default:
      return (material.blend_flag & MA_BL_CULL_BACKFACE) != 0 ? MA_SURFACE_CULL_BACK :
                                                              MA_SURFACE_CULL_NONE;
  }
}

inline eMaterialZTestMode material_ztest_mode_get(const Material &material)
{
  switch (eMaterialZTestMode(material.ztest_mode)) {
    case MA_ZTEST_LESS:
    case MA_ZTEST_GREATER:
    case MA_ZTEST_LESS_EQUAL:
    case MA_ZTEST_GREATER_EQUAL:
    case MA_ZTEST_EQUAL:
    case MA_ZTEST_NOT_EQUAL:
    case MA_ZTEST_ALWAYS:
    case MA_ZTEST_NEVER:
      return eMaterialZTestMode(material.ztest_mode);
    default:
      return MA_ZTEST_LESS_EQUAL;
  }
}

inline bool material_color_write_get(const Material &material)
{
  return material.color_write != 0;
}

inline bool material_depth_write_get(const Material &material)
{
  return material.depth_write != 0;
}
#endif

}  // namespace blender

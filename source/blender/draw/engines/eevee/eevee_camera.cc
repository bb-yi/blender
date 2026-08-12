/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "BKE_screen.hh"
#include "BLI_math_matrix.hh"
#include "BLI_rect.h"

#include "DRW_render.hh"

#include "DNA_camera_types.h"
#include "DNA_view3d_types.h"

#include "RE_engine.h"
#include "RE_pipeline.h"
#include "render_types.h"

#include "eevee_camera.hh"
#include "eevee_instance.hh"

#include <algorithm>
#include <cmath>

namespace blender::eevee {

static bool camera_data_projection_dimensions_valid(const bool is_perspective_camera,
                                                    const float left,
                                                    const float right,
                                                    const float bottom,
                                                    const float top,
                                                    const float near,
                                                    const float far)
{
  if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(bottom) ||
      !std::isfinite(top) || !std::isfinite(near) || !std::isfinite(far))
  {
    return false;
  }

  return !is_perspective_camera || (std::abs(near) > 1.0e-8f);
}

static void camera_data_projection_dimensions_fallback(const bool is_perspective_camera,
                                                       const CameraData &data,
                                                       float &left,
                                                       float &right,
                                                       float &bottom,
                                                       float &top,
                                                       float &near,
                                                       float &far)
{
  near = std::max(std::abs(data.clip_near), 0.01f);
  far = std::max(std::abs(data.clip_far), near + 1.0f);

  const float half_size = is_perspective_camera ? near : 0.5f;
  left = bottom = -half_size;
  right = top = half_size;
}

static void camera_data_update_screen_diagonal(CameraData &data)
{
  float left, right, bottom, top, near, far;
  projmat_dimensions(data.winmat.ptr(), &left, &right, &bottom, &top, &near, &far);

  const bool is_perspective_camera = data.type == CAMERA_PERSP;
  if (!camera_data_projection_dimensions_valid(
          is_perspective_camera, left, right, bottom, top, near, far))
  {
    camera_data_projection_dimensions_fallback(
        is_perspective_camera, data, left, right, bottom, top, near, far);
  }

  const float depth = is_perspective_camera ? -near : 1.0f;
  float2 p0 = float2(left, bottom) / depth;
  float2 p1 = float2(right, top) / depth;
  data.screen_diagonal_length = math::distance(p0, p1);
  if (!std::isfinite(data.screen_diagonal_length) || data.screen_diagonal_length <= 0.0f) {
    data.screen_diagonal_length = 1.0f;
  }
}

bool camera_data_from_object(const Scene *scene,
                             const Object *camera_ob,
                             int2 extent,
                             CameraData &r_data)
{
  if (camera_ob == nullptr || camera_ob->type != OB_CAMERA) {
    return false;
  }

  const blender::Camera *cam = reinterpret_cast<const blender::Camera *>(camera_ob->data);
  CameraParams params;
  BKE_camera_params_init(&params);
  BKE_camera_params_from_object(&params, camera_ob);
  BKE_camera_params_compute_viewplane(
      &params, extent.x, extent.y, scene ? scene->r.xasp : 1.0f, scene ? scene->r.yasp : 1.0f);
  BKE_camera_params_compute_matrix(&params);

  r_data = {};
  switch (cam->type) {
    case CAM_ORTHO:
      r_data.type = CAMERA_ORTHO;
      break;
    case CAM_PERSP:
    default:
      r_data.type = CAMERA_PERSP;
      break;
  }

  r_data.viewinv = camera_ob->object_to_world();
  r_data.viewmat = math::invert(r_data.viewinv);
  r_data.winmat = float4x4(params.winmat);
  r_data.wininv = math::invert(r_data.winmat);
  r_data.persmat = r_data.winmat * r_data.viewmat;
  r_data.persinv = math::invert(r_data.persmat);
  r_data.uv_scale = float2(1.0f);
  r_data.uv_bias = float2(0.0f);
  r_data.equirect_bias = float2(0.0f);
  r_data.equirect_scale = float2(0.0f);
  r_data.equirect_scale_inv = float2(0.0f);
  r_data.fisheye_fov = -1.0f;
  r_data.fisheye_lens = -1.0f;
  r_data.clip_near = cam->clip_start;
  r_data.clip_far = cam->clip_end;
  camera_data_update_screen_diagonal(r_data);
  r_data.initialized = true;
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Camera
 * \{ */

void Camera::init()
{
  const Object *camera_eval = inst_.camera_eval_object;

  CameraData &data = data_;

  if (camera_eval && camera_eval->type == OB_CAMERA) {
    const blender::Camera *cam = reinterpret_cast<const blender::Camera *>(camera_eval->data);
    switch (cam->type) {
      default:
      case CAM_PERSP:
        data.type = CAMERA_PERSP;
        break;
      case CAM_ORTHO:
        data.type = CAMERA_ORTHO;
        break;
#if 0 /* TODO(fclem): Make fisheye properties inside blender. */
      case CAM_PANO: {
        switch (cam->panorama_type) {
          default:
          case CAM_PANO_EQUIRECTANGULAR:
            data.type = CAMERA_PANO_EQUIRECT;
            break;
          case CAM_PANO_FISHEYE_EQUIDISTANT:
            data.type = CAMERA_PANO_EQUIDISTANT;
            break;
          case CAM_PANO_FISHEYE_EQUISOLID:
            data.type = CAMERA_PANO_EQUISOLID;
            break;
          case CAM_PANO_MIRRORBALL:
            data.type = CAMERA_PANO_MIRROR;
            break;
        }
      }
#endif
    }
  }
  else if (inst_.drw_view) {
    data.type = inst_.drw_view->is_persp() ? CAMERA_PERSP : CAMERA_ORTHO;
  }
  else {
    /* Light-probe baking. */
    data.type = CAMERA_PERSP;
  }

  float overscan = 0.0f;
  if ((inst_.scene->eevee.flag & SCE_EEVEE_OVERSCAN) && (inst_.drw_view || inst_.render)) {
    overscan = inst_.scene->eevee.overscan / 100.0f;
    if (inst_.is_custom_matrix()) {
      /* If using a custom matrix (XR and some offscreen render paths)
       * we need to use the v3d winmat as-is. */
      overscan = 0.0f;
    }
  }
  overscan_changed_ = assign_if_different(overscan_, overscan);
  camera_changed_ = assign_if_different(last_camera_object_, inst_.camera_orig_object);
}

float3 Camera::forward_shifted() const
{
  if (is_orthographic()) {
    /* Origin shift: camera shift encoded in asymmetric orthographic winmat.
     * Lateral offset = right * dx + up * dy where dx = -winmat[3][0]/winmat[0][0]. */
    float dx = -data_.winmat[3][0] / data_.winmat[0][0];
    float dy = -data_.winmat[3][1] / data_.winmat[1][1];
    return float3(data_.viewinv[0]) * dx +
           float3(data_.viewinv[1]) * dy;
  }

  /* Perspective: line-of-sight shift (rotate view direction).
   * TODO: horizontal FOV angle a; shift 1 → arctan(2*tan(a/2)). */
  return float3(0.0f);
}

void Camera::sync()
{
  const Object *camera_eval = inst_.camera_eval_object;

  CameraData &data = data_;

  int2 display_extent = inst_.film.display_extent_get();
  int2 film_extent = inst_.film.film_extent_get();
  int2 film_offset = inst_.film.film_offset_get();
  /* Over-scan in film pixel. Not the same as `render_overscan_get`. */
  int film_overscan = Film::overscan_pixels_get(overscan_, film_extent);

  rcti film_rect;
  BLI_rcti_init(&film_rect,
                film_offset.x,
                film_offset.x + film_extent.x,
                film_offset.y,
                film_offset.y + film_extent.y);

  Bounds<float2> uv_region = {float2(0.0f), float2(display_extent)};
  if (inst_.drw_view) {
    float2 uv_scale = float4(inst_.rv3d->viewcamtexcofac).xy();
    float2 uv_bias = float4(inst_.rv3d->viewcamtexcofac).zw();
    /* UV region inside the display extent reference frame. */
    uv_region.min = (-uv_bias * float2(display_extent)) / uv_scale;
    uv_region.max = uv_region.min + (float2(display_extent) / uv_scale);
  }

  data.uv_scale = float2(film_extent + film_overscan * 2) / uv_region.size();
  data.uv_bias = (float2(film_offset - film_overscan) - uv_region.min) / uv_region.size();

  if (inst_.is_baking()) {
    /* Any view so that shadows and light culling works during irradiance bake. */
    draw::View &view = inst_.volume_probes.bake.view_z_;
    data.viewmat = view.viewmat();
    data.viewinv = view.viewinv();
    data.winmat = view.winmat();
    data.type = CAMERA_ORTHO;

    /* \note Follow camera parameters where distances are positive in front of the camera. */
    data.clip_near = -view.far_clip();
    data.clip_far = -view.near_clip();
    data.fisheye_fov = data.fisheye_lens = -1.0f;
    data.equirect_bias = float2(0.0f);
    data.equirect_scale = float2(0.0f);
    data.uv_scale = float2(1.0f);
    data.uv_bias = float2(0.0f);
  }
  else if (inst_.drw_view) {
    data.viewmat = inst_.drw_view->viewmat();
    data.viewinv = inst_.drw_view->viewinv();
    data.winmat = inst_.drw_view->winmat();

    if (inst_.is_custom_matrix()) {
      /* If using a custom matrix (XR and some offscreen render paths)
       * we need to use the v3d winmat as-is. */
    }
    else {
      if (film_offset != int2(0) || film_extent != display_extent) {
        data.winmat = projection_crop_matrix(film_offset, film_extent, display_extent) *
                      data.winmat;
      }

      if (overscan_ != 0.0f) {
        data.winmat = projection_overscan_matrix(film_extent, int2(film_overscan)) * data.winmat;
      }
    }
  }
  else if (inst_.render) {
    const Render *re = inst_.render->re;

    RE_GetCameraWindow(inst_.render->re, camera_eval, data.winmat.ptr());

    RE_GetCameraModelMatrix(re, camera_eval, data.viewinv.ptr());
    data.viewmat = math::invert(data.viewinv);

    rctf viewplane = re->viewplane;
    BKE_camera_params_crop_viewplane(&viewplane, UNPACK2(display_extent), &film_rect);

    RE_GetWindowMatrixWithOverscan(this->is_orthographic(),
                                   re->clip_start,
                                   re->clip_end,
                                   viewplane,
                                   overscan_,
                                   data.winmat.ptr());
  }
  else {
    data.viewmat = float4x4::identity();
    data.viewinv = float4x4::identity();
    data.winmat = math::projection::perspective(-0.1f, 0.1f, -0.1f, 0.1f, 0.1f, 1.0f);
  }

  data.wininv = math::invert(data.winmat);
  data.persmat = data.winmat * data.viewmat;
  data.persinv = math::invert(data.persmat);

  is_camera_object_ = false;
  if (camera_eval && camera_eval->type == OB_CAMERA) {
    const blender::Camera *cam = reinterpret_cast<const blender::Camera *>(camera_eval->data);
    data.clip_near = cam->clip_start;
    data.clip_far = cam->clip_end;
#if 0 /* TODO(fclem): Make fisheye properties inside blender. */
    data.fisheye_fov = cam->fisheye_fov;
    data.fisheye_lens = cam->fisheye_lens;
    data.equirect_bias.x = -cam->longitude_min + M_PI_2;
    data.equirect_bias.y = -cam->latitude_min + M_PI_2;
    data.equirect_scale.x = cam->longitude_min - cam->longitude_max;
    data.equirect_scale.y = cam->latitude_min - cam->latitude_max;
    /* Combine with uv_scale/bias to avoid doing extra computation. */
    data.equirect_bias += data.uv_bias * data.equirect_scale;
    data.equirect_scale *= data.uv_scale;

    data.equirect_scale_inv = 1.0f / data.equirect_scale;
#else
    data.fisheye_fov = data.fisheye_lens = -1.0f;
    data.equirect_bias = float2(0.0f);
    data.equirect_scale = float2(0.0f);
#endif
    is_camera_object_ = true;
  }
  else if (inst_.drw_view) {
    /* \note Follow camera parameters where distances are positive in front of the camera. */
    data.clip_near = -inst_.drw_view->near_clip();
    data.clip_far = -inst_.drw_view->far_clip();
    data.fisheye_fov = data.fisheye_lens = -1.0f;
    data.equirect_bias = float2(0.0f);
    data.equirect_scale = float2(0.0f);
  }

  data.forward_shifted = forward_shifted();

  data_.initialized = true;

  update_bounds();
}

void Camera::override(const CameraData &data, bool is_camera_object)
{
  data_ = data;
  is_camera_object_ = is_camera_object;
  data_.forward_shifted = forward_shifted();
  update_bounds();
}

void Camera::update_bounds()
{
  float left, right, bottom, top, near, far;
  projmat_dimensions(data_.winmat.ptr(), &left, &right, &bottom, &top, &near, &far);

  if (!camera_data_projection_dimensions_valid(
          this->is_perspective(), left, right, bottom, top, near, far))
  {
    camera_data_projection_dimensions_fallback(
        this->is_perspective(), data_, left, right, bottom, top, near, far);
  }

  BoundBox bbox;
  bbox.vec[0][2] = bbox.vec[3][2] = bbox.vec[7][2] = bbox.vec[4][2] = -near;
  bbox.vec[0][0] = bbox.vec[3][0] = left;
  bbox.vec[4][0] = bbox.vec[7][0] = right;
  bbox.vec[0][1] = bbox.vec[4][1] = bottom;
  bbox.vec[7][1] = bbox.vec[3][1] = top;

  /* Get the coordinates of the far plane. */
  if (!this->is_orthographic()) {
    float sca_far = far / near;
    left *= sca_far;
    right *= sca_far;
    bottom *= sca_far;
    top *= sca_far;
  }

  bbox.vec[1][2] = bbox.vec[2][2] = bbox.vec[6][2] = bbox.vec[5][2] = -far;
  bbox.vec[1][0] = bbox.vec[2][0] = left;
  bbox.vec[6][0] = bbox.vec[5][0] = right;
  bbox.vec[1][1] = bbox.vec[5][1] = bottom;
  bbox.vec[2][1] = bbox.vec[6][1] = top;

  bound_sphere.center = {0.0f, 0.0f, 0.0f};
  bound_sphere.radius = 0.0f;

  for (auto i : IndexRange(8)) {
    bound_sphere.center += float3(bbox.vec[i]);
  }
  bound_sphere.center /= 8.0f;
  for (auto i : IndexRange(8)) {
    float dist_sqr = math::distance_squared(bound_sphere.center, float3(bbox.vec[i]));
    bound_sphere.radius = max_ff(bound_sphere.radius, dist_sqr);
  }
  bound_sphere.radius = sqrtf(bound_sphere.radius);

  /* Transform into world space. */
  bound_sphere.center = math::transform_point(data_.viewinv, bound_sphere.center);

  /* Compute diagonal length. */
  float2 p0 = float2(bbox.vec[0]) / (this->is_perspective() ? bbox.vec[0][2] : 1.0f);
  float2 p1 = float2(bbox.vec[7]) / (this->is_perspective() ? bbox.vec[7][2] : 1.0f);
  data_.screen_diagonal_length = math::distance(p0, p1);
  if (!std::isfinite(data_.screen_diagonal_length) || data_.screen_diagonal_length <= 0.0f) {
    data_.screen_diagonal_length = 1.0f;
  }
}

float4x4 Camera::projection_crop_matrix(int2 film_offset, int2 film_extent, int2 display_extent)
{
  float2 uv_min = float2(film_offset) / float2(display_extent);
  float2 uv_max = float2(film_offset + film_extent) / float2(display_extent);

  float2 ndc_min = uv_min * 2.0f - 1.0f;
  float2 ndc_max = uv_max * 2.0f - 1.0f;

  float2 ndc_size = ndc_max - ndc_min;
  float2 ndc_center = (ndc_min + ndc_max) * 0.5f;

  float2 scale = 2.0f / ndc_size;
  float2 offset = -ndc_center * scale;

  float4x4 crop_matrix = float4x4::identity();
  crop_matrix[0][0] = scale.x;
  crop_matrix[1][1] = scale.y;
  crop_matrix[3][0] = offset.x;
  crop_matrix[3][1] = offset.y;

  return crop_matrix;
}

float4x4 Camera::projection_overscan_matrix(int2 film_extent, int2 film_overscan)
{
  float2 overscan_scale = float2(film_extent) / float2(film_extent + film_overscan * 2);

  float4x4 overscan_matrix = float4x4::identity();
  overscan_matrix[0][0] = overscan_scale.x;
  overscan_matrix[1][1] = overscan_scale.y;

  return overscan_matrix;
}

/** \} */

}  // namespace blender::eevee

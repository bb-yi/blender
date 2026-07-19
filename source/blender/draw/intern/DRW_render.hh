/* SPDX-FileCopyrightText: 2016 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

/* This is the Render Functions used by Realtime engines to draw with OpenGL */

#pragma once

#include <cstdint>
#include <functional>

#include "BKE_mesh_wrapper.hh"
#include "BKE_subdiv_modifier.hh"
#include "BLI_math_vector_types.hh"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"

#include "GPU_material.hh"

namespace blender {

namespace gpu {
class Batch;
class Shader;
class Texture;
class UniformBuf;
class FrameBuffer;
}  // namespace gpu
struct ARegion;
struct bContext;
struct DefaultFramebufferList;
struct DefaultTextureList;
struct Depsgraph;
struct DupliObject;
struct GPUMaterial;
struct Mesh;
struct Object;
struct ParticleSystem;
struct rcti;
struct RegionView3D;
struct RenderEngine;
struct RenderEngineType;
struct RenderLayer;
struct RenderResult;
struct SpaceLink;
struct TaskGraph;
struct View3D;
struct ViewLayer;
struct DRWContext;
struct World;
struct DRWData;
struct DRWViewData;
struct GPUViewport;
struct DRWTextStore;
struct GPUViewport;
namespace draw {
class TextureFromPool;
class ObjectRef;
class Manager;
}  // namespace draw

/* TODO: Put it somewhere else? */
struct BoundSphere {
  float center[3], radius;
};

struct DrawPerformanceMetrics {
  bool valid = false;

  bool has_last_evaluation = false;
  double last_evaluation_ms = 0.0;
  /* Monotonic depsgraph evaluation serial, not a count of updated IDs. */
  uint64_t depsgraph_eval_serial = 0;

  double sync_total_ms = 0.0;
  double sync_engine_setup_ms = 0.0;
  double sync_engine_init_ms = 0.0;
  double sync_manager_begin_ms = 0.0;
  double sync_engine_begin_ms = 0.0;
  double sync_modules_begin_ms = 0.0;
  double sync_object_iteration_ms = 0.0;
  double sync_dupli_extraction_ms = 0.0;
  double sync_delayed_extraction_ms = 0.0;
  double sync_extraction_wait_ms = 0.0;
  double sync_curves_update_ms = 0.0;
  double sync_engine_end_ms = 0.0;
  double sync_manager_end_ms = 0.0;

  double submission_total_ms = 0.0;
  double submission_framebuffer_ms = 0.0;
  double submission_callbacks_pre_ms = 0.0;
  double submission_engine_draw_ms = 0.0;
  double submission_callbacks_post_ms = 0.0;
  double submission_framebuffer_restore_ms = 0.0;
};

struct DrawEngine {
  static constexpr int GPU_INFO_SIZE = 512; /* IMA_MAX_RENDER_TEXT_SIZE */

  char info[GPU_INFO_SIZE] = {'\0'};

  bool used = false;

  virtual ~DrawEngine() = default;

  virtual StringRefNull name_get() = 0;

  virtual bool performance_capture_requested(const DRWContext & /*draw_ctx*/) const
  {
    return false;
  }

  /* Functions called for viewport. */

  /** Init engine. Run first and for every redraw. */
  virtual void init() = 0;
  /** Scene synchronization. Command buffers building. */
  virtual void begin_sync() = 0;
  virtual void object_sync(draw::ObjectRef &ob_ref, draw::Manager &manager) = 0;
  virtual void end_sync() = 0;
  /** Command Submission. */
  virtual void draw(draw::Manager &manager) = 0;
  /**
   * Optional callback after the complete shared viewport draw cycle.
   * The metrics reference is transient and must be copied if the engine keeps it.
   */
  virtual void performance_frame_end(const DrawPerformanceMetrics & /*metrics*/) {}

  /* Called when closing blender.
   * Cleanup all lazily initialized static members that have GPU resources.
   * Implemented on a case by case basis and called directly. */
  //  static void exit(){};

  struct Pointer {
    DrawEngine *instance = nullptr;

    ~Pointer()
    {
      free_instance();
    }

    void free_instance()
    {
      delete instance;
      instance = nullptr;
    }

    void set_used(bool used)
    {
      if (used) {
        if (instance == nullptr) {
          instance = create_instance();
        }
        instance->used = true;
      }
      else if (instance) {
        instance->used = false;
      }
    }

    virtual DrawEngine *create_instance() = 0;
  };
};

/* Viewport. */

/**
 * Returns a TextureFromPool stored in the given view data for the pass identified by the given
 * pass name. Engines should call this function for each of the passes needed by the viewport
 * compositor in every redraw, then it should allocate the texture and write the pass data to it.
 * The texture should cover the entire viewport.
 */
draw::TextureFromPool &DRW_viewport_pass_texture_get(const char *pass_name);

/**
 * Returns true if the pass with the given name exists.
 */
bool DRW_viewport_pass_texture_exists(const char *pass_name);

void DRW_viewport_request_redraw();

void DRW_render_to_image(
    RenderEngine *engine,
    Depsgraph *depsgraph,
    ::std::function<void(RenderEngine *, RenderLayer *, const rcti)> render_view_cb,
    ::std::function<void(RenderResult *)> store_metadata_cb);

void DRW_render_object_iter(RenderEngine *engine,
                            Depsgraph *depsgraph,
                            ::std::function<void(draw::ObjectRef &, RenderEngine *, Depsgraph *)>);

void DRW_render_set_time(RenderEngine *engine, Depsgraph *depsgraph, int frame, float subframe);

/**
 * Assume a valid GL context is bound (and that the gl_context_mutex has been acquired).
 * This function only setup DST and execute the given function.
 * \warning similar to DRW_render_to_image you cannot use default lists (`dfbl` & `dtxl`).
 */
void DRW_custom_pipeline_begin(DRWContext &draw_ctx, Depsgraph *depsgraph);
void DRW_custom_pipeline_end(DRWContext &draw_ctx);

/**
 * Used when the render engine want to redo another cache populate inside the same render frame.
 * Assumes it is called between `DRW_custom_pipeline_begin/end()`.
 */
void DRW_cache_restart();

/* Settings. */

bool DRW_object_is_renderable(const Object *ob);
/**
 * Does `ob` needs to be rendered in edit mode.
 *
 * When using duplicate linked meshes, objects that are not in edit-mode will be drawn as
 * it is in edit mode, when another object with the same mesh is in edit mode.
 * This will not be the case when one of the objects are influenced by modifiers.
 */
bool DRW_object_is_in_edit_mode(const Object *ob);
/**
 * Return whether this object is visible depending if
 * we are rendering or drawing in the viewport.
 */
int DRW_object_visibility_in_active_context(const Object *ob);
bool DRW_object_use_hide_faces(const Object *ob);

bool DRW_object_is_visible_psys_in_active_context(const Object *object,
                                                  const ParticleSystem *psys);

/**
 * Convenient accessor for object data, that also automatically returns
 * the base or tessellated mesh depending if GPU subdivision is enabled.
 */
template<typename T> T &DRW_object_get_data_for_drawing(const Object &object)
{
  return *id_cast<T *>(object.data);
}

inline Mesh &DRW_mesh_get_for_drawing(Mesh &mesh)
{
  /* For drawing we want either the base mesh if GPU subdivision is enabled, or the
   * tessellated mesh if GPU subdivision is disabled. */
  if (BKE_subsurf_modifier_has_gpu_subdiv(&mesh)) {
    return mesh;
  }
  return *BKE_mesh_wrapper_ensure_subdivision(&mesh);
}

template<> inline Mesh &DRW_object_get_data_for_drawing(const Object &object)
{
  BLI_assert(object.type == OB_MESH);
  return DRW_mesh_get_for_drawing(*id_cast<Mesh *>(object.data));
}

/**
 * Same as DRW_object_get_data_for_drawing, but for the editmesh cage,
 * if it exists.
 */
const Mesh *DRW_object_get_editmesh_cage_for_drawing(const Object &object);

/* Draw State. */

/* -------------------------------------------------------------------- */
/** \name Draw Context
 * \{ */

struct DRWContext {
 private:
  /** Render State: No persistent data between draw calls. */
  static thread_local DRWContext *g_context;
  /** Timings recorded for performance overlay. */
  float last_sync_time_;
  float last_submission_time_;
  DrawPerformanceMetrics performance_metrics_;
  double performance_sync_start_time_ = 0.0;

  /* TODO(fclem): Private? */
 public:
  /* TODO: clean up this struct a bit. */
  /** Cache generation */
  DRWData *data = nullptr;
  /** Active view data structure for one of the 2 stereo view. */
  DRWViewData *view_data_active = nullptr;

  /** Optional associated viewport. Can be nullptr. */
  GPUViewport *viewport = nullptr;
  /** Size of the viewport or the final render frame. */
  float2 size = {0, 0};
  float2 inv_size = {0, 0};

  /** Returns the viewport's default frame-buffer. */
  gpu::FrameBuffer *default_framebuffer();
  /** Returns the viewport's default frame-buffer list. Not all of them might be available. */
  DefaultFramebufferList *viewport_framebuffer_list_get() const;
  /** Returns the viewport's default texture list. Not all of them might be available. */
  DefaultTextureList *viewport_texture_list_get() const;

  const enum Mode {
    /** Render for display of 2D or 3D area. Runs on main thread. */
    VIEWPORT = 0,

    /* These viewport modes will render without some overlays (i.e. no text). */

    /** Render for a 3D viewport in XR. Runs on main thread. */
    VIEWPORT_XR,
    /** Render for a 3D viewport offscreen render (python). Runs on main thread. */
    VIEWPORT_OFFSCREEN,
    /**
     * Render for a 3D viewport image render (Render Viewport Preview, also VSE scene strips).
     * Runs on main thread.
     */
    VIEWPORT_RENDER,

    /** Render for object mode selection. Runs on main thread. */
    SELECT_OBJECT,
    /** Render for object material selection. Runs on main thread. */
    SELECT_OBJECT_MATERIAL,
    /** Render for edit mesh selection. Runs on main thread. */
    SELECT_EDIT_MESH,

    /** Render for depth picking (auto-depth). Runs on main thread. */
    DEPTH,
    DEPTH_ACTIVE_OBJECT,

    /** Render for F12 final render. Can run in any thread. */
    RENDER,
    /** Used by custom pipeline. Can run in any thread. */
    CUSTOM,
  } mode;

  struct {
    bool draw_background = false;
  } options;

  /** Convenience pointer to text_store owned by the viewport */
  DRWTextStore **text_store_p = nullptr;

  /** Contains list of objects that needs to be extracted from other objects. */
  Set<Object *> delayed_extraction;

  /* TODO(fclem): Public. */

  /** Current rendering context. Avoid too many lookups while drawing. */

  /** Evaluated Depsgraph. */
  Depsgraph *depsgraph = nullptr;
  /** Evaluated Scene. */
  Scene *scene = nullptr;
  /** Evaluated ViewLayer. */
  ViewLayer *view_layer = nullptr;

  /** Last resort (some functions take this as an arg so we can't easily avoid).
   * May be nullptr when used for selection or depth buffer. */
  const bContext *evil_C = nullptr;
  /** Can be nullptr depending on context. */
  ARegion *region = nullptr;
  /** Can be nullptr depending on context. */
  SpaceLink *space_data = nullptr;
  /** Can be nullptr depending on context. */
  RegionView3D *rv3d = nullptr;
  /** Can be nullptr depending on context. */
  View3D *v3d = nullptr;
  /** Use 'object_edit' for edit-mode */
  Object *obact = nullptr;
  Object *object_pose = nullptr;
  Object *object_edit = nullptr;

  eObjectMode object_mode = OB_MODE_OBJECT;

 public:
  /**
   * If `viewport` is not specified, `DRWData` will be considered temporary and discarded on exit.
   * If `C` is nullptr, it means that the context is **not** associated with any UI or operator.
   * If `region` is nullptr, it will be sourced from the context `C` or left as nullptr otherwise.
   * If `v3d` is nullptr, it will be sourced from the context `C` or left as nullptr otherwise.
   */
  DRWContext(Mode mode,
             Depsgraph *depsgraph,
             GPUViewport *viewport,
             const bContext *C = nullptr,
             ARegion *region = nullptr,
             View3D *v3d = nullptr);
  DRWContext(Mode mode,
             Depsgraph *depsgraph,
             const int2 size = {1, 1},
             const bContext *C = nullptr,
             ARegion *region = nullptr,
             View3D *v3d = nullptr);

  ~DRWContext();

  /**
   * Acquire `data` and `view_data_active`.
   * Needs to be called before enabling any draw engine.
   * IMPORTANT: This can be called multiple times before release_data.
   * IMPORTANT: This must be called with an active GPUContext.
   */
  void acquire_data();

  /**
   * Make sure to release acquired DRWData. If created on the fly, make sure to destroy them.
   * IMPORTANT: This needs to be called with the same active GPUContext `acquire_data()` was called
   * with.
   */
  void release_data();

  /**
   * Enable engines from context. Not needed for Mode::RENDER and Mode::CUSTOM.
   *
   * `render_engine_type` specify the engine to use in OB_MATERIAL or OB_RENDER modes.
   * `gpencil_engine_needed` should be set to true if the grease pencil engine is needed.
   */
  void enable_engines(bool gpencil_engine_needed = false,
                      RenderEngineType *render_engine_type = nullptr);

  /** Free unused engine data. */
  void engines_data_validate();

  using iter_callback_t =
      ::std::function<void(struct DupliCacheManager &, struct ExtractionGraph &)>;

  /** Run the sync phase with data extraction. iter_callback defines which object to sync. */
  void sync(iter_callback_t iter_callback);
  /** Run enabled engine init and sync callbacks. iter_callback defines which object to sync. */
  void engines_init_and_sync(iter_callback_t iter_callback);
  /** Run enabled engine init and draw scene callbacks. */
  void engines_draw_scene();
  /** Start the shared performance root timer for a regular 3D viewport draw. */
  void performance_sync_begin(double sync_start_time);
  /** Enable detailed capture after the draw engines have been selected and validated. */
  void performance_capture_enable();
  void performance_sync_end();
  void performance_submission_end(double total_ms,
                                  double framebuffer_ms,
                                  double callbacks_pre_ms,
                                  double engine_draw_ms,
                                  double callbacks_post_ms,
                                  double framebuffer_restore_ms);

  /** True when the detailed shared timing phases are being collected for this draw. */
  bool performance_capture_valid() const
  {
    return performance_metrics_.valid;
  }

  static DRWContext &get_active()
  {
    return *g_context;
  }

  float2 viewport_size_get() const
  {
    return size;
  }

  /** Return true if any #DRWContext is active on this thread. */
  static bool is_active()
  {
    return g_context != nullptr;
  }

  bool is_select() const
  {
    return ELEM(mode, SELECT_OBJECT, SELECT_OBJECT_MATERIAL, SELECT_EDIT_MESH);
  }
  bool is_material_select() const
  {
    return ELEM(mode, SELECT_OBJECT_MATERIAL);
  }
  bool is_depth() const
  {
    return ELEM(mode, DEPTH, DEPTH_ACTIVE_OBJECT);
  }
  bool is_image_render() const
  {
    return ELEM(mode, VIEWPORT_RENDER, RENDER);
  }
  bool is_scene_render() const
  {
    return ELEM(mode, RENDER);
  }
  bool is_viewport_image_render() const
  {
    return ELEM(mode, VIEWPORT_RENDER);
  }
  bool is_viewport_xr() const
  {
    return ELEM(mode, VIEWPORT_XR);
  }
  float last_sync_time() const
  {
    return last_sync_time_;
  }
  float last_submission_time() const
  {
    return last_submission_time_;
  }

  /** True if current viewport is drawn during playback. */
  bool is_playback() const;
  /** True if current viewport is drawn during navigation operator. */
  bool is_navigating() const;
  /** True if current viewport is drawn during painting operator. */
  bool is_painting() const;
  /** True if current viewport is drawn during transforming operator. */
  bool is_transforming() const;
  /** True if viewport compositor is enabled when drawing with this context. */
  bool is_viewport_compositor_enabled() const;
};

/** \} */

const DRWContext *DRW_context_get();

}  // namespace blender

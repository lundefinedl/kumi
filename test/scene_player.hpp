#pragma once

#include "../effect.hpp"
#include "../property_manager.hpp"
#include "../camera.hpp"
#include "../gaussian_blur.hpp"

struct Scene;
class DeferredContext;

class ScenePlayer : public Effect {
public:

  ScenePlayer(const std::string &name);
  ~ScenePlayer();
  virtual bool init() override;
  virtual bool update(int64 global_time, int64 local_time, int64 delta, bool paused, int64 frequency, int32 num_ticks, float ticks_fraction) override;
  virtual bool render() override;
  virtual bool close() override;
private:

  virtual void fill_cbuffer(CBuffer *cbuffer) const;

  virtual void update_from_json(const JsonValue::JsonValuePtr &state) override;
  bool file_changed(const char *filename, void *token);

  void calc_camera_matrices(double time, double delta, XMFLOAT4X4 *view, XMFLOAT4X4 *proj);

  void post_process(GraphicsObjectHandle input, GraphicsObjectHandle output, GraphicsObjectHandle technique);

  PropertyId _light_pos_id, _light_color_id, _light_att_start_id, _light_att_end_id;
  PropertyId _view_mtx_id, _proj_mtx_id;

  Scene *_scene;

  GraphicsObjectHandle _default_shader;
  GraphicsObjectHandle _ssao_fill;
  GraphicsObjectHandle _ssao_compute;
  GraphicsObjectHandle _ssao_blur;
  GraphicsObjectHandle _ssao_ambient;
  GraphicsObjectHandle _ssao_light;
  GraphicsObjectHandle _gamma_correct;
  GraphicsObjectHandle _scale, _scale_cutoff;
  GraphicsObjectHandle _blur_horiz, _blur_vert;

  GraphicsObjectHandle _luminance_map;

  GraphicsObjectHandle _blur_sbuffer;
  GraphicsObjectHandle _rt_final;

  GaussianBlur _blur;

  PropertyId _kernel_id, _noise_id, _ambient_id;

  XMFLOAT4X4 _view, _proj;

  DeferredContext *_ctx;

  float _blurX, _blurY;

  PropertyId _screenSizeId;
  PropertyId _DofSettingsId;
  float _nearFocusStart, _nearFocusEnd;
  float _farFocusStart, _farFocusEnd;
};

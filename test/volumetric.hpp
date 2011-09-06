#pragma once

#include "../demo_engine.hpp"

struct Scene;

class VolumetricEffect : public Effect {
public:

	VolumetricEffect(GraphicsObjectHandle context, const std::string &name) : Effect(context, name) {}
	virtual bool init() override;
	virtual bool update(int64 global_time, int64 local_time, int64 frequency, int32 num_ticks, float ticks_fraction) override;
	virtual bool render() override;
	virtual bool close() override;
private:
	GraphicsObjectHandle _render_target;
	Scene *_scene;
};
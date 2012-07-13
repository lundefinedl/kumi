#include "stdafx.h"
#include "graphics.hpp"
#include "logger.hpp"
#include "mesh.hpp"
#include "deferred_context.hpp"
#include "material_manager.hpp"

using namespace std;

DeferredContext::DeferredContext() : _ctx(nullptr) {
}


void DeferredContext::render_technique(GraphicsObjectHandle technique_handle, 
                                       const std::array<GraphicsObjectHandle, MAX_TEXTURES> &resources,
                                       const InstanceData &instance_data) {
  Technique *technique = GRAPHICS._techniques.get(technique_handle);
  //const TechniqueRenderData *rd = (const TechniqueRenderData *)data;

  Shader *vs = technique->vertex_shader(0);
  Shader *ps = technique->pixel_shader(0);
  set_vs(vs->handle());
  set_ps(ps->handle());

  set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  set_vb(technique->vb(), technique->vertex_size());
  set_layout(technique->input_layout());
  set_ib(technique->ib(), technique->index_format());

  set_rs(technique->rasterizer_state());
  set_dss(technique->depth_stencil_state(), GRAPHICS.default_stencil_ref());
  set_bs(technique->blend_state(), GRAPHICS.default_blend_factors(), GRAPHICS.default_sample_mask());

  // set samplers
  auto &samplers = ps->samplers();
  if (!samplers.empty()) {
    set_samplers(ps->samplers());
  }

  // set resource views
  auto &rv = ps->resource_views();
  if (rv.count > 0) {
    array<GraphicsObjectHandle, 8> rr;
    fill_system_resource_views(rv, &rr);
    for (int i = 0; i < MAX_TEXTURES; ++i) {
      if (resources[i].is_valid())
        rr[i] = resources[i];
    }
    set_shader_resources(rr);
  }

  // set cbuffers
  auto &vs_cbuffers = vs->cbuffers();
  for (size_t i = 0; i < vs_cbuffers.size(); ++i) {
    technique->fill_cbuffer(&vs_cbuffers[i]);
  }

  // Check if we're drawing instanced
  if (instance_data.num_instances) {
    for (int ii = 0; ii < instance_data.num_instances; ++ii) {

      auto &ps_cbuffers = ps->cbuffers();
      for (size_t i = 0; i < ps_cbuffers.size(); ++i) {
        technique->fill_cbuffer(&ps_cbuffers[i]);

        for (size_t j = 0; j < ps_cbuffers[i].instance_vars.size(); ++j) {
          auto &var = ps_cbuffers[i].instance_vars[j];

          for (size_t k = 0; k < instance_data.vars.size(); ++k) {
            auto &cur = instance_data.vars[k];
            if (cur.id == var.id) {
              const void *data = &instance_data.payload[cur.ofs + ii * instance_data.block_size];
              memcpy(&ps_cbuffers[i].staging[var.ofs], data, cur.len);
              break;
            }
          }
        }
      }
      set_cbuffer(vs_cbuffers, ps_cbuffers);
      draw_indexed(technique->index_count(), 0, 0);
    }

  } else {

    auto &ps_cbuffers = ps->cbuffers();
    for (size_t i = 0; i < ps_cbuffers.size(); ++i) {
      technique->fill_cbuffer(&ps_cbuffers[i]);
    }
    set_cbuffer(vs_cbuffers, ps_cbuffers);
    draw_indexed(technique->index_count(), 0, 0);
  }

  if (rv.count > 0)
    unset_shader_resource(rv.first, rv.count);
}

void DeferredContext::fill_system_resource_views(const SparseUnknown &props, std::array<GraphicsObjectHandle, MAX_TEXTURES> *out) const {

}

void DeferredContext::render_mesh(Mesh *mesh, GraphicsObjectHandle technique_handle) {

  Technique *technique = GRAPHICS._techniques.get(technique_handle);

  set_layout(technique->input_layout());
  set_rs(technique->rasterizer_state());
  set_dss(technique->depth_stencil_state(), GRAPHICS.default_stencil_ref());
  set_bs(technique->blend_state(), GRAPHICS.default_blend_factors(), GRAPHICS.default_sample_mask());

  for (size_t i = 0; i < mesh->submeshes.size(); ++i) {
    SubMesh *submesh = mesh->submeshes[i];

    const MeshGeometry *geometry = &submesh->geometry;
    const Material *material = MATERIAL_MANAGER.get_material(submesh->material_id);

    // get the shader for the current technique, based on the flags used by the material
    int flags = material->flags();
    Shader *vs = technique->vertex_shader(flags);
    Shader *ps = technique->pixel_shader(flags);

    set_vs(vs->handle());
    set_ps(ps->handle());

    set_vb(geometry->vb, geometry->vertex_size);
    set_ib(geometry->ib, geometry->index_format);
    set_topology(geometry->topology);

    // set cbuffers
    auto &vs_cbuffers = vs->cbuffers();
    for (size_t i = 0; i < vs_cbuffers.size(); ++i) {
      mesh->fill_cbuffer(&vs_cbuffers[i]);
      material->fill_cbuffer(&vs_cbuffers[i]);
      technique->fill_cbuffer(&vs_cbuffers[i]);
    }

    auto &ps_cbuffers = ps->cbuffers();
    for (size_t i = 0; i < ps_cbuffers.size(); ++i) {
      mesh->fill_cbuffer(&ps_cbuffers[i]);
      material->fill_cbuffer(&ps_cbuffers[i]);
      technique->fill_cbuffer(&ps_cbuffers[i]);
    }
    set_cbuffer(vs_cbuffers, ps_cbuffers);

    // set samplers
    auto &samplers = ps->samplers();
    if (!samplers.empty()) {
      set_samplers(samplers);
    }

    // set resource views
    auto &rv = ps->resource_views();
    if (rv.count > 0) {
      array<GraphicsObjectHandle, MAX_TEXTURES> rr;
      fill_system_resource_views(rv, &rr);
      material->fill_resource_views(rv, &rr);
      set_shader_resources(rr);
    }

    draw_indexed(geometry->index_count, 0, 0);

    if (rv.count > 0)
      unset_shader_resource(rv.first, rv.count);
  }

}

void DeferredContext::generate_mips(GraphicsObjectHandle h) {
  auto rt = GRAPHICS._render_targets.get(h);
  _ctx->GenerateMips(rt->srv.resource);
}

void DeferredContext::set_render_targets(GraphicsObjectHandle *render_targets, bool *clear_targets, int num_render_targets) {

  ID3D11RenderTargetView *rts[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
  ID3D11DepthStencilView *dsv = nullptr;
  D3D11_TEXTURE2D_DESC texture_desc;
  float color[4] = {0,0,0,0};
  // Collect the valid render targets, set the first available depth buffer
  // and clear targets if specified
  for (int i = 0; num_render_targets < 8; ++i) {
    GraphicsObjectHandle h = render_targets[i];
    KASSERT(h.is_valid());
    auto rt = GRAPHICS._render_targets.get(h);
    texture_desc = rt->texture.desc;
    if (!dsv && rt->dsv.resource) {
      dsv = rt->dsv.resource;
    }
    rts[i] = rt->rtv.resource;
    // clear render target (and depth stenci)
    if (clear_targets[i]) {
      _ctx->ClearRenderTargetView(rts[i], color);
      if (rt->dsv.resource) {
        _ctx->ClearDepthStencilView(rt->dsv.resource, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0 );
      }
    }
  }
  CD3D11_VIEWPORT viewport(0.0f, 0.0f, (float)texture_desc.Width, (float)texture_desc.Height);
  _ctx->RSSetViewports(1, &viewport);
  _ctx->OMSetRenderTargets(num_render_targets, rts, dsv);
}

void DeferredContext::set_default_render_target() {
  auto rt = GRAPHICS._render_targets.get(GRAPHICS._default_render_target);
  _ctx->OMSetRenderTargets(1, &rt->rtv.resource.p, rt->dsv.resource);
  _ctx->RSSetViewports(1, &GRAPHICS._viewport);

}

void DeferredContext::set_vs(GraphicsObjectHandle vs) {
  if (prev_vs != vs) {
    _ctx->VSSetShader(GRAPHICS._vertex_shaders.get(vs), NULL, 0);
    prev_vs = vs;
  }
}

void DeferredContext::set_ps(GraphicsObjectHandle ps) {
  if (prev_ps != ps) {
    _ctx->PSSetShader(GRAPHICS._pixel_shaders.get(ps), NULL, 0);
    prev_ps = ps;
  }
}

void DeferredContext::set_layout(GraphicsObjectHandle layout) {
  if (prev_layout != layout) {
    _ctx->IASetInputLayout(GRAPHICS._input_layouts.get(layout));
    prev_layout = layout;
  }
}

void DeferredContext::set_vb(ID3D11Buffer *buf, uint32_t stride)
{
  UINT ofs[] = { 0 };
  ID3D11Buffer* bufs[] = { buf };
  uint32_t strides[] = { stride };
  _ctx->IASetVertexBuffers(0, 1, bufs, strides, ofs);
}

void DeferredContext::set_vb(GraphicsObjectHandle vb, int vertex_size) {
  if (prev_vb != vb) {
    set_vb(GRAPHICS._vertex_buffers.get(vb), vertex_size);
    prev_vb = vb;
  }
}

void DeferredContext::set_ib(GraphicsObjectHandle ib, DXGI_FORMAT format) {
  if (prev_ib != ib) {
    _ctx->IASetIndexBuffer(GRAPHICS._index_buffers.get(ib), format, 0);
    prev_ib = ib;
  }
}

void DeferredContext::set_topology(D3D11_PRIMITIVE_TOPOLOGY top) {
  if (prev_topology != top) {
    _ctx->IASetPrimitiveTopology(top);
    prev_topology = top;
  }
}

void DeferredContext::set_rs(GraphicsObjectHandle rs) {
  if (prev_rs != rs) {
    _ctx->RSSetState(GRAPHICS._rasterizer_states.get(rs));
    prev_rs = rs;
  }
}

void DeferredContext::set_dss(GraphicsObjectHandle dss, UINT stencil_ref) {
  if (prev_dss != dss) {
    _ctx->OMSetDepthStencilState(GRAPHICS._depth_stencil_states.get(dss), stencil_ref);
    prev_dss = dss;
  }
}

void DeferredContext::set_bs(GraphicsObjectHandle bs, const float *blend_factors, UINT sample_mask) {
  if (prev_bs != bs) {
    _ctx->OMSetBlendState(GRAPHICS._blend_states.get(bs), blend_factors, sample_mask);
    prev_bs = bs;
  }
}

void DeferredContext::set_samplers(const std::array<GraphicsObjectHandle, MAX_SAMPLERS> &samplers) {
  int size = samplers.size() * sizeof(GraphicsObjectHandle);
  if (memcmp(samplers.data(), prev_samplers, size)) {
    int first_sampler = MAX_SAMPLERS, num_samplers = 0;
    ID3D11SamplerState *d3dsamplers[MAX_SAMPLERS];
    for (int i = 0; i < MAX_SAMPLERS; ++i) {
      if (samplers[i].is_valid()) {
        d3dsamplers[i] = GRAPHICS._sampler_states.get(samplers[i]);
        first_sampler = min(first_sampler, i);
        num_samplers++;
      }
    }

    if (num_samplers) {
      _ctx->PSSetSamplers(first_sampler, num_samplers, &d3dsamplers[first_sampler]);
      memcpy(prev_samplers, samplers.data(), size);
    }
  }
}

void DeferredContext::set_shader_resources(const std::array<GraphicsObjectHandle, MAX_TEXTURES> &resources) {
  int size = resources.size() * sizeof(GraphicsObjectHandle);
  // force setting the views because we always unset them..
  bool force = true;
  if (force || memcmp(resources.data(), prev_resources, size)) {
    ID3D11ShaderResourceView *d3dresources[MAX_TEXTURES];
    int first_resource = MAX_TEXTURES, num_resources = 0;
    for (int i = 0; i < MAX_TEXTURES; ++i) {
      if (resources[i].is_valid()) {
        GraphicsObjectHandle h = resources[i];
        if (h.type() == GraphicsObjectHandle::kTexture) {
          auto *data = GRAPHICS._textures.get(h);
          d3dresources[i] = data->view.resource;
        } else if (h.type() == GraphicsObjectHandle::kResource) {
          auto *data = GRAPHICS._resources.get(h);
          d3dresources[i] = data->view.resource;
        } else if (h.type() == GraphicsObjectHandle::kRenderTarget) {
          auto *data = GRAPHICS._render_targets.get(h);
          d3dresources[i] = data->srv.resource;
        } else {
          LOG_ERROR_LN("Trying to set invalid resource type!");
        }
        num_resources++;
        first_resource = min(first_resource, i);
      }
    }

    if (num_resources) {
      int ofs = first_resource;
      int num_resources_set = 0;
      while (ofs < MAX_TEXTURES && num_resources_set < num_resources) {
        // handle non sequential resources
        int cur = 0;
        int tmp = ofs;
        while (resources[ofs].is_valid()) {
          ofs++;
          cur++;
          if (ofs == MAX_TEXTURES)
            break;
        }
        _ctx->PSSetShaderResources(tmp, cur, &d3dresources[tmp]);
        num_resources_set += cur;
        while (ofs < MAX_TEXTURES && !resources[ofs].is_valid())
          ofs++;
      }
      memcpy(prev_resources, resources.data(), size);
    }
  }
}

void DeferredContext::unset_shader_resource(int first_view, int num_views) {
  if (!num_views)
    return;
  static ID3D11ShaderResourceView *null_views[MAX_SAMPLERS] = {0, 0, 0, 0, 0, 0, 0, 0};
  _ctx->PSSetShaderResources(first_view, num_views, null_views);
}

void DeferredContext::set_cbuffer(const vector<CBuffer> &vs, const vector<CBuffer> &ps) {

  ID3D11Buffer **vs_cb = (ID3D11Buffer **)_alloca(vs.size() * sizeof(ID3D11Buffer *));
  ID3D11Buffer **ps_cb = (ID3D11Buffer **)_alloca(ps.size() * sizeof(ID3D11Buffer *));

  // Copy the vs cbuffers
  for (size_t i = 0; i < vs.size(); ++i) {
    auto &cur = vs[i];
    ID3D11Buffer *buffer = GRAPHICS._constant_buffers.get(cur.handle);
    D3D11_MAPPED_SUBRESOURCE sub;
    _ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &sub);
    memcpy(sub.pData, cur.staging.data(), cur.staging.size());
    _ctx->Unmap(buffer, 0);
    vs_cb[i] = buffer;
  }

  // Copy the ps cbuffers
  for (size_t i = 0; i < ps.size(); ++i) {
    auto &cur = ps[i];
    ID3D11Buffer *buffer = GRAPHICS._constant_buffers.get(cur.handle);
    D3D11_MAPPED_SUBRESOURCE sub;
    _ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &sub);
    memcpy(sub.pData, cur.staging.data(), cur.staging.size());
    _ctx->Unmap(buffer, 0);
    ps_cb[i] = buffer;
  }

  if (!vs.empty())
    _ctx->VSSetConstantBuffers(0, vs.size(), vs_cb);

  if (!ps.empty())
    _ctx->PSSetConstantBuffers(0, ps.size(), ps_cb);

}

void DeferredContext::draw_indexed(int count, int start_index, int base_vertex) {
  _ctx->DrawIndexed(count, start_index, base_vertex);
}

#include "stdafx.h"
#include "technique.hpp"
#include "dx_utils.hpp"
#include "graphics_interface.hpp"
#include "resource_interface.hpp"
#include "tracked_location.hpp"
#include "path_utils.hpp"
#include "graphics.hpp"

using namespace boost::assign;
using namespace std;

Technique::Technique()
  : _vertex_shader(nullptr)
  , _pixel_shader(nullptr)
  , _rasterizer_desc(CD3D11_DEFAULT())
  , _blend_desc(CD3D11_DEFAULT())
  , _depth_stencil_desc(CD3D11_DEFAULT())
  , _vertex_size(-1)
  , _index_format(DXGI_FORMAT_UNKNOWN)
  , _valid(false)
  , _first_sampler(MAX_SAMPLERS)
  , _num_valid_samplers(0)
{
}

Technique::~Technique() {
  SAFE_DELETE(_vertex_shader);
  SAFE_DELETE(_pixel_shader);
}

void Technique::add_error_msg(const char *fmt, ...) {
  va_list arg;
  va_start(arg, fmt);

  const int len = _vscprintf(fmt, arg) + 1;

  char* buf = (char*)_alloca(len);
  vsprintf_s(buf, len, fmt, arg);
  va_end(arg);

  if (_error_msg.empty())
    _error_msg = buf;
  else
    _error_msg += "\n" + string(buf);

  _valid = false;
}

bool Technique::parse_input_layout(GraphicsInterface *graphics, ID3D11ShaderReflection* reflector, 
                                                   const D3D11_SHADER_DESC &shader_desc, void *buf, size_t len) {
  auto mapping = map_list_of
    ("SV_POSITION", DXGI_FORMAT_R32G32B32_FLOAT)
    ("POSITION", DXGI_FORMAT_R32G32B32_FLOAT)
    ("NORMAL", DXGI_FORMAT_R32G32B32_FLOAT)
    ("COLOR", DXGI_FORMAT_R32G32B32A32_FLOAT)
    ("TEXCOORD", DXGI_FORMAT_R32G32_FLOAT);

  // ShaderInputs inputs;
  vector<D3D11_INPUT_ELEMENT_DESC> inputs;
  for (UINT i = 0; i < shader_desc.InputParameters; ++i) {
    D3D11_SIGNATURE_PARAMETER_DESC input;
    reflector->GetInputParameterDesc(i, &input);

    DXGI_FORMAT fmt;
    if (!lookup<string, DXGI_FORMAT>(input.SemanticName, mapping, &fmt)) {
      add_error_msg("Invalid input semantic found: %s", input.SemanticName);
      return false;
    }

    inputs.push_back(CD3D11_INPUT_ELEMENT_DESC(input.SemanticName, input.SemanticIndex, fmt, input.Stream));
  }

  _input_layout = graphics->create_input_layout(FROM_HERE, &inputs[0], inputs.size(), buf, len);
  if (!_input_layout.is_valid())
    add_error_msg("Invalid input layout");

  return _input_layout.is_valid();
}

CBufferParam *Technique::find_cbuffer_param(const std::string &name, Shader::Type type) {
  vector<CBufferParam> *params = type == Shader::kVertexShader ? &_vs_cbuffer_params : &_ps_cbuffer_params;
  for (auto it = begin(*params); it != end(*params); ++it) {
    if (it->name == name)
      return &(*it);
  }
  return nullptr;
}

bool Technique::do_reflection(GraphicsInterface *graphics, Shader *shader, void *buf, size_t len)
{
  ID3D11ShaderReflection* reflector = NULL; 
  B_ERR_HR(D3DReflect(buf, len, IID_ID3D11ShaderReflection, (void**)&reflector));
  D3D11_SHADER_DESC shader_desc;
  reflector->GetDesc(&shader_desc);

  // Parse input layout for vertex shaders
  if (shader->type() == Shader::kVertexShader && !parse_input_layout(graphics, reflector, shader_desc, buf, len))
    return false;

  // The technique contains both vs and ps cbuffers
  for (UINT i = 0; i < shader_desc.ConstantBuffers; ++i) {
    ID3D11ShaderReflectionConstantBuffer* cb = reflector->GetConstantBufferByIndex(i);
    D3D11_SHADER_BUFFER_DESC cb_desc;
    cb->GetDesc(&cb_desc);

    CBuffer cbuffer;
    int size = 0;

    for (UINT j = 0; j < cb_desc.Variables; ++j) {
      ID3D11ShaderReflectionVariable* v = cb->GetVariableByIndex(j);
      D3D11_SHADER_VARIABLE_DESC var_desc;
      v->GetDesc(&var_desc);
      if (var_desc.uFlags & D3D_SVF_USED) {
        // Get the parameter from the shader (as specified in the .tec file)
        CBufferParam *param = find_cbuffer_param(var_desc.Name, shader->type());
        if (!param) {
          add_error_msg("Shader parameter %s not found", var_desc.Name);
          return false;
        }

        CBufferVariable var;
        var.ofs = var_desc.StartOffset;
        var.len = var_desc.Size;
        size = max(size, var.ofs + var .len);
        // The id is either a classifer (for mesh/material), or a real id in the system case
        string id = PropertySource::to_string(param->source) + "::" + var_desc.Name;
        int var_size = param->source == PropertySource::kMesh ? sizeof(int) : var_desc.Size;
        var.id = PROPERTY_MANAGER.get_or_create_raw(id.c_str(), var_size, nullptr);
#ifdef _DEBUG
        var.name = id;
#endif
        switch (param->source) {
          case PropertySource::kMaterial: cbuffer.material_vars.emplace_back(var); break;
          case PropertySource::kMesh: cbuffer.mesh_vars.emplace_back(var); break;
          case PropertySource::kSystem: cbuffer.system_vars.emplace_back(var); break;
          default: LOG_WARNING_LN("Unsupported property source for %s", var_desc.Name);
        }
        param->used = true;
      }
    }

    cbuffer.handle = GRAPHICS.create_constant_buffer(FROM_HERE, size, true);
    cbuffer.staging.resize(size);
    if (shader->type() == Shader::kVertexShader)
      _vs_cbuffers.emplace_back(cbuffer);
    else
      _ps_cbuffers.emplace_back(cbuffer);
  }

  // get bound resources (textures and buffers)
  for (UINT i = 0; i < shader_desc.BoundResources; ++i) {
    D3D11_SHADER_INPUT_BIND_DESC input_desc;
    reflector->GetResourceBindingDesc(i, &input_desc);
    if (input_desc.BindCount > 1) {
      add_error_msg("Only bind counts of 1 are supported");
      return false;
    }

    switch (input_desc.Type) {

      case D3D10_SIT_TEXTURE: {
        ResourceViewParam *param = shader->find_resource_view_param(input_desc.Name);
        if (!param) {
          add_error_msg("Resource view %s not found", input_desc.Name);
          return false;
        }
        param->bind_point = input_desc.BindPoint;
        param->used = true;
        break;
      }

      case D3D10_SIT_SAMPLER: {
        _first_sampler = min(_first_sampler, (int)input_desc.BindPoint);
        _num_valid_samplers = max((int)input_desc.BindPoint - _first_sampler + 1, _num_valid_samplers);
        // find out where this sampler goes..
        for (auto i = begin(_sampler_states), e = end(_sampler_states); i != e; ++i) {
          if (i->first == input_desc.Name) {
            _ordered_sampler_states[input_desc.BindPoint] = i->second;
            goto FOUND_SAMPLER;
          }
        }
        add_error_msg("Sampler %s not found", input_desc.Name);
        FOUND_SAMPLER:
        break;
      }
    }
  }
  return true;
}

GraphicsObjectHandle Technique::cbuffer_handle() {
  return _constant_buffers.empty() ? GraphicsObjectHandle() : _constant_buffers[0].handle;
}

void Technique::update_cbuffers() {
  for (auto i = begin(_cbuffer_vars), e = end(_cbuffer_vars); i != e; ++i) {
    const CBufferVariable &var = *i;
    PROPERTY_MANAGER.get_property_raw(var.id, &_cbuffer_staged[var.ofs], var.len);
  }
}

void Technique::prepare_cbuffers() {
  // TODO
/*
  int cbuffer_size = 0;
  auto collect_params = [&, this](const std::vector<CBufferParam> &params) {
    for (auto i = std::begin(params), e = std::end(params); i != e; ++i) {
      const CBufferParam &param = *i;
      int len = PropertyType::len(param.type);;
      if (param.cbuffer != -1) {
        CBufferVariable &var = dummy_push_back(&_cbuffer_vars);
        var.ofs = param.start_offset;
        var.len = len;

        switch (param.source) {
          case PropertySource::kSystem:
            var.id = PROPERTY_MANAGER.get_or_create_raw(param.name.c_str(), var.len, nullptr);
            break;
          default:
            LOG_WARNING_LN("Unhandled parameter source");
            break;
        }

        cbuffer_size = max(cbuffer_size, var.ofs + var.len);
      }
    }
  };

  collect_params(_vertex_shader->cbuffer_params());
  collect_params(_pixel_shader->cbuffer_params());
  _cbuffer_staged.resize(cbuffer_size);
*/
}


bool Technique::compile_shader(GraphicsInterface *res, Shader *shader) {

  STARTUPINFOA startup_info;
  ZeroMemory(&startup_info, sizeof(startup_info));
  startup_info.cb = sizeof(STARTUPINFO);

  // create a pipe, and use it for stdout/stderr
  HANDLE stdout_read, stdout_write;
  SECURITY_ATTRIBUTES sattr;
  ZeroMemory(&sattr, sizeof(sattr));
  sattr.nLength = sizeof(SECURITY_ATTRIBUTES); 
  sattr.bInheritHandle = TRUE; 
  sattr.lpSecurityDescriptor = NULL;

  if (!CreatePipe(&stdout_read, &stdout_write, &sattr, 0)) {
    DWORD err = GetLastError();
    return false;
  }

  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdError = startup_info.hStdOutput = stdout_write;

  // redirect the child's output handles to the log manager's
  // TODO
  /*
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdOutput = startup_info.hStdError = LOGGER.handle();
  */
  PROCESS_INFORMATION process_info;
  ZeroMemory(&process_info, sizeof(process_info));

  const char *profile = shader->type() == Shader::kVertexShader ? res->vs_profile() : res->ps_profile();

  //fxc -nologo -T$(PROFILE) -E$(ENTRY_POINT) -Vi -O3 -Fo $(OBJECT_FILE) $(SOURCE_FILE)
  char cmd_line[MAX_PATH];
#ifdef _DEBUG
  sprintf(cmd_line, "fxc.exe -nologo -T%s -E%s -Vi -Od -Zi -Fo %s %s", 
    profile,
    shader->entry_point().c_str(),
    shader->obj_filename().c_str(), 
    shader->source_filename().c_str());
#else
  sprintf(cmd_line, "fxc.exe -nologo -T%s -E%s -Vi -O3 -Fo %s %s", 
    profile,
    shader->entry_point().c_str(),
    shader->obj_filename().c_str(), 
    shader->source_filename().c_str());
#endif

  DWORD exit_code = 1;

  if (CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE, NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, NULL, NULL, 
                     &startup_info, &process_info)) {
    WaitForSingleObject(process_info.hProcess, INFINITE);
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    FlushFileBuffers(LOGGER.handle());
    CloseHandle(process_info.hProcess);
    CloseHandle(process_info.hThread);
  } else {
    add_error_msg("Unable to launch fxc.exe");
  }

  if (exit_code) {
    // read the pipe
    string err_msg;
    char buf[4096+1];
    while (true) {
      DWORD bytes_read;
      BOOL res = ReadFile(stdout_read, buf, sizeof(buf)-1, &bytes_read, NULL);
      if (!res || bytes_read == 0)
        break;
      buf[bytes_read] = 0;
      err_msg += buf;
      if (bytes_read < sizeof(buf)-1)
        break;
    }
    add_error_msg(err_msg.c_str());
    LOG_WARNING_LN(err_msg.c_str());
  }
  CloseHandle(stdout_read);
  CloseHandle(stdout_write);

  return exit_code == 0;
}

bool Technique::reload_shaders() {
  return true;
}

bool Technique::init_shader(GraphicsInterface *graphics, ResourceInterface *res, Shader *shader) {

  bool force = true;

  bool ok = true;
  const char *obj = shader->obj_filename().c_str();
  const char *src = shader->source_filename().c_str();
  // compile the shader if the source is newer, or the object file doesn't exist
  if (force || res->file_exists(src) && (!res->file_exists(obj) || res->mdate(src) > res->mdate(obj))) {
    if (!compile_shader(graphics, shader))
      return false;
  } else {
    if (!res->file_exists(obj))
      return false;
  }

  vector<uint8> buf;
  if (!res->load_file(obj, &buf))
    return false;

  int len = (int)buf.size();
  if (!do_reflection(graphics, shader, buf.data(), len))
    return false;

  shader->prune_unused_parameters();

  switch (shader->type()) {
    case Shader::kVertexShader: 
      shader->set_handle(graphics->create_vertex_shader(FROM_HERE, buf.data(), len, shader->id()));
      break;
    case Shader::kPixelShader: 
      shader->set_handle(graphics->create_pixel_shader(FROM_HERE, buf.data(), len, shader->id()));
      break;
    case Shader::kGeometryShader: break;
  }

  shader->validate();

  // check that our samplers are sequential
  if (_ordered_sampler_states[_first_sampler].is_valid()) {
    bool prev_valid = true;
    for (int i = _first_sampler + 1; i < MAX_SAMPLERS; ++i) {
      bool cur_valid = _ordered_sampler_states[i].is_valid();
      if (cur_valid && !prev_valid) {
        add_error_msg("Nonsequential sampler states found!");
      }
      prev_valid = cur_valid;
    }
  }
  return true;
}

bool Technique::init(GraphicsInterface *graphics, ResourceInterface *res) {

  _rasterizer_state = graphics->create_rasterizer_state(FROM_HERE, _rasterizer_desc);
  _blend_state = graphics->create_blend_state(FROM_HERE, _blend_desc);
  _depth_stencil_state = graphics->create_depth_stencil_state(FROM_HERE, _depth_stencil_desc);

  // create all the sampler states from the descs
  for (auto i = begin(_sampler_descs), e = end(_sampler_descs); i != e; ++i) {
    _sampler_states.push_back(make_pair(i->first, graphics->create_sampler_state(FROM_HERE, i->second)));
  }

  if (_vertex_shader && !init_shader(graphics, res, _vertex_shader))
    return false;

  if (_pixel_shader && !init_shader(graphics, res, _pixel_shader))
    return false;

  prepare_textures();
  prepare_cbuffers();

  return true;
}

GraphicsObjectHandle Technique::sampler_state(const char *name) const {
  for (auto it = begin(_sampler_states), e = end(_sampler_states); it != e; ++it) {
    if (it->first == name)
      return it->second;
  }
  return GraphicsObjectHandle();
}

void Technique::get_render_objects(RenderObjects *obj) {
  obj->vs = _vertex_shader->handle();
  obj->gs = GraphicsObjectHandle();
  obj->ps = _pixel_shader->handle();
  obj->layout = _input_layout;
  obj->rs = _rasterizer_state;
  obj->dss = _depth_stencil_state;
  obj->bs = _blend_state;
  obj->stencil_ref = GRAPHICS.default_stencil_ref();
  memcpy(obj->blend_factors, GRAPHICS.default_blend_factors(), sizeof(obj->blend_factors));
  memcpy(obj->samplers, &_ordered_sampler_states[_first_sampler], _num_valid_samplers * sizeof(GraphicsObjectHandle));
  obj->first_sampler = _first_sampler;
  obj->num_valid_samplers = _num_valid_samplers;
  obj->sample_mask = GRAPHICS.default_sample_mask();
}


void Technique::prepare_textures() {

  auto &params = _pixel_shader->resource_view_params();
  for (auto it = begin(params), e = end(params); it != e; ++it) {
    auto &name = it->name;
    auto &friendly_name = it->friendly_name;
    int bind_point = it->bind_point;
    _render_data.textures[bind_point] = GRAPHICS.find_resource(friendly_name.empty() ? name.c_str() : friendly_name.c_str());
    _render_data.first_texture = min(_render_data.first_texture, bind_point);
    _render_data.num_textures = max(_render_data.num_textures, bind_point - _render_data.first_texture + 1);
  }
}

void Technique::fill_cbuffer(CBuffer *cbuffer) {
  for (size_t i = 0; i < cbuffer->system_vars.size(); ++i) {
    auto &cur = cbuffer->system_vars[i];
    PROPERTY_MANAGER.get_property_raw(cur.id, &cbuffer->staging[cur.ofs], cur.len);
  }
}

#ifndef _GRAPHICS_HPP_
#define _GRAPHICS_HPP_

#include "utils.hpp"
#include "property_manager.hpp"
#include "id_buffer.hpp"

struct Io;
struct Shader;
struct Material;
class Technique;

using std::map;
using std::string;
using std::vector;
using std::pair;

enum FileEvent;

class GraphicsObjectHandle {
	friend class Graphics;
	enum { 
		cTypeBits = 8,
		cGenerationBits = 8,
		cIdBits = 12 
	};
	enum Type {
		kInvalid = -1,
		kContext,
		kVertexBuffer,
		kIndexBuffer,
		kConstantBuffer,
		kTexture,
		kRenderTarget,
		kShader,
		kInputLayout,
		kBlendState,
		kRasterizerState,
		kSamplerState,
		kDepthStencilState,
		kTechnique,
		kVertexShader,
		kPixelShader,
		kShaderResourceView,
	};	
	GraphicsObjectHandle(uint32 type, uint32 generation, uint32 id) : _type(type), _generation(generation), _id(id) {}
	union {
		struct {
			uint32 _type : 8;
			uint32 _generation : 8;
			uint32 _id : 12;
		};
		uint32 _data;
	};
public:
	GraphicsObjectHandle() : _data(kInvalid) {}
	GraphicsObjectHandle(uint32 data) : _data(data) {}
	bool is_valid() const { return _data != kInvalid; }
	operator int() const { return _data; }
	uint32 id() const { return _id; }
};

static_assert(sizeof(GraphicsObjectHandle) <= sizeof(uint32_t), "GraphicsObjectHandle too large");


// from http://realtimecollisiondetection.net/blog/?p=86
struct RenderKey {
	enum Cmd {
		kDelete,	// submitted when the object is deleted to avoid using stale data
		kSetRenderTarget,
		kRenderMesh,
		kRenderTechnique,
		kNumCommands
	};
	static_assert(kNumCommands < (1 << 16), "Too many commands");

	RenderKey() : data(0) {}

	union {
		// Written for little endianess
		struct {
			union {
				struct {
					uint32 padding : 6;
					uint32 material : 16;
					uint32 shader : 10;
				};
				uint32 handle : 32;  // a GraphicsObjectHandle
			};
			uint16 cmd;
			union {
				uint16 seq_nr;
				uint16 depth;
			};
		};
		uint64 data;
	};
};

static_assert(sizeof(RenderKey) <= sizeof(uint64_t), "RenderKey too large");

struct RenderTargetData;
struct TextureData;

struct MeshRenderData {
	MeshRenderData() : topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {}
	GraphicsObjectHandle vb, ib;
	DXGI_FORMAT index_format;
	int index_count;
	int vertex_size;
	int vertex_count;
	D3D_PRIMITIVE_TOPOLOGY topology;
	GraphicsObjectHandle technique;
	uint16 material_id;
	PropertyManager::Id mesh_id;
};

class Graphics {
public:

	static Graphics& instance();

	bool	init(const HWND hwnd, const int width, const int height);
	bool	close();
	void	clear(const XMFLOAT4& c);
	void	clear(GraphicsObjectHandle h, const XMFLOAT4 &c);
	void	present();
	void	resize(const int width, const int height);

	const char *vs_profile() const { return _vs_profile.c_str(); }
	const char *ps_profile() const { return _ps_profile.c_str(); }

	ID3D11Device* device() { return _device; }
  ID3D11DeviceContext* context() { return _immediate_context; }

  const D3D11_VIEWPORT& viewport() const { return _viewport; }

	void set_default_render_target();

  D3D_FEATURE_LEVEL feature_level() const { return _feature_level; }

	GraphicsObjectHandle create_render_target(int width, int height, bool shader_resource, const char *name);
	GraphicsObjectHandle create_texture(const D3D11_TEXTURE2D_DESC &desc, const char *name);

	bool map(GraphicsObjectHandle h, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *res);
	void unmap(GraphicsObjectHandle h, UINT sub);
	void copy_resource(GraphicsObjectHandle dst, GraphicsObjectHandle src);

	// Create a texture, and fill it with data
	bool create_texture(int width, int height, DXGI_FORMAT fmt, void *data, int data_width, int data_height, int data_pitch, TextureData *out);

  ID3D11RasterizerState *default_rasterizer_state() const { return _default_rasterizer_state; }
  ID3D11DepthStencilState *default_depth_stencil_state() const { return _default_depth_stencil_state; }
  uint32_t default_stencil_ref() const { return 0; }
  ID3D11BlendState *default_blend_state() const { return _default_blend_state; }
  const float *default_blend_factors() const { return _default_blend_factors; }
  uint32_t default_sample_mask() const { return 0xffffffff; }

  float fps() const { return _fps; }
  int width() const { return _width; }
  int height() const { return _height; }
	void size(int *width, int *height) const { *width = _width; *height = _height; }

	GraphicsObjectHandle create_constant_buffer(int size);
	GraphicsObjectHandle create_input_layout(const D3D11_INPUT_ELEMENT_DESC *desc, int elems, void *shader_bytecode, int len);
	GraphicsObjectHandle create_static_vertex_buffer(uint32_t data_size, const void* data);
	GraphicsObjectHandle create_static_index_buffer(uint32_t data_size, const void* data);

	GraphicsObjectHandle create_vertex_shader(void *shader_bytecode, int len, const string &id);
	GraphicsObjectHandle create_pixel_shader(void *shader_bytecode, int len, const string &id);

	bool load_techniques(const char *filename, vector<GraphicsObjectHandle> *techniques);
	Technique *find_technique(const char *name);

	GraphicsObjectHandle create_rasterizer_state(const D3D11_RASTERIZER_DESC &desc);
	GraphicsObjectHandle create_blend_state(const D3D11_BLEND_DESC &desc);
	GraphicsObjectHandle create_depth_stencil_state(const D3D11_DEPTH_STENCIL_DESC &desc);
	GraphicsObjectHandle create_sampler_state(const D3D11_SAMPLER_DESC &desc);

	HRESULT create_dynamic_vertex_buffer(uint32_t vertex_count, uint32_t vertex_size, ID3D11Buffer** vertex_buffer);
	HRESULT create_static_vertex_buffer(uint32_t vertex_count, uint32_t vertex_size, const void* data, ID3D11Buffer** vertex_buffer);
	HRESULT create_static_index_buffer(uint32_t index_count, uint32_t elem_size, const void* data, ID3D11Buffer** index_buffer);
	void set_vb(ID3D11DeviceContext *context, ID3D11Buffer *buf, uint32_t stride);

	GraphicsObjectHandle default_rt_handle() const { return _default_rt_handle; }

	// TODO: this should be on the context
	void submit_command(RenderKey key, void *data, void *data2 = NULL);
	uint16 next_seq_nr() const { assert(_render_commands.size() < 65536); return (uint16)_render_commands.size(); }
	void render();

private:
	DISALLOW_COPY_AND_ASSIGN(Graphics);

	Graphics();
	~Graphics();

	bool create_render_target(int width, int height, bool shader_resource, RenderTargetData *out);
	bool create_texture(const D3D11_TEXTURE2D_DESC &desc, TextureData *out);

	void set_cbuffer_params(Technique *technique, Shader *shader, uint16 material_id, PropertyManager::Id mesh_id);
	void set_samplers(Technique *technique, Shader *shader);
	void set_resource_views(Technique *technique, Shader *shader);

  bool create_back_buffers(int width, int height);
	static Graphics* _instance;

	int _width;
	int _height;
	D3D11_VIEWPORT _viewport;
	DXGI_FORMAT _buffer_format;
  D3D_FEATURE_LEVEL _feature_level;
	CComPtr<ID3D11Device> _device;
	CComPtr<IDXGISwapChain> _swap_chain;

	GraphicsObjectHandle _default_rt_handle;

	CComPtr<ID3D11Debug> _d3d_debug;

	CComPtr<ID3D11RasterizerState> _default_rasterizer_state;
	CComPtr<ID3D11DepthStencilState> _default_depth_stencil_state;
	CComPtr<ID3D11SamplerState> _default_sampler_state;
	float _default_blend_factors[4];
	CComPtr<ID3D11BlendState> _default_blend_state;

	DWORD _start_fps_time;
	int32_t _frame_count;
	float _fps;
	CComPtr<ID3D11DeviceContext> _immediate_context;

	enum { IdCount = 1 << 1 << GraphicsObjectHandle::cIdBits };
	IdBuffer<ID3D11VertexShader *, IdCount, string> _vertex_shaders;
	IdBuffer<ID3D11PixelShader *, IdCount, string> _pixel_shaders;
	IdBuffer<ID3D11Buffer *, IdCount> _vertex_buffers;
	IdBuffer<ID3D11Buffer *, IdCount> _index_buffers;
	IdBuffer<ID3D11Buffer *, IdCount> _constant_buffers;
	IdBuffer<Technique *, IdCount, string> _techniques;
	IdBuffer<ID3D11InputLayout *, IdCount> _input_layouts;

	IdBuffer<ID3D11BlendState *, IdCount> _blend_states;
	IdBuffer<ID3D11DepthStencilState *, IdCount> _depth_stencil_states;
	IdBuffer<ID3D11RasterizerState *, IdCount> _rasterizer_states;
	IdBuffer<ID3D11SamplerState *, IdCount> _sampler_states;
	IdBuffer<ID3D11ShaderResourceView *, IdCount> _shader_resource_views;
	IdBuffer<TextureData *, IdCount, string> _textures;
	IdBuffer<RenderTargetData *, IdCount, string> _render_targets;

	struct RenderCmd {
		RenderCmd(RenderKey key, void *data, void *data2) : key(key), data(data), data2(data2) {}
		RenderKey key;
		void *data;
		void *data2;
	};
	vector<RenderCmd > _render_commands;

	std::map<string, vector<string> > _techniques_by_file;

	string _vs_profile;
	string _ps_profile;
};

#define GRAPHICS Graphics::instance()

#define D3D_DEVICE Graphics::instance().device()
#define D3D_CONTEXT Graphics::instance().context()

#endif

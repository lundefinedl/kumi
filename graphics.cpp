#include "stdafx.h"
#include "graphics.hpp"
#include "logger.hpp"
#include "technique.hpp"
#include "technique_parser.hpp"
#include "resource_interface.hpp"
#include "material_manager.hpp"
#include "string_utils.hpp"
#include "tracked_location.hpp"
#include "mesh.hpp"
#include "vertex_types.hpp"
#include "deferred_context.hpp"
#include "profiler.hpp"
#include "effect.hpp"
#include "kumi.hpp"
#include "_win32/resource.h"

using namespace std;
using namespace std::tr1::placeholders;

#pragma comment(lib, "DXGI.lib")
#pragma comment(lib, "DXGUID.lib")
#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "D3DX11.lib")

#define USE_CONFIG_DLG 0

namespace
{
  template <class T>
  void set_private_data(const TrackedLocation &loc, T *t) {
#if WITH_TRACKED_LOCATION
    t->SetPrivateData(WKPDID_D3DDebugObjectName, strlen(loc.function), loc.function);
#else
#endif
  }

  uint32 multiple_of_16(uint32 a) {
    return (a + 15) & ~0xf;
  }
}

bool Graphics::enumerateDisplayModes(HWND hWnd) {
  // Create DXGI factory to enumerate adapters

  CComPtr<IDXGIFactory1> dxgi_factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)))) {
    return false;
  }

  GRAPHICS._curSetup.videoAdapters.clear();

  // Enumerate the adapters
  CComPtr<IDXGIAdapter1> adapter;
  for (int i = 0; SUCCEEDED(dxgi_factory->EnumAdapters1(i, &adapter)); ++i) {

    VideoAdapter &curAdapter = dummy_push_back(&GRAPHICS._curSetup.videoAdapters);
    curAdapter.adapter = adapter;
    adapter->GetDesc(&curAdapter.desc);
    HWND hAdapter = GetDlgItem(hWnd, IDC_VIDEO_ADAPTER);
    ComboBox_AddString(hAdapter, wide_char_to_utf8(curAdapter.desc.Description).c_str());
    ComboBox_SetCurSel(hAdapter, 0);
    GRAPHICS._curSetup.selectedAdapter = 0;

    IDXGIOutput *output = nullptr;
    vector<CComPtr<IDXGIOutput>> outputs;
    vector<DXGI_MODE_DESC> displayModes;

    // Only enumerate the first adapter
    for (int j = 0; SUCCEEDED(adapter->EnumOutputs(j, &output)); ++j) {
      DXGI_OUTPUT_DESC outputDesc;
      output->GetDesc(&outputDesc);
      UINT numModes;
      output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &numModes, NULL);
      size_t prevSize = displayModes.size();
      displayModes.resize(displayModes.size() + numModes);
      output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &numModes, displayModes.data() + prevSize);
      output->Release();
      if (!GRAPHICS._displayAllModes)
        break;
    }

    // Only keep the version of each display mode with the highest refresh rate
    auto &safeRational = [](const DXGI_RATIONAL &r) { return r.Denominator == 0 ? 0 : r.Numerator / r.Denominator; };
    if (GRAPHICS.displayAllModes()) {
      curAdapter.displayModes = displayModes;
    } else {
      std::map<pair<int, int>, DXGI_RATIONAL> highestRate;
      for (size_t i = 0; i < displayModes.size(); ++i) {
        auto &cur = displayModes[i];
        auto key = make_pair(cur.Width, cur.Height);
        if (safeRational(cur.RefreshRate) > safeRational(highestRate[key])) {
          highestRate[key] = cur.RefreshRate;
        }
      }

      for (auto it = begin(highestRate); it != end(highestRate); ++it) {
        DXGI_MODE_DESC desc;
        desc.Width = it->first.first;
        desc.Height = it->first.second;
        desc.RefreshRate = it->second;
        curAdapter.displayModes.push_back(desc);
      }

      auto &resSorter = [&](const DXGI_MODE_DESC &a, const DXGI_MODE_DESC &b) { 
        return a.Width < b.Width; };

      sort(begin(curAdapter.displayModes), end(curAdapter.displayModes), resSorter);
    }

    HWND hDisplayMode = GetDlgItem(hWnd, IDC_DISPLAY_MODES);
    for (size_t k = 0; k < curAdapter.displayModes.size(); ++k) {
      auto &cur = curAdapter.displayModes[k];
      char buf[256];
      sprintf(buf, "%dx%d (%dHz)", cur.Width, cur.Height, safeRational(cur.RefreshRate));
      ComboBox_InsertString(hDisplayMode, k, buf);
      // encode width << 16 | height in the item data
      ComboBox_SetItemData(hDisplayMode, k, (cur.Width << 16) | cur.Height);
    }

    int cnt = ComboBox_GetCount(hDisplayMode);
    ComboBox_SetCurSel(hDisplayMode, cnt-1);
  }

  HWND hMultisample = GetDlgItem(hWnd, IDC_MULTISAMPLE);
  ComboBox_InsertString(hMultisample, -1, "No multisample");
  ComboBox_InsertString(hMultisample, -1, "2x multisample");
  ComboBox_InsertString(hMultisample, -1, "4x multisample");
  ComboBox_InsertString(hMultisample, -1, "8x multisample");

  ComboBox_SetItemData(hMultisample, 0, 1);
  ComboBox_SetItemData(hMultisample, 1, 2);
  ComboBox_SetItemData(hMultisample, 2, 4);
  ComboBox_SetItemData(hMultisample, 3, 8);

  ComboBox_SetCurSel(hMultisample, 0);
  GRAPHICS._curSetup.multisampleCount = 1;

#if DISTRIBUTION
  GRAPHICS._curSetup.windowed = false;
#else
  GRAPHICS._curSetup.windowed = true;
  Button_SetCheck(GetDlgItem(hWnd, IDC_WINDOWED), TRUE);
#endif

  return true;
}

INT_PTR CALLBACK Graphics::dialogWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {

    case WM_INITDIALOG: {
      if (!enumerateDisplayModes(hWnd)) {
        EndDialog(hWnd, -1);
      }
#if !USE_CONFIG_DLG
      ComboBox_SetCurSel(GetDlgItem(hWnd, IDC_DISPLAY_MODES), 3 * GRAPHICS._curSetup.videoAdapters[0].displayModes.size() / 4);
      EndDialog(hWnd, 1);
#endif
      RECT rect;
      GetClientRect(hWnd, &rect);
      int width = GetSystemMetrics(SM_CXSCREEN);
      int height = GetSystemMetrics(SM_CYSCREEN);
      SetWindowPos(hWnd, NULL, width/2 - (rect.right - rect.left) / 2, height/2 - (rect.bottom - rect.top)/2, -1, -1, 
        SWP_NOZORDER | SWP_NOSIZE);
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDCANCEL:
          EndDialog(hWnd, 0);
          return TRUE;

        case IDOK:
          EndDialog(hWnd, 1);
          return TRUE;
      }
      break; // end WM_COMMAND

    case WM_DESTROY: {
      // read the settings
      GRAPHICS._curSetup.windowed = !!Button_GetCheck(GetDlgItem(hWnd, IDC_WINDOWED));
      GRAPHICS._curSetup.selectedDisplayMode = ComboBox_GetCurSel(GetDlgItem(hWnd, IDC_DISPLAY_MODES));

      HWND hMultisample = GetDlgItem(hWnd, IDC_MULTISAMPLE);
      GRAPHICS._curSetup.multisampleCount = ComboBox_GetItemData(hMultisample, ComboBox_GetCurSel(hMultisample));

      HWND hDisplayModes = GetDlgItem(hWnd, IDC_DISPLAY_MODES);
      int sel = ComboBox_GetCurSel(hDisplayModes);
      int data = ComboBox_GetItemData(hDisplayModes, sel);
      int w = HIWORD(data);
      GRAPHICS._width = w;
      GRAPHICS._height = LOWORD(data);
      break;
    }
  }
  return FALSE;
}

static GraphicsObjectHandle emptyGoh;

Graphics* Graphics::_instance;

Graphics::Graphics()
  : _width(-1)
  , _height(-1)
  , _buffer_format(DXGI_FORMAT_R8G8B8A8_UNORM)
  , _start_fps_time(0xffffffff)
  , _frame_count(0)
  , _fps(0)
  , _vs_profile("vs_5_0")
  , _ps_profile("ps_5_0")
  , _cs_profile("cs_5_0")
  , _gs_profile("gs_5_0")
  , _vertex_shaders(release_obj<ID3D11VertexShader *>)
  , _pixel_shaders(release_obj<ID3D11PixelShader *>)
  , _compute_shaders(release_obj<ID3D11ComputeShader *>)
  , _geometry_shaders(release_obj<ID3D11GeometryShader *>)
  , _vertex_buffers(release_obj<ID3D11Buffer *>)
  , _index_buffers(release_obj<ID3D11Buffer *>)
  , _constant_buffers(release_obj<ID3D11Buffer *>)
  , _techniques(delete_obj<Technique *>)
  , _input_layouts(release_obj<ID3D11InputLayout *>)
  , _blend_states(release_obj<ID3D11BlendState *>)
  , _depth_stencil_states(release_obj<ID3D11DepthStencilState *>)
  , _rasterizer_states(release_obj<ID3D11RasterizerState *>)
  , _sampler_states(release_obj<ID3D11SamplerState *>)
  , _shader_resource_views(release_obj<ID3D11ShaderResourceView *>)
  , _textures(delete_obj<TextureResource *>)
  , _render_targets(delete_obj<RenderTargetResource *>)
  , _resources(delete_obj<SimpleResource *>)
  , _structured_buffers(delete_obj<StructuredBuffer *>)
  , _vsync(false)
  , _totalBytesAllocated(0)
  , _displayAllModes(false)
{
}

bool Graphics::create() {
  assert(!_instance);
  _instance = new Graphics();
  return true;
}

bool Graphics::close() {
  delete exch_null(_instance);
  return true;
}

void Graphics::setClientSize()
{
  RECT client_rect;
  RECT window_rect;
  GetClientRect(_hwnd, &client_rect);
  GetWindowRect(_hwnd, &window_rect);
  window_rect.right -= window_rect.left;
  window_rect.bottom -= window_rect.top;
  const int dx = window_rect.right - client_rect.right;
  const int dy = window_rect.bottom - client_rect.bottom;

  SetWindowPos(_hwnd, NULL, -1, -1, _width + dx, _height + dy, SWP_NOZORDER | SWP_NOREPOSITION);
}

bool Graphics::createWindow(WNDPROC wndProc) {

  WNDCLASSEX wcex;
  ZeroMemory(&wcex, sizeof(wcex));

  wcex.cbSize = sizeof(WNDCLASSEX);
  wcex.style          = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc    = wndProc;
  wcex.hInstance      = _hInstance;
  wcex.hbrBackground  = 0;
  wcex.lpszClassName  = g_app_window_class;

  B_ERR_INT(RegisterClassEx(&wcex));

  //const UINT window_style = WS_VISIBLE | WS_POPUP | WS_OVERLAPPEDWINDOW;
  const UINT window_style = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS; //WS_VISIBLE | WS_POPUP;

  _hwnd = CreateWindow(g_app_window_class, g_app_window_title, window_style,
    CW_USEDEFAULT, CW_USEDEFAULT, _width, _height, NULL, NULL,
    _hInstance, NULL);

  setClientSize();

  ShowWindow(_hwnd, SW_SHOW);

  return true;
}

const DXGI_MODE_DESC &Graphics::selectedDisplayMode() const {
  return _curSetup.videoAdapters[_curSetup.selectedAdapter].displayModes[_curSetup.selectedDisplayMode];
}

bool Graphics::init(WNDPROC wndProc)
{
  auto &curMode = selectedDisplayMode();

  createWindow(wndProc);

  DXGI_SWAP_CHAIN_DESC swapchain_desc;
  ZeroMemory(&swapchain_desc, sizeof(swapchain_desc));
  swapchain_desc.BufferCount = 3;
  swapchain_desc.BufferDesc.Width = _width;
  swapchain_desc.BufferDesc.Height = _height;
  swapchain_desc.BufferDesc.Format = _buffer_format;
  swapchain_desc.BufferDesc.RefreshRate = curMode.RefreshRate;
  swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapchain_desc.OutputWindow = _hwnd;
  swapchain_desc.SampleDesc.Count = _curSetup.multisampleCount;
  swapchain_desc.SampleDesc.Quality = 0;
  swapchain_desc.Windowed = _curSetup.windowed;

  int flags = 0;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  // Create the DX11 device
  B_ERR_HR(D3D11CreateDeviceAndSwapChain(
    _curSetup.videoAdapters[_curSetup.selectedAdapter].adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, NULL, 0, 
    D3D11_SDK_VERSION, &swapchain_desc, &_swap_chain.p, &_device.p,
    &_feature_level, &_immediate_context.p));
  set_private_data(FROM_HERE, _immediate_context.p);

  B_ERR_BOOL(_feature_level >= D3D_FEATURE_LEVEL_9_3);

#ifdef _DEBUG
  B_ERR_HR(_device->QueryInterface(IID_ID3D11Debug, (void **)&(_d3d_debug.p)));
#endif

  B_ERR_BOOL(create_back_buffers(_width, _height));

  _default_depth_stencil_state = create_depth_stencil_state(FROM_HERE, CD3D11_DEPTH_STENCIL_DESC(CD3D11_DEFAULT()));
  _default_rasterizer_state = create_rasterizer_state(FROM_HERE, CD3D11_RASTERIZER_DESC(CD3D11_DEFAULT()));
  _default_blend_state = create_blend_state(FROM_HERE, CD3D11_BLEND_DESC(CD3D11_DEFAULT()));

  _device->CreateClassLinkage(&_class_linkage.p);

  create_default_geometry();

  // Create a dummy texture
  DWORD black = 0;
  _dummy_texture = create_texture(FROM_HERE, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, &black, 1, 1, 1, "dummy_texture");
  _immediate_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  for (int i = 0; i < 4; ++i)
    _default_blend_factors[i] = 1.0f;
  return true;
}

GraphicsObjectHandle Graphics::create_buffer(const TrackedLocation &loc, 
                                             D3D11_BIND_FLAG bind, int size, bool dynamic, const void* buf, int data) {

  if (bind == D3D11_BIND_INDEX_BUFFER) {
    const int idx = _index_buffers.find_free_index();
    if (idx != -1 && create_buffer_inner(loc, bind, size, dynamic, buf, &_index_buffers[idx])) {
      KASSERT(data == DXGI_FORMAT_R16_UINT || data == DXGI_FORMAT_R32_UINT);
      return make_goh(GraphicsObjectHandle::kIndexBuffer, idx, data);
    }

  } else if (bind == D3D11_BIND_VERTEX_BUFFER) {
    const int idx = _vertex_buffers.find_free_index();
    if (idx != -1 && create_buffer_inner(loc, bind, size, dynamic, buf, &_vertex_buffers[idx])) {
      KASSERT(data > 0);
      return make_goh(GraphicsObjectHandle::kVertexBuffer, idx, data);
    }

  } else if (bind == D3D11_BIND_CONSTANT_BUFFER) {
    const int idx = _constant_buffers.find_free_index();
    if (idx != -1 && create_buffer_inner(loc, bind, size, dynamic, buf, &_constant_buffers[idx]))
      return make_goh(GraphicsObjectHandle::kConstantBuffer, idx, size);

  } else {
    LOG_ERROR_LN("Implement me!");
  }
  return emptyGoh;
}


bool Graphics::create_buffer_inner(const TrackedLocation &loc, D3D11_BIND_FLAG bind, int size, bool dynamic, const void* data, ID3D11Buffer** buffer)
{
  CD3D11_BUFFER_DESC desc(multiple_of_16(size), bind, 
    dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT, 
    dynamic ? D3D11_CPU_ACCESS_WRITE : 0);

  D3D11_SUBRESOURCE_DATA init_data;
  ZeroMemory(&init_data, sizeof(init_data));
  init_data.pSysMem = data;
  HRESULT hr = _device->CreateBuffer(&desc, data ? &init_data : NULL, buffer);
  if (hr == E_OUTOFMEMORY) {
    LOG_ERROR_LN("Out of memory trying to allocate %d bytes for buffer [total allocated: %d bytes]", size, _totalBytesAllocated);
  }
  set_private_data(loc, *buffer);
  _totalBytesAllocated += size;
  return SUCCEEDED(hr);
}

GraphicsObjectHandle Graphics::get_temp_render_target(const TrackedLocation &loc, 
    int width, int height, DXGI_FORMAT format, uint32 bufferFlags, const std::string &name) {

  KASSERT(_render_targets._key_to_idx.find(name) == _render_targets._key_to_idx.end());

  // look for a free render target with the wanted properties
  UINT flags = (bufferFlags & kCreateMipMaps) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

  auto cmp_desc = [&](const D3D11_TEXTURE2D_DESC &desc) {
    return desc.Width == width && desc.Height == height && desc.Format == format && desc.MiscFlags == flags;
  };

  for (int idx = 0; idx < _render_targets.Size; ++idx) {
    if (auto rt = _render_targets[idx]) {
      auto &desc = rt->texture.desc;
      if (!rt->in_use && cmp_desc(desc) && !!(bufferFlags & kCreateDepthBuffer) == !!rt->depth_stencil.resource.p) {
        rt->in_use = true;
        _render_targets._key_to_idx[name] = idx;
        _render_targets._idx_to_key[idx] = name;
        auto goh = make_goh(GraphicsObjectHandle::kRenderTarget, idx);
        auto pid = PROPERTY_MANAGER.get_or_create<GraphicsObjectHandle>(name);
        PROPERTY_MANAGER.set_property(pid, goh);
        return goh;
      }
    }
  }
  // nothing suitable found, so we create a render target
  return create_render_target(loc, width, height, format, bufferFlags, name);
}

void Graphics::release_temp_render_target(GraphicsObjectHandle h) {
  auto rt = _render_targets.get(h);
  KASSERT(rt->in_use);
  rt->in_use = false;
  int idx = h.id();
  string key = _render_targets._idx_to_key[idx];
  _render_targets._idx_to_key.erase(idx);
  _render_targets._key_to_idx.erase(key);
}

GraphicsObjectHandle Graphics::create_structured_buffer(const TrackedLocation &loc, int elemSize, int numElems, bool createSrv) {

  unique_ptr<StructuredBuffer> sb(new StructuredBuffer);

  // Create Structured Buffer
  D3D11_BUFFER_DESC sbDesc;
  sbDesc.BindFlags            = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  sbDesc.CPUAccessFlags       = 0;
  sbDesc.MiscFlags            = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  sbDesc.StructureByteStride  = elemSize;
  sbDesc.ByteWidth            = elemSize * numElems;
  sbDesc.Usage                = D3D11_USAGE_DEFAULT;
  if (FAILED(_device->CreateBuffer(&sbDesc, NULL, &sb->buffer.resource)))
    return emptyGoh;

  auto buf = sb->buffer.resource.p;
  set_private_data(loc, sb->buffer.resource.p);

  // create the UAV for the structured buffer
  D3D11_UNORDERED_ACCESS_VIEW_DESC sbUAVDesc;
  sbUAVDesc.Buffer.FirstElement       = 0;
  sbUAVDesc.Buffer.Flags              = 0;
  sbUAVDesc.Buffer.NumElements        = numElems;
  sbUAVDesc.Format                    = DXGI_FORMAT_UNKNOWN;
  sbUAVDesc.ViewDimension             = D3D11_UAV_DIMENSION_BUFFER;
  if (FAILED(_device->CreateUnorderedAccessView(buf, &sbUAVDesc, &sb->uav.resource)))
    return emptyGoh;

  set_private_data(loc, sb->uav.resource.p);

  if (createSrv) {
    // create the Shader Resource View (SRV) for the structured buffer
    D3D11_SHADER_RESOURCE_VIEW_DESC sbSRVDesc;
    sbSRVDesc.Buffer.ElementOffset          = 0;
    sbSRVDesc.Buffer.ElementWidth           = elemSize;
    sbSRVDesc.Buffer.FirstElement           = 0;
    sbSRVDesc.Buffer.NumElements            = numElems;
    sbSRVDesc.Format                        = DXGI_FORMAT_UNKNOWN;
    sbSRVDesc.ViewDimension                 = D3D11_SRV_DIMENSION_BUFFER;
    if (FAILED(_device->CreateShaderResourceView(buf, &sbSRVDesc, &sb->srv.resource)))
      return emptyGoh;

    set_private_data(loc, sb->srv.resource.p);

  }

  int idx = _structured_buffers.find_free_index();
  if (idx != -1) {
    _structured_buffers[idx] = sb.release();
    return make_goh(GraphicsObjectHandle::kStructuredBuffer, idx);
  }

  return emptyGoh;
}

GraphicsObjectHandle Graphics::create_render_target(const TrackedLocation &loc, int width, int height, DXGI_FORMAT format, uint32 bufferFlags, const std::string &name) {

    unique_ptr<RenderTargetResource> data(new RenderTargetResource);
    if (create_render_target(loc, width, height, format, bufferFlags, data.get())) {
      int idx = !name.empty() ? _render_targets.idx_from_token(name) : -1;
      if (idx != -1 || (idx = _render_targets.find_free_index()) != -1) {
        if (_render_targets[idx])
          _render_targets[idx]->reset();
        _render_targets.set_pair(idx, make_pair(name, data.release()));
        auto goh = make_goh(GraphicsObjectHandle::kRenderTarget, idx);
        auto pid = PROPERTY_MANAGER.get_or_create<GraphicsObjectHandle>(name);
        PROPERTY_MANAGER.set_property(pid, goh);
        return goh;
      }
    }
    return emptyGoh;
}

bool Graphics::create_render_target(const TrackedLocation &loc, int width, int height, DXGI_FORMAT format, uint32 bufferFlags, RenderTargetResource *out)
{
  out->reset();

  // create the render target
  int mip_levels = (bufferFlags & kCreateMipMaps) ? 0 : 1;
  uint32 flags = D3D11_BIND_RENDER_TARGET 
    | (bufferFlags & kCreateSrv ? D3D11_BIND_SHADER_RESOURCE : 0)
    | (bufferFlags & kCreateUav ? D3D11_BIND_UNORDERED_ACCESS : 0);

  out->texture.desc = CD3D11_TEXTURE2D_DESC(format, width, height, 1, mip_levels, flags);
  out->texture.desc.MiscFlags = (bufferFlags & kCreateMipMaps) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
  B_ERR_HR(_device->CreateTexture2D(&out->texture.desc, NULL, &out->texture.resource.p));
  set_private_data(loc, out->texture.resource.p);

  // create the render target view
  out->rtv.desc = CD3D11_RENDER_TARGET_VIEW_DESC(D3D11_RTV_DIMENSION_TEXTURE2D, out->texture.desc.Format);
  B_ERR_HR(_device->CreateRenderTargetView(out->texture.resource, &out->rtv.desc, &out->rtv.resource.p));
  set_private_data(loc, out->rtv.resource.p);
  float color[4] = { 0 };
  _immediate_context->ClearRenderTargetView(out->rtv.resource.p, color);

  if (bufferFlags & kCreateDepthBuffer) {
    // create the depth stencil texture
    out->depth_stencil.desc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_D24_UNORM_S8_UINT, width, height, 1, 1, D3D11_BIND_DEPTH_STENCIL);
    B_ERR_HR(_device->CreateTexture2D(&out->depth_stencil.desc, NULL, &out->depth_stencil.resource.p));
    set_private_data(loc, out->depth_stencil.resource.p);

    // create depth stencil view
    out->dsv.desc = CD3D11_DEPTH_STENCIL_VIEW_DESC(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D24_UNORM_S8_UINT);
    B_ERR_HR(_device->CreateDepthStencilView(out->depth_stencil.resource, &out->dsv.desc, &out->dsv.resource.p));
    set_private_data(loc, out->dsv.resource.p);
  }

  if (bufferFlags & kCreateSrv) {
    // create the shader resource view
    out->srv.desc = CD3D11_SHADER_RESOURCE_VIEW_DESC(D3D11_SRV_DIMENSION_TEXTURE2D, out->texture.desc.Format);
    B_ERR_HR(_device->CreateShaderResourceView(out->texture.resource, &out->srv.desc, &out->srv.resource.p));
    set_private_data(loc, out->srv.resource.p);
  }


  if (bufferFlags & kCreateUav) {
    out->uav.desc = CD3D11_UNORDERED_ACCESS_VIEW_DESC(D3D11_UAV_DIMENSION_TEXTURE2D, format, 0, 0, width*height);
    B_ERR_HR(_device->CreateUnorderedAccessView(out->texture.resource, &out->uav.desc, &out->uav.resource));
  }

  return true;
}

bool Graphics::read_texture(const char *filename, D3DX11_IMAGE_INFO *info, uint32 *pitch, vector<uint8> *bits) {

  HRESULT hr;
  D3DX11GetImageInfoFromFile(filename, NULL, info, &hr);
  if (FAILED(hr))
    return false;

  D3DX11_IMAGE_LOAD_INFO loadinfo;
  ZeroMemory(&loadinfo, sizeof(D3DX11_IMAGE_LOAD_INFO));
  loadinfo.CpuAccessFlags = D3D11_CPU_ACCESS_READ;
  loadinfo.Usage = D3D11_USAGE_STAGING;
  loadinfo.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  CComPtr<ID3D11Resource> resource;
  D3DX11CreateTextureFromFile(_device, filename, &loadinfo, NULL, &resource.p, &hr);
  if (FAILED(hr))
    return false;

  D3D11_MAPPED_SUBRESOURCE sub;
  if (FAILED(_immediate_context->Map(resource, 0, D3D11_MAP_READ, 0, &sub)))
    return false;

  uint8 *src = (uint8 *)sub.pData;
  bits->resize(sub.RowPitch * info->Height);
  uint8 *dst = &(*bits)[0];

  int row_size = 4 * info->Width;
  for (uint32 i = 0; i < info->Height; ++i) {
    memcpy(&dst[i*sub.RowPitch], &src[i*sub.RowPitch], row_size);
  }

  _immediate_context->Unmap(resource, 0);
  *pitch = sub.RowPitch;
  return true;
}

GraphicsObjectHandle Graphics::get_texture(const char *filename) {
  int idx = _resources.idx_from_token(filename);
  return make_goh(GraphicsObjectHandle::kResource, idx);
}

GraphicsObjectHandle Graphics::load_texture(const char *filename, const char *friendly_name, bool srgb, D3DX11_IMAGE_INFO *info) {

  D3DX11_IMAGE_INFO imageInfo;
  if (FAILED(D3DX11GetImageInfoFromFile(filename, NULL, &imageInfo, NULL)))
    return emptyGoh;

  if (info)
    *info = imageInfo;

  auto data = unique_ptr<SimpleResource>(new SimpleResource());
  if (FAILED(D3DX11CreateTextureFromFile(_device, filename, NULL, NULL, &data->resource, NULL)))
    return emptyGoh;

  // TODO: allow for srgb loading
  auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  // check if this is a cube map
  auto dim = imageInfo.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2D;
  auto desc = CD3D11_SHADER_RESOURCE_VIEW_DESC(dim, fmt);
  if (FAILED(_device->CreateShaderResourceView(data->resource, &desc, &data->view.resource)))
    return emptyGoh;

  int idx = _resources.find_free_index(friendly_name);
  if (_resources[idx])
    _resources[idx]->reset();
  _resources.set_pair(idx, make_pair(friendly_name, data.release()));
  return make_goh(GraphicsObjectHandle::kResource, idx);
}

GraphicsObjectHandle Graphics::load_texture_from_memory(const void *buf, size_t len, const char *friendly_name, bool srgb, D3DX11_IMAGE_INFO *info) {

  if (info && FAILED(D3DX11GetImageInfoFromMemory(buf, len, NULL, info, NULL)))
    return emptyGoh;

  auto data = unique_ptr<SimpleResource>(new SimpleResource());
  if (FAILED(D3DX11CreateTextureFromMemory(_device, buf, len, NULL, NULL, &data->resource, NULL)))
    return emptyGoh;

  // TODO: allow for srgb loading
  auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto desc = CD3D11_SHADER_RESOURCE_VIEW_DESC(D3D11_SRV_DIMENSION_TEXTURE2D, fmt);
  if (FAILED(_device->CreateShaderResourceView(data->resource, &desc, &data->view.resource)))
    return emptyGoh;

  int idx = _resources.idx_from_token(friendly_name);
  if (idx != -1 || (idx = _resources.find_free_index()) != -1) {
    if (_resources[idx])
      _resources[idx]->reset();
    _resources.set_pair(idx, make_pair(friendly_name, data.release()));
  }
  return make_goh(GraphicsObjectHandle::kResource, idx);
}

GraphicsObjectHandle Graphics::insert_texture(TextureResource *data, const char *friendly_name) {

  int idx = friendly_name ? _textures.idx_from_token(friendly_name) : -1;
  if (idx != -1 || (idx = _textures.find_free_index()) != -1) {
    if (_textures[idx])
      _textures[idx]->reset();
    _textures.set_pair(idx, make_pair(friendly_name, data));
  }
  return make_goh(GraphicsObjectHandle::kTexture, idx);
}

GraphicsObjectHandle Graphics::create_texture(const TrackedLocation &loc, const D3D11_TEXTURE2D_DESC &desc, const char *name) {
  TextureResource *data = new TextureResource;
  if (!create_texture(loc, desc, data)) {
    delete exch_null(data);
    return emptyGoh;
  }
  return insert_texture(data, name);
}

bool Graphics::create_texture(const TrackedLocation &loc, const D3D11_TEXTURE2D_DESC &desc, TextureResource *out)
{
  out->reset();

  // create the texture
  out->texture.desc = desc;
  B_WRN_HR(_device->CreateTexture2D(&out->texture.desc, NULL, &out->texture.resource.p));
  set_private_data(loc, out->texture.resource.p);

  // create the shader resource view if the texture has a shader resource bind flag
  if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
    out->view.desc = CD3D11_SHADER_RESOURCE_VIEW_DESC(D3D11_SRV_DIMENSION_TEXTURE2D, out->texture.desc.Format);
    B_WRN_HR(_device->CreateShaderResourceView(out->texture.resource, &out->view.desc, &out->view.resource.p));
    set_private_data(loc, out->view.resource.p);
  }

  return true;
}

GraphicsObjectHandle Graphics::create_texture(const TrackedLocation &loc, int width, int height, DXGI_FORMAT fmt, 
                                              void *data_bits, int data_width, int data_height, int data_pitch, const char *friendly_name) {
  TextureResource *data = new TextureResource;
  if (!create_texture(loc, width, height, fmt, data_bits, data_width, data_height, data_pitch, data)) {
    delete exch_null(data);
    return emptyGoh;
  }
  return insert_texture(data, friendly_name);
}

bool Graphics::create_texture(const TrackedLocation &loc, int width, int height, DXGI_FORMAT fmt, 
                              void *data, int data_width, int data_height, int data_pitch, 
                              TextureResource *out)
{
  if (!create_texture(loc, CD3D11_TEXTURE2D_DESC(fmt, width, height, 1, 1, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE), out))
    return false;

  D3D11_MAPPED_SUBRESOURCE resource;
  B_ERR_HR(_immediate_context->Map(out->texture.resource, 0, D3D11_MAP_WRITE_DISCARD, 0, &resource));
  uint8_t *src = (uint8_t *)data;
  uint8_t *dst = (uint8_t *)resource.pData;
  const int w = std::min<int>(width, data_width);
  const int h = std::min<int>(height, data_height);
  for (int i = 0; i < h; ++i) {
    memcpy(dst, src, w);
    src += data_pitch;
    dst += resource.RowPitch;
  }
  _immediate_context->Unmap(out->texture.resource, 0);
  return true;
}

bool Graphics::create_back_buffers(int width, int height)
{
  KASSERT(width && height);
  _width = width;
  _height = height;

  int idx = _render_targets.idx_from_token("default_rt");
  RenderTargetResource *rt = nullptr;
  if (idx != -1) {
    rt = _render_targets[idx];
    rt->reset();
    // release any existing buffers
    //_immediate_context->ClearState();
    _swap_chain->ResizeBuffers(1, width, height, _buffer_format, 0);
  } else {
    rt = new RenderTargetResource;
    idx = _render_targets.find_free_index();
  }

  // Get the dx11 back buffer
  B_ERR_HR(_swap_chain->GetBuffer(0, IID_PPV_ARGS(&rt->texture.resource)));
  rt->texture.resource->GetDesc(&rt->texture.desc);

  D3D11_RENDER_TARGET_VIEW_DESC rtViewDesc;
  ZeroMemory(&rtViewDesc, sizeof(rtViewDesc));
  rtViewDesc.Format = rt->texture.desc.Format;
  rtViewDesc.ViewDimension = rt->texture.desc.SampleDesc.Count == 1 ? D3D11_RTV_DIMENSION_TEXTURE2D : D3D11_RTV_DIMENSION_TEXTURE2DMS;
  B_ERR_HR(_device->CreateRenderTargetView(rt->texture.resource, &rtViewDesc, &rt->rtv.resource));
  set_private_data(FROM_HERE, rt->rtv.resource.p);
  rt->rtv.resource->GetDesc(&rt->rtv.desc);

  // depth buffer
  B_ERR_HR(_device->CreateTexture2D(
    &CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_D24_UNORM_S8_UINT, width, height, 1, 1, D3D11_BIND_DEPTH_STENCIL, D3D11_USAGE_DEFAULT, 0, _curSetup.multisampleCount), 
    NULL, &rt->depth_stencil.resource));
  rt->depth_stencil.resource->GetDesc(&rt->depth_stencil.desc);
  set_private_data(FROM_HERE, rt->depth_stencil.resource.p);

  B_ERR_HR(_device->CreateDepthStencilView(rt->depth_stencil.resource, NULL, &rt->dsv.resource));
  set_private_data(FROM_HERE, rt->dsv.resource.p);
  rt->dsv.resource->GetDesc(&rt->dsv.desc);

  _render_targets.set_pair(idx, make_pair("default_rt", rt));
  _default_render_target = GraphicsObjectHandle(GraphicsObjectHandle::kRenderTarget, idx);

  _viewport = CD3D11_VIEWPORT (0.0f, 0.0f, (float)_width, (float)_height);

  //set_default_render_target();

  return true;
}

bool Graphics::create_default_geometry() {

  static const uint16 quadIndices[] = {
    0, 1, 2,
    2, 1, 3
  };

  {
    // fullscreen pos/tex quad
    static const float verts[] = {
      -1, +1, +1, +1, +0, +0,
      +1, +1, +1, +1, +1, +0,
      -1, -1, +1, +1, +0, +1,
      +1, -1, +1, +1, +1, +1
    };


    auto vb = create_buffer(FROM_HERE, D3D11_BIND_VERTEX_BUFFER, sizeof(verts), false, verts, sizeof(Pos4Tex));
    auto ib = create_buffer(FROM_HERE, D3D11_BIND_INDEX_BUFFER, sizeof(quadIndices), false, quadIndices, DXGI_FORMAT_R16_UINT);
    _predefined_geometry.insert(make_pair(kGeomFsQuadPosTex, make_pair(vb, ib)));
  }

  // fullscreen pos quad
  {
    static const float verts[] = {
      -1, +1, +1, 1,
      +1, +1, +1, 1,
      -1, -1, +1, 1,
      +1, -1, +1, 1,
    };

    auto vb = create_buffer(FROM_HERE, D3D11_BIND_VERTEX_BUFFER, sizeof(verts), false, verts, sizeof(XMFLOAT4));
    auto ib = create_buffer(FROM_HERE, D3D11_BIND_INDEX_BUFFER, sizeof(quadIndices), false, quadIndices, DXGI_FORMAT_R16_UINT);
    _predefined_geometry.insert(make_pair(kGeomFsQuadPos, make_pair(vb, ib)));
  }

  return true;
}

bool Graphics::config(HINSTANCE hInstance) {

  _hInstance = hInstance;
  int res = DialogBox(hInstance, MAKEINTRESOURCE(IDD_SETUP_DLG), NULL, dialogWndProc);
  return res == IDOK;
}

void Graphics::present()
{
  const DWORD now = timeGetTime();
  if (_start_fps_time == 0xffffffff) {
    _start_fps_time = now;
  } else if (++_frame_count == 50) {
    _fps = 50.0f * 1000.0f / (timeGetTime() - _start_fps_time);
    _start_fps_time = now;
    _frame_count = 0;
  }
  _swap_chain->Present(_vsync ? 1 : 0,0);
}

/*
void Graphics::resize(int width, int height)
{
  if (!_swap_chain || width == 0 || height == 0)
    return;
  create_back_buffers(width, height);
}
*/

GraphicsObjectHandle Graphics::create_input_layout(const TrackedLocation &loc, const std::vector<D3D11_INPUT_ELEMENT_DESC> &desc, const std::vector<char> &shader_bytecode) {
  const int idx = _input_layouts.find_free_index();
  if (idx != -1 && SUCCEEDED(_device->CreateInputLayout(&desc[0], desc.size(), &shader_bytecode[0], shader_bytecode.size(), &_input_layouts[idx])))
    return GraphicsObjectHandle(GraphicsObjectHandle::kInputLayout, idx);
  return emptyGoh;
}

template<typename T, class Cont>
GraphicsObjectHandle add_shader(const TrackedLocation &loc, int idx, Cont &cont, T *shader, const string &id, GraphicsObjectHandle::Type type) {
  set_private_data(loc, shader);
  SAFE_RELEASE(cont[idx]);
  cont.set_pair(idx, make_pair(id, shader));
  return Graphics::make_goh(type, idx);
}

GraphicsObjectHandle Graphics::create_vertex_shader(const TrackedLocation &loc, const std::vector<char> &shader_bytecode, const string &id) {

  int idx = _vertex_shaders.idx_from_token(id);
  if (idx != -1 || (idx = _vertex_shaders.find_free_index()) != -1) {
    ID3D11VertexShader *vs = nullptr;
    if (SUCCEEDED(_device->CreateVertexShader(&shader_bytecode[0], shader_bytecode.size(), NULL, &vs))) {
      return add_shader(loc, idx, _vertex_shaders, vs, id, GraphicsObjectHandle::kVertexShader);
    }
  }
  return emptyGoh;
}

GraphicsObjectHandle Graphics::create_pixel_shader(const TrackedLocation &loc, const std::vector<char> &shader_bytecode, const string &id) {

  int idx = _pixel_shaders.idx_from_token(id);
  if (idx != -1 || (idx = _pixel_shaders.find_free_index()) != -1) {
    ID3D11PixelShader *ps = nullptr;
    if (SUCCEEDED(_device->CreatePixelShader(&shader_bytecode[0], shader_bytecode.size(), NULL, &ps))) {
      return add_shader(loc, idx, _pixel_shaders, ps, id, GraphicsObjectHandle::kPixelShader);
    }
  }
  return emptyGoh;
}

GraphicsObjectHandle Graphics::create_compute_shader(const TrackedLocation &loc, const std::vector<char> &shader_bytecode, const string &id) {

  int idx = _compute_shaders.idx_from_token(id);
  if (idx != -1 || (idx = _compute_shaders.find_free_index()) != -1) {
    ID3D11ComputeShader *cs = nullptr;
    if (SUCCEEDED(_device->CreateComputeShader(&shader_bytecode[0], shader_bytecode.size(), NULL, &cs))) {
      return add_shader(loc, idx, _compute_shaders, cs, id, GraphicsObjectHandle::kComputeShader);
    }
  }
  return emptyGoh;
}

GraphicsObjectHandle Graphics::create_geometry_shader(const TrackedLocation &loc, const std::vector<char> &shader_bytecode, const string &id) {

  int idx = _geometry_shaders.idx_from_token(id);
  if (idx != -1 || (idx = _geometry_shaders.find_free_index()) != -1) {
    ID3D11GeometryShader *cs = nullptr;
    if (SUCCEEDED(_device->CreateGeometryShader(&shader_bytecode[0], shader_bytecode.size(), NULL, &cs))) {
      return add_shader(loc, idx, _geometry_shaders, cs, id, GraphicsObjectHandle::kGeometryShader);
    }
  }
  return emptyGoh;
}

template<typename T, class Cont>
int find_existing(const T &desc, Cont &c) {
  // look for an existing state
  T d2;
  for (int i = 0; i < c.Size; ++i) {
    if (!c[i])
      continue;
    c[i]->GetDesc(&d2);
    if (0 == memcmp(&d2, &desc, sizeof(T)))
      return i;
  }
  return -1;
}

GraphicsObjectHandle Graphics::create_rasterizer_state(const TrackedLocation &loc, const D3D11_RASTERIZER_DESC &desc, const char *name) {

  int idx = name ? _rasterizer_states.idx_from_token(name) : find_existing(desc, _rasterizer_states);
  if (idx != -1)
    return make_goh(GraphicsObjectHandle::kRasterizerState, idx);

  idx = _rasterizer_states.find_free_index();
  ID3D11RasterizerState *rs;
  if (idx != -1 && SUCCEEDED(_device->CreateRasterizerState(&desc, &rs))) {
    _rasterizer_states.set_pair(idx, make_pair(name ? name : "", rs));
    return make_goh(GraphicsObjectHandle::kRasterizerState, idx);
  }
  return emptyGoh;
}

GraphicsObjectHandle Graphics::create_blend_state(const TrackedLocation &loc, const D3D11_BLEND_DESC &desc, const char *name) {

  int idx = name ? _blend_states.idx_from_token(name) : find_existing(desc, _blend_states);
  if (idx != -1)
    return make_goh(GraphicsObjectHandle::kBlendState, idx);

  idx = _blend_states.find_free_index();
  ID3D11BlendState *bs;
  if (idx != -1 && SUCCEEDED(_device->CreateBlendState(&desc, &bs))) {
    _blend_states.set_pair(idx, make_pair(name ? name : "", bs));
    return make_goh(GraphicsObjectHandle::kBlendState, idx);
  }
  return emptyGoh;
}

GraphicsObjectHandle Graphics::create_depth_stencil_state(const TrackedLocation &loc, const D3D11_DEPTH_STENCIL_DESC &desc, const char *name) {

  int idx = name ? _depth_stencil_states.idx_from_token(name) : find_existing(desc, _depth_stencil_states);
  if (idx != -1)
    return make_goh(GraphicsObjectHandle::kDepthStencilState, idx);

  idx = _depth_stencil_states.find_free_index();
  ID3D11DepthStencilState *dss;
  if (idx != -1 && SUCCEEDED(_device->CreateDepthStencilState(&desc, &dss))) {
    _depth_stencil_states.set_pair(idx, make_pair(name ? name : "", dss));
    return make_goh(GraphicsObjectHandle::kDepthStencilState, idx);
  }
  return emptyGoh;
}

GraphicsObjectHandle Graphics::create_sampler_state(const TrackedLocation &loc, const D3D11_SAMPLER_DESC &desc, const char *name) {

  int idx = name ? _sampler_states.idx_from_token(name) : find_existing(desc, _sampler_states);
  if (idx != -1)
    return make_goh(GraphicsObjectHandle::kSamplerState, idx);

  idx = _sampler_states.find_free_index();
  ID3D11SamplerState *ss;
  if (idx != -1 && SUCCEEDED(_device->CreateSamplerState(&desc, &ss))) {
    _sampler_states.set_pair(idx, make_pair(name ? name : "", ss));
    return make_goh(GraphicsObjectHandle::kSamplerState, idx);
  }
  return emptyGoh;
}

bool Graphics::technique_file_changed(const char *filename, void *token) {
  return true;
}

bool Graphics::shader_file_changed(const char *filename, void *token) {
  Technique *t = (Technique *)token;
  return t->init();
}

bool Graphics::load_techniques(const char *filename, bool add_materials) {

  LOG_CONTEXT("%s loading: %s", __FUNCTION__, filename);

  bool res = true;

  TechniqueFile result;
  TechniqueParser parser(filename, &result);
  B_ERR_BOOL(parser.parse());

  vector<Material *> &materials = result.materials;
  vector<Technique *> &tmp = result.techniques;

  if (add_materials) {
    for (auto it = begin(materials); it != end(materials); ++it)
      MATERIAL_MANAGER.add_material(*it, true);
  }

  auto &shader_changed = bind(&Graphics::shader_file_changed, this, _1, _2);

  // Init the techniques
  while (!tmp.empty()) {
    unique_ptr<Technique> t(tmp.back());
    tmp.pop_back();

    if (!t->init()) {
      LOG_ERROR_LN("init failed for technique: %s (%s). Error msg: %s", t->name().c_str(), filename, t->error_msg().c_str());
      continue;
    }

#if WITH_UNPACKED_RESOUCES
    // Add file watches on the shader files
    set<string> shaderFiles;
    if (Shader *vs = t->vertex_shader(0)) shaderFiles.insert(vs->source_filename());
    if (Shader *gs = t->geometry_shader(0)) shaderFiles.insert(gs->source_filename());
    if (Shader *ps = t->pixel_shader(0)) shaderFiles.insert(ps->source_filename());
    if (Shader *cs = t->compute_shader(0)) shaderFiles.insert(cs->source_filename());

    for (auto it = begin(shaderFiles); it != end(shaderFiles); ++it) 
      RESOURCE_MANAGER.add_file_watch(it->c_str(), t.get(), shader_changed, false, nullptr, -1);
#endif

    int idx = _techniques.find_free_index(t->name());
    SAFE_DELETE(_techniques[idx]);
    auto tt = t.release();
    _techniques.set_pair(idx, make_pair(tt->name(), tt));
  }

#if WITH_UNPACKED_RESOUCES
  RESOURCE_MANAGER.add_file_watch(filename, NULL, bind(&Graphics::technique_file_changed, this, _1, _2), false, nullptr, -1);
#endif

  return res;
}

GraphicsObjectHandle Graphics::find_resource(const std::string &name) {

  if (name.empty())
    return _dummy_texture;

  // check textures, then resources, then render targets
  int idx = _textures.idx_from_token(name);
  if (idx != -1)
    return GraphicsObjectHandle(GraphicsObjectHandle::kTexture, idx);
  idx = _resources.idx_from_token(name);
  if (idx != -1)
    return GraphicsObjectHandle(GraphicsObjectHandle::kResource, idx);
  idx = _render_targets.idx_from_token(name);
  return make_goh(GraphicsObjectHandle::kRenderTarget, idx);
}

GraphicsObjectHandle Graphics::find_technique(const std::string &name) {
  return make_goh(GraphicsObjectHandle::kTechnique, _techniques.idx_from_token(name));
}

GraphicsObjectHandle Graphics::find_sampler(const std::string &name) {
  return make_goh(GraphicsObjectHandle::kSamplerState, _sampler_states.idx_from_token(name));
}

GraphicsObjectHandle Graphics::find_blend_state(const std::string &name) {
  return make_goh(GraphicsObjectHandle::kBlendState, _blend_states.idx_from_token(name));
}

GraphicsObjectHandle Graphics::find_rasterizer_state(const std::string &name) {
  return make_goh(GraphicsObjectHandle::kRasterizerState, _rasterizer_states.idx_from_token(name));
}

GraphicsObjectHandle Graphics::find_depth_stencil_state(const std::string &name) {
  return make_goh(GraphicsObjectHandle::kDepthStencilState, _depth_stencil_states.idx_from_token(name));
}

Technique *Graphics::get_technique(GraphicsObjectHandle h) {
  return _techniques.get(h);
}

ID3D11ClassLinkage *Graphics::get_class_linkage() {
  return _class_linkage.p;
}

void Graphics::add_shader_flag(const std::string &flag) {
  auto it = _shader_flags.find(flag);
  if (it == _shader_flags.end()) {
    _shader_flags[flag] = 1 << _shader_flags.size();
  }
}

int Graphics::get_shader_flag(const std::string &flag) {
  auto it = _shader_flags.find(flag);
  return it == _shader_flags.end() ? 0 : it->second;
}

GraphicsObjectHandle Graphics::make_goh(GraphicsObjectHandle::Type type, int idx, int data) {
  KASSERT(idx != -1);
  return idx != -1 ? GraphicsObjectHandle(type, idx, data) : emptyGoh;
}

void Graphics::fill_system_resource_views(const ResourceViewArray &views, TextureArray *out) const {

  for (size_t i = 0; i < views.size(); ++i) {
    if (views[i].used && views[i].source == PropertySource::kSystem) {
      (*out)[i] = PROPERTY_MANAGER.get_property<GraphicsObjectHandle>(views[i].class_id);
    }
  }
}

void Graphics::destroy_deferred_context(DeferredContext *ctx) {
  if (ctx) {
    if (!ctx->_is_immediate_context)
      ctx->_ctx->Release();
    delete exch_null(ctx);
  }
}

DeferredContext *Graphics::create_deferred_context(bool can_use_immediate) {
  DeferredContext *dc = new DeferredContext;
  if (can_use_immediate) {
    dc->_is_immediate_context = true;
    dc->_ctx = _immediate_context;
  } else {
    _device->CreateDeferredContext(0, &dc->_ctx);
  }

  return dc;
}

void Graphics::get_predefined_geometry(PredefinedGeometry geom, GraphicsObjectHandle *vb, int *vertex_size, GraphicsObjectHandle *ib, 
                                       DXGI_FORMAT *index_format, int *index_count) {

  *index_format = DXGI_FORMAT_R16_UINT;
  KASSERT(_predefined_geometry.find(geom) != _predefined_geometry.end());

  switch (geom) {
    case kGeomFsQuadPos:
      *vb = _predefined_geometry[kGeomFsQuadPos].first;
      *ib = _predefined_geometry[kGeomFsQuadPos].second;
      *vertex_size = sizeof(XMFLOAT4);
      *index_count = 6;
      break;

    case kGeomFsQuadPosTex:
      *vb = _predefined_geometry[kGeomFsQuadPosTex].first;
      *ib = _predefined_geometry[kGeomFsQuadPosTex].second;
      *vertex_size = sizeof(Pos4Tex);
      *index_count = 6;
      break;
  }
}

void Graphics::add_command_list(ID3D11CommandList *cmd_list) {
  _immediate_context->ExecuteCommandList(cmd_list, FALSE);
  cmd_list->Release();
}

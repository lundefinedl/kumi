#include "stdafx.h"
#include "kumi_gwen.hpp"
#include "gwen/Structures.h"
#include "gwen/Font.h"
#include "gwen/Texture.h"
#include "dynamic_vb.hpp"
#include "vertex_types.hpp"
#include "renderer.hpp"
#include "string_utils.hpp"
#include <D3DX11tex.h>

#pragma comment(lib, "D3DX11.lib")

struct MappedTexture {
  MappedTexture() : data(nullptr) {}
  ~MappedTexture() {
    SAFE_ADELETE(data);
  }
  D3DX11_IMAGE_INFO image_info;
  uint32 pitch;
  uint8 *data;
};

struct KumiGwenRenderer : public Gwen::Renderer::Base
{
  KumiGwenRenderer()
    : _locked_pos_col(nullptr)
  {
    _vb_pos_col.create(64 * 1024);
    _render_key.cmd = RenderKey::kRenderBuffers;
  }

  virtual void Init() {
  };

  virtual void Begin() {
    _locked_pos_col = _vb_pos_col.map();
  };

  virtual void End() {
    int pos_col_count = _vb_pos_col.unmap(exch_null(_locked_pos_col));

    BufferRenderData *data = RENDERER.alloc_command_data<BufferRenderData>();
    data->topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    data->vb = _vb_pos_col.vb();
    data->vertex_size = _vb_pos_col.stride;
    data->vs = GRAPHICS.find_shader("gwen", "vs_color_main");
    data->ps = GRAPHICS.find_shader("gwen", "ps_color_main");
    data->layout = GRAPHICS.get_input_layout("gwen");
    data->vertex_count = pos_col_count;
    GRAPHICS.get_technique_states("gwen", &data->rs, &data->bs, &data->dss);
    RENDERER.submit_command(FROM_HERE, _render_key, data);
  };

  static XMFLOAT4 to_directx(const Gwen::Color &col) {
    return XMFLOAT4(float(col.r) / 255, float(col.g) / 255, float(col.b) / 255, float(col.a) / 255);
  }

  static uint32 to_abgr(const XMFLOAT4 &col) {
    return ((uint32(255 * col.w)) << 24) | ((uint32(255 * col.z)) << 16) | ((uint32(255 * col.y)) << 8) | (uint32(255 * col.x));
  }

  virtual void SetDrawColor(Gwen::Color color) {
    _draw_color = to_directx(color);
  };

  virtual void DrawFilledRect(Gwen::Rect rect){
    // (0,0) ---> (w,0)
    //  |
    // (0,h)

    // 0,1
    // 2,3
    // 0, 1, 2
    // 2, 1, 3

    Translate(rect);

    float x0, y0, x1, y1, x2, y2, x3, y3;
    screen_to_clip(rect.x, rect.y, GRAPHICS.width(), GRAPHICS.height(), &x0, &y0);
    screen_to_clip(rect.x + rect.w, rect.y, GRAPHICS.width(), GRAPHICS.height(), &x1, &y1);
    screen_to_clip(rect.x, rect.y + rect.h, GRAPHICS.width(), GRAPHICS.height(), &x2, &y2);
    screen_to_clip(rect.x + rect.w, rect.y + rect.h, GRAPHICS.width(), GRAPHICS.height(), &x3, &y3);

    PosCol v0(x0, y0, 0, _draw_color);
    PosCol v1(x1, y1, 0, _draw_color);
    PosCol v2(x2, y2, 0, _draw_color);
    PosCol v3(x3, y3, 0, _draw_color);

    *_locked_pos_col++ = v0;
    *_locked_pos_col++ = v1;
    *_locked_pos_col++ = v2;

    *_locked_pos_col++ = v2;
    *_locked_pos_col++ = v1;
    *_locked_pos_col++ = v3;
  };

  virtual void StartClip() {
    const Gwen::Rect& rect = ClipRegion();
  };

  virtual void EndClip() {

  };

  virtual void LoadTexture( Gwen::Texture* pTexture ) {

    D3DX11_IMAGE_INFO image_info;
    HRESULT hr;
    D3DX11GetImageInfoFromFile(pTexture->name.c_str(), NULL, &image_info, &hr);
    if (FAILED(hr)) {
      pTexture->failed = true;
      return;
    }

    D3DX11_IMAGE_LOAD_INFO loadinfo;
    ZeroMemory(&loadinfo, sizeof(D3DX11_IMAGE_LOAD_INFO));
    loadinfo.CpuAccessFlags = D3D11_CPU_ACCESS_READ;
    loadinfo.Usage = D3D11_USAGE_STAGING;
    loadinfo.Format = image_info.Format;
    CComPtr<ID3D11Resource> resource;
    D3DX11CreateTextureFromFile(GRAPHICS.device(), pTexture->name.c_str(), &loadinfo, NULL, &resource.p, &hr);
    if (FAILED(hr)) {
      pTexture->failed = true;
      return;
    }

    D3D11_MAPPED_SUBRESOURCE sub;
    if (FAILED(GRAPHICS.context()->Map(resource, 0, D3D11_MAP_READ, 0, &sub))) {
      pTexture->failed = true;
      return;
    }

    MappedTexture &texture = _mapped_textures[pTexture->name.c_str()];
    texture.image_info = image_info;
    uint32 pitch = texture.pitch = sub.RowPitch;
    uint8 *src = (uint8 *)sub.pData;
    uint8 *dst = texture.data = new uint8[pitch * image_info.Height];
    
    int row_size = 4 * image_info.Width;
    for (uint32 i = 0; i < image_info.Height; ++i) {
      memcpy(&dst[i*pitch], &src[i*pitch], row_size);
    }

    pTexture->failed = false;

    GRAPHICS.context()->Unmap(resource, 0);
  };

  virtual void FreeTexture( Gwen::Texture* pTexture ){
  };

  virtual void DrawTexturedRect( Gwen::Texture* pTexture, Gwen::Rect pTargetRect, float u1=0.0f, float v1=0.0f, float u2=1.0f, float v2=1.0f ) {
    DrawFilledRect(pTargetRect);
  };

  virtual void DrawMissingImage( Gwen::Rect pTargetRect ) {

  }

  virtual Gwen::Color PixelColour(Gwen::Texture* pTexture, unsigned int x, unsigned int y, const Gwen::Color& col_default = Gwen::Color( 255, 255, 255, 255 ) ) { 
    auto it = _mapped_textures.find(pTexture->name);
    if (it == _mapped_textures.end()) {
      LOG_WARNING_LN("Texture not loaded: %s", pTexture->name.c_str());
      return col_default; 
    }
    const MappedTexture &texture = it->second;
    uint8 *pixel = &texture.data[y*texture.pitch+x*4];
    return Gwen::Color(pixel[0], pixel[1], pixel[2], pixel[3]);
  }

  virtual Gwen::Renderer::ICacheToTexture* GetCTT() { return NULL; }

  virtual void LoadFont( Gwen::Font* pFont ){
  };

  virtual void FreeFont( Gwen::Font* pFont ){
  };

  virtual void RenderText( Gwen::Font* pFont, Gwen::Point pos, const Gwen::UnicodeString& text ) {
    Translate(pos.x, pos.y);
    RenderKey key;
    key.cmd = RenderKey::kRenderText;
    TextRenderData *data = RENDERER.alloc_command_data<TextRenderData>();
    data->font = get_font(pFont->facename);
    int len = (text.size() + 1) * 2;
    data->str = (WCHAR *)RENDERER.alloc_command_data(len);
    memcpy((void *)data->str, text.c_str(), len);
    data->font_size = pFont->size;
    data->x = pos.x;
    data->y = pos.y;
    data->color = to_abgr(_draw_color);
    data->flags = 0;

    RENDERER.submit_command(FROM_HERE, key, data);
  }

  virtual Gwen::Point MeasureText(Gwen::Font* pFont, const Gwen::UnicodeString& text) {
    FW1_RECTF rect;
    GRAPHICS.measure_text(get_font(pFont->facename), pFont->facename, text, pFont->size, 0, &rect);
    return Gwen::Point(rect.Right - rect.Left, rect.Bottom - rect.Top);
  }

  GraphicsObjectHandle get_font(const std::wstring &font_name) {
    auto it = _fonts.find(font_name);
    if (it == _fonts.end()) {
      GraphicsObjectHandle font = GRAPHICS.get_or_create_font_family(font_name);
      _fonts[font_name] = font;
      return font;
    }
    return it->second;
  }

  //
  // No need to implement these functions in your derived class, but if 
  // you can do them faster than the default implementation it's a good idea to.
  //
/*
  virtual void DrawLinedRect( Gwen::Rect rect ) {
  }
  virtual void DrawPixel( int x, int y );
  virtual void DrawShavedCornerRect( Gwen::Rect rect, bool bSlight = false );
*/

  std::map<string, MappedTexture> _mapped_textures;
  std::map<wstring, GraphicsObjectHandle> _fonts;
  XMFLOAT4 _draw_color;
  DynamicVb<PosCol> _vb_pos_col;
  PosCol *_locked_pos_col;
  RenderKey _render_key;
};


Gwen::Renderer::Base* create_kumi_gwen_renderer() {
  return new KumiGwenRenderer();
}

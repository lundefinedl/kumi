#include "stdafx.h"
#include "kumi_gwen.hpp"
#include "gwen/Structures.h"
#include "dynamic_vb.hpp"
#include "vertex_types.hpp"
#include "renderer.hpp"

struct KumiGwenRenderer : public Gwen::Renderer::Base
{
  KumiGwenRenderer() 
    : _locked_pos_col(nullptr) {
      _vb_pos_col.create(64 * 1024);
  }

  virtual void Init() {
  };

  virtual void Begin() {
    _locked_pos_col = _vb_pos_col.map();
  };

  virtual void End() {
    int pos_col_count = _vb_pos_col.unmap(exch_null(_locked_pos_col));

    BufferRenderData *data = RENDERER.alloc_command_data<BufferRenderData>();
    RenderKey key;
    key.cmd = RenderKey::kRenderBuffers;
    RENDERER.submit_command(FROM_HERE, key, data);
  };

  virtual void SetDrawColor(Gwen::Color color) {
    _draw_color = color;
  };

  virtual void DrawFilledRect(Gwen::Rect rect){
    // (0,0) ---> (w,0)
    //  |
    // (0,h)

    // 0,1
    // 2,3
    // 0, 1, 2
    // 2, 1, 3
    PosCol v0(rect.x, rect.y, 0, _draw_color.r, _draw_color.g, _draw_color.b, _draw_color.a);
    PosCol v1(rect.x + rect.w, rect.y, 0, _draw_color.r, _draw_color.g, _draw_color.b, _draw_color.a);
    PosCol v2(rect.x, rect.y + rect.h, 0, _draw_color.r, _draw_color.g, _draw_color.b, _draw_color.a);
    PosCol v3(rect.x + rect.w, rect.y + rect.h, 0, _draw_color.r, _draw_color.g, _draw_color.b, _draw_color.a);

    *_locked_pos_col++ = v0;
    *_locked_pos_col++ = v1;
    *_locked_pos_col++ = v2;

    *_locked_pos_col++ = v2;
    *_locked_pos_col++ = v1;
    *_locked_pos_col++ = v3;
  };

  virtual void StartClip() {

  };

  virtual void EndClip() {

  };

  virtual void LoadTexture( Gwen::Texture* pTexture ) {

  };

  virtual void FreeTexture( Gwen::Texture* pTexture ){
  };

  virtual void DrawTexturedRect( Gwen::Texture* pTexture, Gwen::Rect pTargetRect, float u1=0.0f, float v1=0.0f, float u2=1.0f, float v2=1.0f ){};
  virtual void DrawMissingImage( Gwen::Rect pTargetRect ) {

  }
  virtual Gwen::Color PixelColour( Gwen::Texture* pTexture, unsigned int x, unsigned int y, const Gwen::Color& col_default = Gwen::Color( 255, 255, 255, 255 ) ){ return col_default; }

  virtual Gwen::Renderer::ICacheToTexture* GetCTT() { return NULL; }

  virtual void LoadFont( Gwen::Font* pFont ){

  };

  virtual void FreeFont( Gwen::Font* pFont ){
  };

  virtual void RenderText( Gwen::Font* pFont, Gwen::Point pos, const Gwen::UnicodeString& text ) {

  }

  virtual Gwen::Point MeasureText(Gwen::Font* pFont, const Gwen::UnicodeString& text) {
    return Gwen::Point();
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
  virtual Gwen::Point MeasureText( Gwen::Font* pFont, const Gwen::String& text );
  virtual void RenderText( Gwen::Font* pFont, Gwen::Point pos, const Gwen::String& text );
*/

  Gwen::Color _draw_color;
  DynamicVb<PosCol> _vb_pos_col;
  PosCol *_locked_pos_col;
};


Gwen::Renderer::Base* create_kumi_gwen_renderer() {
  return new KumiGwenRenderer();
}

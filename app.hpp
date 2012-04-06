#ifndef _APP_HPP_
#define _APP_HPP_

#include "graphics.hpp"
#include "threading.hpp"

using std::string;
using std::map;
using std::vector;

/*
repeat after me: directx is left-handed. z goes into the screen.
*/

class EffectBase;
class EffectWrapper;

struct Scene;

struct RenderStates {
  RenderStates() : blend_state(nullptr), depth_stencil_state(nullptr), 
                   rasterizer_state(nullptr), sampler_state(nullptr) {}
  D3D11_BLEND_DESC blend_desc;
  ID3D11BlendState *blend_state;

  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
  ID3D11DepthStencilState *depth_stencil_state;

  D3D11_RASTERIZER_DESC rasterizer_desc;
  ID3D11RasterizerState *rasterizer_state;

  D3D11_SAMPLER_DESC sampler_desc;
  ID3D11SamplerState *sampler_state;
};

namespace Gwen {
  namespace Renderer {
    class Base;
  }
  namespace Controls {
    class Canvas;
  }
  namespace Skin {
    class TexturedBase;
  }
  namespace Input {
    class Windows;
  }
}

class App : 
#if USE_CEF 
  public CefClient, public CefLifeSpanHandler, public CefLoadHandler, public CefRenderHandler, 
#endif
  public threading::GreedyThread
{
public:

  static App& instance();

  bool init(HINSTANCE hinstance);
  static bool close();

  void	tick();

  virtual UINT run(void *userdata);
  void on_idle();

  void debug_text(const char *fmt, ...);

private:
  DISALLOW_COPY_AND_ASSIGN(App);
  App();
  ~App();

  bool create_window();
  void set_client_size();
  void find_app_root();

  static LRESULT CALLBACK wnd_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
  //static LRESULT CALLBACK tramp_wnd_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

#if USE_CEF
  // CefClient methods
  virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() OVERRIDE { return this; }
  virtual CefRefPtr<CefLoadHandler> GetLoadHandler() OVERRIDE { return this; }
  virtual CefRefPtr<CefRenderHandler> GetRenderHandler() OVERRIDE { return this; }

  // CefLifeSpanHandler methods
  virtual bool OnBeforePopup(CefRefPtr<CefBrowser> parentBrowser, const CefPopupFeatures& popupFeatures, 
                             CefWindowInfo& windowInfo, const CefString& url, CefRefPtr<CefClient>& client, 
                             CefBrowserSettings& settings) OVERRIDE;
  virtual void OnAfterCreated(CefRefPtr<CefBrowser> browser) OVERRIDE;
  virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) OVERRIDE;

  // CefLoadHandler methods
  virtual void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) OVERRIDE;
  virtual void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) OVERRIDE;
  virtual bool OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, 
                           const CefString& failedUrl, CefString& errorText) OVERRIDE;

  // CefRenderHandler methods
  virtual void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const CefRect& dirtyRect, 
                       const void* buffer);
  virtual bool GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect);
  virtual bool GetScreenRect(CefRefPtr<CefBrowser> browser, CefRect& rect);
  virtual bool GetScreenPoint(CefRefPtr<CefBrowser> browser, int viewX, int viewY, int& screenX, int& screenY);

  // CefJSBindingHandler
  //virtual void OnJSBinding(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Value> object);

  virtual void OnCursorChange(CefRefPtr<CefBrowser> browser, CefCursorHandle cursor) OVERRIDE;

  CefRefPtr<CefBrowser> GetBrowser() { return m_Browser; }
  CefWindowHandle GetBrowserHwnd() { return _browser_hwnd; }

  std::string GetLogFile();

  void SetLastDownloadFile(const std::string& fileName);
  std::string GetLastDownloadFile();

  // DOM visitors will be called after the associated path is loaded.
  void AddDOMVisitor(const std::string& path, CefRefPtr<CefDOMVisitor> visitor);
  CefRefPtr<CefDOMVisitor> GetDOMVisitor(const std::string& path);

  // Send a notification to the application. Notifications should not block the
  // caller.
  enum NotificationType {
    NOTIFY_CONSOLE_MESSAGE,
    NOTIFY_DOWNLOAD_COMPLETE,
    NOTIFY_DOWNLOAD_ERROR,
  };
  void SendNotification(NotificationType type);
#endif
protected:
#if USE_CEF
  void SetLoading(bool isLoading);
  void SetNavState(bool canGoBack, bool canGoForward);

  // The child browser window
  CefRefPtr<CefBrowser> m_Browser;

  // The main frame window handle
  CefWindowHandle _main_hwnd;

  // The child browser window handle
  CefWindowHandle _browser_hwnd;

  // The edit window handle
  CefWindowHandle _edit_hwnd;

  // The button window handles
  CefWindowHandle _back_hwnd;
  CefWindowHandle _forward_hwnd;
  CefWindowHandle _stop_hwnd;
  CefWindowHandle _reload_hwnd;

  // Support for logging.
  std::string m_LogFile;

  // Support for downloading files.
  std::string m_LastDownloadFile;

  // Support for DOM visitors.
  typedef std::map<std::string, CefRefPtr<CefDOMVisitor> > DOMVisitorMap;
  DOMVisitorMap m_DOMVisitors;

  // Include the default reference counting implementation.
  IMPLEMENT_REFCOUNTING(App);
  // Include the default locking implementation.
  IMPLEMENT_LOCKING(App);
#endif
  static App* _instance;
  EffectBase* _test_effect;
  HINSTANCE _hinstance;
  int32_t _width;
  int32_t _height;
  HWND _hwnd;
  int _dbg_message_count;

  int _cur_camera;
  bool _draw_plane;
  string _app_root;
  int _ref_count;
#if USE_CEF
  GraphicsObjectHandle _cef_texture;
  GraphicsObjectHandle _cef_staging;
#endif

  std::unique_ptr<Gwen::Controls::Canvas> _gwen_canvas;
  std::unique_ptr<Gwen::Renderer::Base> _gwen_renderer;
  std::unique_ptr<Gwen::Skin::TexturedBase> _gwen_skin;
  std::unique_ptr<Gwen::Input::Windows> _gwen_input;
};

#define APP App::instance()

#endif

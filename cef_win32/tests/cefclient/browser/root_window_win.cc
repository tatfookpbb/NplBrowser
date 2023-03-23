// Copyright (c) 2015 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/root_window_win.h"

#include <shellscalingapi.h>

#include "include/base/cef_bind.h"
#include "include/base/cef_build.h"
#include "include/cef_app.h"
#include "tests/cefclient/browser/browser_window_osr_win.h"
#include "tests/cefclient/browser/browser_window_std_win.h"
#include "tests/cefclient/browser/main_context.h"
#include "tests/cefclient/browser/resource.h"
#include "tests/cefclient/browser/temp_window.h"
#include "tests/cefclient/browser/window_test_runner_win.h"
#include "tests/shared/browser/geometry_util.h"
#include "tests/shared/browser/main_message_loop.h"
#include "tests/shared/browser/util_win.h"
#include "tests/shared/common/client_switches.h"
#include "tests/cefclient/browser/root_window_manager.h"
#include "json.hpp"
#include "tests/cefclient/CefClientConfig.h"
#include "tests/cefclient/cefclient_to_npl.h"
#define MAX_URL_LENGTH 255
#define BUTTON_WIDTH 72
#define URLBAR_HEIGHT 24
static bool bFirstInit = false;
namespace client {

namespace {

// Message handler for the About box.
INT_PTR CALLBACK AboutWndProc(HWND hDlg,
                              UINT message,
                              WPARAM wParam,
                              LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  switch (message) {
    case WM_INITDIALOG:
      return TRUE;

    case WM_COMMAND:
      if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
        EndDialog(hDlg, LOWORD(wParam));
        return TRUE;
      }
      break;
  }
  return FALSE;
}

// Returns true if the process is per monitor DPI aware.
bool IsProcessPerMonitorDpiAware() {
  enum class PerMonitorDpiAware {
    UNKNOWN = 0,
    PER_MONITOR_DPI_UNAWARE,
    PER_MONITOR_DPI_AWARE,
  };
  static PerMonitorDpiAware per_monitor_dpi_aware = PerMonitorDpiAware::UNKNOWN;
  if (per_monitor_dpi_aware == PerMonitorDpiAware::UNKNOWN) {
    per_monitor_dpi_aware = PerMonitorDpiAware::PER_MONITOR_DPI_UNAWARE;
    HMODULE shcore_dll = ::LoadLibrary(L"shcore.dll");
    if (shcore_dll) {
      typedef HRESULT(WINAPI * GetProcessDpiAwarenessPtr)(
          HANDLE, PROCESS_DPI_AWARENESS*);
      GetProcessDpiAwarenessPtr func_ptr =
          reinterpret_cast<GetProcessDpiAwarenessPtr>(
              ::GetProcAddress(shcore_dll, "GetProcessDpiAwareness"));
      if (func_ptr) {
        PROCESS_DPI_AWARENESS awareness;
        if (SUCCEEDED(func_ptr(nullptr, &awareness)) &&
            awareness == PROCESS_PER_MONITOR_DPI_AWARE)
          per_monitor_dpi_aware = PerMonitorDpiAware::PER_MONITOR_DPI_AWARE;
      }
    }
  }
  return per_monitor_dpi_aware == PerMonitorDpiAware::PER_MONITOR_DPI_AWARE;
}

// DPI value for 1x scale factor.
#define DPI_1X 96.0f

float GetWindowScaleFactor(HWND hwnd) {
  if (hwnd && IsProcessPerMonitorDpiAware()) {
    typedef UINT(WINAPI * GetDpiForWindowPtr)(HWND);
    static GetDpiForWindowPtr func_ptr = reinterpret_cast<GetDpiForWindowPtr>(
        GetProcAddress(GetModuleHandle(L"user32.dll"), "GetDpiForWindow"));
    if (func_ptr)
      return static_cast<float>(func_ptr(hwnd)) / DPI_1X;
  }

  return client::GetDeviceScaleFactor();
}

int GetButtonWidth(HWND hwnd) {
  return LogicalToDevice(BUTTON_WIDTH, GetWindowScaleFactor(hwnd));
}

int GetURLBarHeight(HWND hwnd) {
  return LogicalToDevice(URLBAR_HEIGHT, GetWindowScaleFactor(hwnd));
}

}  // namespace

RootWindowWin::RootWindowWin()
    : with_controls_(false),
      always_on_top_(false),
      with_osr_(false),
      with_extension_(false),
      is_popup_(false),
      start_rect_(),
      initialized_(false),
      hwnd_(NULL),
      draggable_region_(NULL),
      font_(NULL),
      font_height_(0),
      back_hwnd_(NULL),
      forward_hwnd_(NULL),
      reload_hwnd_(NULL),
      stop_hwnd_(NULL),
      edit_hwnd_(NULL),
      edit_wndproc_old_(NULL),
      find_hwnd_(NULL),
      find_message_id_(0),
      find_wndproc_old_(NULL),
      find_state_(),
      find_next_(false),
      find_match_case_last_(false),
      window_destroyed_(false),
      browser_destroyed_(false),
      called_enable_non_client_dpi_scaling_(false) {
  find_buff_[0] = 0;

  // Create a HRGN representing the draggable window area.
  draggable_region_ = ::CreateRectRgn(0, 0, 0, 0);
  bFirstInit = false;
}

RootWindowWin::~RootWindowWin() {
  REQUIRE_MAIN_THREAD();

  ::DeleteObject(draggable_region_);
  ::DeleteObject(font_);
  bFirstInit = false;
  // The window and browser should already have been destroyed.
  DCHECK(window_destroyed_);
  DCHECK(browser_destroyed_);
}

void RootWindowWin::Init(RootWindow::Delegate* delegate,
                         const RootWindowConfig& config,
                         const CefBrowserSettings& settings) {
  DCHECK(delegate);
  DCHECK(!initialized_);

  delegate_ = delegate;
  with_controls_ = config.with_controls;
  always_on_top_ = config.always_on_top;
  with_osr_ = config.with_osr;
  with_extension_ = config.with_extension;

  start_rect_.left = config.bounds.x;
  start_rect_.top = config.bounds.y;
  start_rect_.right = config.bounds.x + config.bounds.width;
  start_rect_.bottom = config.bounds.y + config.bounds.height;

  CreateBrowserWindow(config.url);

  initialized_ = true;
  bFirstInit = false;
  // Create the native root window on the main thread.
  if (CURRENTLY_ON_MAIN_THREAD()) {
    CreateRootWindow(settings, config.initially_hidden);
  } else {
    MAIN_POST_CLOSURE(base::Bind(&RootWindowWin::CreateRootWindow, this,
                                 settings, config.initially_hidden));
  }
}

void RootWindowWin::InitAsPopup(RootWindow::Delegate* delegate,
                                bool with_controls,
                                bool with_osr,
                                const CefPopupFeatures& popupFeatures,
                                CefWindowInfo& windowInfo,
                                CefRefPtr<CefClient>& client,
                                CefBrowserSettings& settings) {
  CEF_REQUIRE_UI_THREAD();

  DCHECK(delegate);
  DCHECK(!initialized_);

  delegate_ = delegate;
  with_controls_ = with_controls;
  with_osr_ = with_osr;
  is_popup_ = true;

  if (popupFeatures.xSet)
    start_rect_.left = popupFeatures.x;
  if (popupFeatures.ySet)
    start_rect_.top = popupFeatures.y;
  if (popupFeatures.widthSet)
    start_rect_.right = start_rect_.left + popupFeatures.width;
  if (popupFeatures.heightSet)
    start_rect_.bottom = start_rect_.top + popupFeatures.height;

  CreateBrowserWindow(std::string());

  initialized_ = true;

  // The new popup is initially parented to a temporary window. The native root
  // window will be created after the browser is created and the popup window
  // will be re-parented to it at that time.
  browser_window_->GetPopupConfig(TempWindow::GetWindowHandle(), windowInfo,
                                  client, settings);
}

void RootWindowWin::Show(ShowMode mode) {
  REQUIRE_MAIN_THREAD();

  if (!hwnd_)
    return;

  int nCmdShow = SW_SHOWNORMAL;
  switch (mode) {
    case ShowMinimized:
      nCmdShow = SW_SHOWMINIMIZED;
      break;
    case ShowMaximized:
      nCmdShow = SW_SHOWMAXIMIZED;
      break;
    case ShowNoActivate:
      nCmdShow = SW_SHOWNOACTIVATE;
      break;
    default:
      break;
  }

  ShowWindow(hwnd_, nCmdShow);
  UpdateWindow(hwnd_);
}

void RootWindowWin::Hide() {
  REQUIRE_MAIN_THREAD();

  if (hwnd_)
    ShowWindow(hwnd_, SW_HIDE);
}

void RootWindowWin::SetBounds(int x, int y, size_t width, size_t height) {
  REQUIRE_MAIN_THREAD();

  if (hwnd_) {
    SetWindowPos(hwnd_, NULL, x, y, static_cast<int>(width),
                 static_cast<int>(height), SWP_NOZORDER);
  }
}

void RootWindowWin::Close(bool force) {
  REQUIRE_MAIN_THREAD();

  if (hwnd_) {
    if (force)
      DestroyWindow(hwnd_);
    else
      PostMessage(hwnd_, WM_CLOSE, 0, 0);
  }
}

void RootWindowWin::SetDeviceScaleFactor(float device_scale_factor) {
  REQUIRE_MAIN_THREAD();

  if (browser_window_ && with_osr_)
    browser_window_->SetDeviceScaleFactor(device_scale_factor);
}

float RootWindowWin::GetDeviceScaleFactor() const {
  REQUIRE_MAIN_THREAD();

  if (browser_window_ && with_osr_)
    return browser_window_->GetDeviceScaleFactor();

  NOTREACHED();
  return 0.0f;
}

CefRefPtr<CefBrowser> RootWindowWin::GetBrowser() const {
  REQUIRE_MAIN_THREAD();

  if (browser_window_)
    return browser_window_->GetBrowser();
  return NULL;
}

ClientWindowHandle RootWindowWin::GetWindowHandle() const {
  REQUIRE_MAIN_THREAD();
  return hwnd_;
}

bool RootWindowWin::WithWindowlessRendering() const {
  REQUIRE_MAIN_THREAD();
  return with_osr_;
}

bool RootWindowWin::WithExtension() const {
  REQUIRE_MAIN_THREAD();
  return with_extension_;
}

void RootWindowWin::CreateBrowserWindow(const std::string& startup_url) {
  if (with_osr_) {
    OsrRendererSettings settings = {};
    MainContext::Get()->PopulateOsrSettings(&settings);
    browser_window_.reset(new BrowserWindowOsrWin(this, startup_url, settings));
  } else {
    browser_window_.reset(new BrowserWindowStdWin(this, startup_url));
  }
}

void RootWindowWin::CreateRootWindow(const CefBrowserSettings& settings,
                                     bool initially_hidden) {
  REQUIRE_MAIN_THREAD();
  DCHECK(!hwnd_);

  HINSTANCE hInstance = GetModuleHandle(NULL);

  CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
  CefString window_title_s = command_line->GetSwitchValue("window_title");
  CefString window_name_s = command_line->GetSwitchValue("window_name");
  CefString parent_handle_s = command_line->GetSwitchValue("parent_handle");
  // Load strings from the resource file.
  const std::wstring& window_title = window_title_s.empty() ? GetResourceString(IDS_APP_TITLE) : window_title_s;
  const std::wstring& window_name = window_name_s.empty() ? GetResourceString(IDC_CEFCLIENT) : window_name_s;
  HWND parent_handle = parent_handle_s.empty() ? NULL : (HWND)std::stoull(parent_handle_s.c_str());
  //LOG(ERROR) << "parent_handle_s:" << parent_handle_s;
  //LOG(ERROR) << "parent_handle:" << parent_handle;

  const cef_color_t background_color = MainContext::Get()->GetBackgroundColor();
  const HBRUSH background_brush = CreateSolidBrush(
      RGB(CefColorGetR(background_color), CefColorGetG(background_color),
          CefColorGetB(background_color)));

  // Register the window class.
  RegisterRootClass(hInstance, window_name, background_brush);

  // Register the message used with the find dialog.
  find_message_id_ = RegisterWindowMessage(FINDMSGSTRING);
  CHECK(find_message_id_);

 
  const bool no_activate = command_line->HasSwitch(switches::kNoActivate);


  DWORD dwStyle;
  if (parent_handle)
  {
	  dwStyle = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN;
	  SetWindowLong(parent_handle, GWL_STYLE, GetWindowLong(parent_handle, GWL_STYLE) | WS_CLIPCHILDREN);
  }
  else {
	  dwStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;

  }
  DWORD dwExStyle = always_on_top_ ? WS_EX_TOPMOST : 0;
  if (no_activate) {
    // Don't activate the browser window on creation.
    dwExStyle |= WS_EX_NOACTIVATE;
  }
 
  int x, y, width, height;
  if (::IsRectEmpty(&start_rect_)) {
    // Use the default window position/size.
    x = y = width = height = CW_USEDEFAULT;
  } else {
    // Adjust the window size to account for window frame and controls.
    RECT window_rect = start_rect_;
    ::AdjustWindowRectEx(&window_rect, dwStyle, with_controls_, dwExStyle);

    x = start_rect_.left;
    y = start_rect_.top;
    width = window_rect.right - window_rect.left;
    height = window_rect.bottom - window_rect.top;
  }
  browser_settings_ = settings;
  // Create the main window initially hidden.
  CreateWindowEx(dwExStyle, window_name.c_str(), window_title.c_str(), dwStyle,
                 x, y, width, height, parent_handle, NULL, hInstance, this);
  CHECK(hwnd_);
  if (!called_enable_non_client_dpi_scaling_ && IsProcessPerMonitorDpiAware()) {
    // This call gets Windows to scale the non-client area when WM_DPICHANGED
    // is fired on Windows versions < 10.0.14393.0.
    // Derived signature; not available in headers.
    typedef LRESULT(WINAPI * EnableChildWindowDpiMessagePtr)(HWND, BOOL);
    static EnableChildWindowDpiMessagePtr func_ptr =
        reinterpret_cast<EnableChildWindowDpiMessagePtr>(GetProcAddress(
            GetModuleHandle(L"user32.dll"), "EnableChildWindowDpiMessage"));
    if (func_ptr)
      func_ptr(hwnd_, TRUE);
  }

  if (!initially_hidden) {
    // Show this window.
    Show(no_activate ? ShowNoActivate : ShowNormal);
  }
}

// static
void RootWindowWin::RegisterRootClass(HINSTANCE hInstance,
                                      const std::wstring& window_class,
                                      HBRUSH background_brush) {
  // Only register the class one time.
  static bool class_registered = false;
  if (class_registered)
    return;
  class_registered = true;

  WNDCLASSEX wcex;

  wcex.cbSize = sizeof(WNDCLASSEX);

  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc = RootWndProc;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = hInstance;
  wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CEFCLIENT));
  wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
  wcex.hbrBackground = background_brush;
  wcex.lpszMenuName = MAKEINTRESOURCE(IDC_CEFCLIENT);
  wcex.lpszClassName = window_class.c_str();
  wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

  RegisterClassEx(&wcex);
}

// static
LRESULT CALLBACK RootWindowWin::EditWndProc(HWND hWnd,
                                            UINT message,
                                            WPARAM wParam,
                                            LPARAM lParam) {
  REQUIRE_MAIN_THREAD();

  RootWindowWin* self = GetUserDataPtr<RootWindowWin*>(hWnd);
  DCHECK(self);
  DCHECK(hWnd == self->edit_hwnd_);

  switch (message) {
    case WM_CHAR:
      if (wParam == VK_RETURN) {
        // When the user hits the enter key load the URL.
        CefRefPtr<CefBrowser> browser = self->GetBrowser();
        if (browser) {
          wchar_t strPtr[MAX_URL_LENGTH + 1] = {0};
          *((LPWORD)strPtr) = MAX_URL_LENGTH;
          LRESULT strLen = SendMessage(hWnd, EM_GETLINE, 0, (LPARAM)strPtr);
          if (strLen > 0) {
            strPtr[strLen] = 0;
            browser->GetMainFrame()->LoadURL(strPtr);
          }
        }
        return 0;
      }
      break;
    case WM_NCDESTROY:
      // Clear the reference to |self|.
      SetUserDataPtr(hWnd, NULL);
      self->edit_hwnd_ = NULL;
      break;
  }

  return CallWindowProc(self->edit_wndproc_old_, hWnd, message, wParam, lParam);
}

// static
LRESULT CALLBACK RootWindowWin::FindWndProc(HWND hWnd,
                                            UINT message,
                                            WPARAM wParam,
                                            LPARAM lParam) {
  REQUIRE_MAIN_THREAD();

  RootWindowWin* self = GetUserDataPtr<RootWindowWin*>(hWnd);
  DCHECK(self);
  DCHECK(hWnd == self->find_hwnd_);

  switch (message) {
    case WM_ACTIVATE:
      // Set this dialog as current when activated.
      MainMessageLoop::Get()->SetCurrentModelessDialog(wParam == 0 ? NULL
                                                                   : hWnd);
      return FALSE;
    case WM_NCDESTROY:
      // Clear the reference to |self|.
      SetUserDataPtr(hWnd, NULL);
      self->find_hwnd_ = NULL;
      break;
  }

  return CallWindowProc(self->find_wndproc_old_, hWnd, message, wParam, lParam);
}

// static
LRESULT CALLBACK RootWindowWin::RootWndProc(HWND hWnd,
                                            UINT message,
                                            WPARAM wParam,
                                            LPARAM lParam) {
  REQUIRE_MAIN_THREAD();

  RootWindowWin* self = NULL;
  if (message != WM_NCCREATE) {
    self = GetUserDataPtr<RootWindowWin*>(hWnd);
    if (!self)
      return DefWindowProc(hWnd, message, wParam, lParam);
    DCHECK_EQ(hWnd, self->hwnd_);
  }

  if (self && message == self->find_message_id_) {
    // Message targeting the find dialog.
    LPFINDREPLACE lpfr = reinterpret_cast<LPFINDREPLACE>(lParam);
    CHECK(lpfr == &self->find_state_);
    self->OnFindEvent();
    return 0;
  }

  // Callback for the main window
  switch (message) {
    case WM_COMMAND:
      if (self->OnCommand(LOWORD(wParam)))
        return 0;
      break;

    case WM_GETOBJECT: {
      // Only the lower 32 bits of lParam are valid when checking the object id
      // because it sometimes gets sign-extended incorrectly (but not always).
      DWORD obj_id = static_cast<DWORD>(static_cast<DWORD_PTR>(lParam));

      // Accessibility readers will send an OBJID_CLIENT message.
      if (static_cast<DWORD>(OBJID_CLIENT) == obj_id) {
        if (self->GetBrowser() && self->GetBrowser()->GetHost())
          self->GetBrowser()->GetHost()->SetAccessibilityState(STATE_ENABLED);
      }
    } break;

    case WM_PAINT:
      self->OnPaint();
      return 0;

    case WM_ACTIVATE:
      self->OnActivate(LOWORD(wParam) != WA_INACTIVE);
      // Allow DefWindowProc to set keyboard focus.
      break;

    case WM_SETFOCUS:
      self->OnFocus();
      return 0;

    case WM_SIZE:
      self->OnSize(wParam == SIZE_MINIMIZED);
      break;

    case WM_MOVING:
    case WM_MOVE:
      self->OnMove();
      return 0;

    case WM_DPICHANGED:
      self->OnDpiChanged(wParam, lParam);
      break;

    case WM_ERASEBKGND:
      if (self->OnEraseBkgnd())
        break;
      // Don't erase the background.
      return 0;

    case WM_ENTERMENULOOP:
      if (!wParam) {
        // Entering the menu loop for the application menu.
        CefSetOSModalLoop(true);
      }
      break;

    case WM_EXITMENULOOP:
      if (!wParam) {
        // Exiting the menu loop for the application menu.
        CefSetOSModalLoop(false);
      }
      break;

    case WM_CLOSE:
      if (self->OnClose())
        return 0;  // Cancel the close.
      break;

    case WM_NCHITTEST: {
      LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
      if (hit == HTCLIENT) {
        POINTS points = MAKEPOINTS(lParam);
        POINT point = {points.x, points.y};
        ::ScreenToClient(hWnd, &point);
        if (::PtInRegion(self->draggable_region_, point.x, point.y)) {
          // If cursor is inside a draggable region return HTCAPTION to allow
          // dragging.
          return HTCAPTION;
        }
      }
      return hit;
    }

    case WM_NCCREATE: {
      CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
      self = reinterpret_cast<RootWindowWin*>(cs->lpCreateParams);
      DCHECK(self);
      // Associate |self| with the main window.
      SetUserDataPtr(hWnd, self);
      self->hwnd_ = hWnd;

      self->OnNCCreate(cs);
    } break;

    case WM_CREATE:
      self->OnCreate(reinterpret_cast<CREATESTRUCT*>(lParam));
      break;

    case WM_NCDESTROY:
      // Clear the reference to |self|.
      SetUserDataPtr(hWnd, NULL);
      self->hwnd_ = NULL;
      self->OnDestroyed();
      break;

	case WM_COPYDATA:
		self->HandleCustomMsg(hWnd, wParam, lParam);
		break;
  }

  return DefWindowProc(hWnd, message, wParam, lParam);
}

void RootWindowWin::OnPaint() {
  PAINTSTRUCT ps;
  BeginPaint(hwnd_, &ps);
  EndPaint(hwnd_, &ps);
}

void RootWindowWin::OnFocus() {
  // Selecting "Close window" from the task bar menu may send a focus
  // notification even though the window is currently disabled (e.g. while a
  // modal JS dialog is displayed).
  if (browser_window_ && ::IsWindowEnabled(hwnd_))
    browser_window_->SetFocus(true);
}

void RootWindowWin::OnActivate(bool active) {
  if (active)
    delegate_->OnRootWindowActivated(this);
}

void RootWindowWin::OnSize(bool minimized) {
  if (minimized) {
    // Notify the browser window that it was hidden and do nothing further.
    if (browser_window_)
      browser_window_->Hide();
    return;
  }

  if (browser_window_)
    browser_window_->Show();

  RECT rect;
  GetClientRect(hwnd_, &rect);

  if (with_controls_ && edit_hwnd_) {
    const int button_width = GetButtonWidth(hwnd_);
    const int urlbar_height = GetURLBarHeight(hwnd_);
    const int font_height = LogicalToDevice(14, GetWindowScaleFactor(hwnd_));

    if (font_height != font_height_) {
      font_height_ = font_height;
      if (font_) {
        DeleteObject(font_);
      }

      // Create a scaled font.
      font_ =
          ::CreateFont(-font_height, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");

      SendMessage(back_hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                  TRUE);
      SendMessage(forward_hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                  TRUE);
      SendMessage(reload_hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                  TRUE);
      SendMessage(stop_hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                  TRUE);
      SendMessage(edit_hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                  TRUE);
    }

    // Resize the window and address bar to match the new frame size.
    rect.top += urlbar_height;

    int x_offset = rect.left;

    // |browser_hwnd| may be NULL if the browser has not yet been created.
    HWND browser_hwnd = NULL;
    if (browser_window_)
      browser_hwnd = browser_window_->GetWindowHandle();

    // Resize all controls.
    HDWP hdwp = BeginDeferWindowPos(browser_hwnd ? 6 : 5);
    hdwp = DeferWindowPos(hdwp, back_hwnd_, NULL, x_offset, 0, button_width,
                          urlbar_height, SWP_NOZORDER);
    x_offset += button_width;
    hdwp = DeferWindowPos(hdwp, forward_hwnd_, NULL, x_offset, 0, button_width,
                          urlbar_height, SWP_NOZORDER);
    x_offset += button_width;
    hdwp = DeferWindowPos(hdwp, reload_hwnd_, NULL, x_offset, 0, button_width,
                          urlbar_height, SWP_NOZORDER);
    x_offset += button_width;
    hdwp = DeferWindowPos(hdwp, stop_hwnd_, NULL, x_offset, 0, button_width,
                          urlbar_height, SWP_NOZORDER);
    x_offset += button_width;
    hdwp = DeferWindowPos(hdwp, edit_hwnd_, NULL, x_offset, 0,
                          rect.right - x_offset, urlbar_height, SWP_NOZORDER);

    if (browser_hwnd) {
      hdwp = DeferWindowPos(hdwp, browser_hwnd, NULL, rect.left, rect.top,
                            rect.right - rect.left, rect.bottom - rect.top,
                            SWP_NOZORDER);
    }

    BOOL result = EndDeferWindowPos(hdwp);
    ALLOW_UNUSED_LOCAL(result);
    DCHECK(result);
  } else if (browser_window_) {
    // Size the browser window to the whole client area.
    browser_window_->SetBounds(0, 0, rect.right, rect.bottom);
  }
}

void RootWindowWin::OnMove() {
  // Notify the browser of move events so that popup windows are displayed
  // in the correct location and dismissed when the window moves.
  CefRefPtr<CefBrowser> browser = GetBrowser();
  if (browser)
    browser->GetHost()->NotifyMoveOrResizeStarted();
}

void RootWindowWin::OnDpiChanged(WPARAM wParam, LPARAM lParam) {
  if (LOWORD(wParam) != HIWORD(wParam)) {
    NOTIMPLEMENTED() << "Received non-square scaling factors";
    return;
  }

  if (browser_window_ && with_osr_) {
    // Scale factor for the new display.
    const float display_scale_factor =
        static_cast<float>(LOWORD(wParam)) / DPI_1X;
    browser_window_->SetDeviceScaleFactor(display_scale_factor);
  }

  // Suggested size and position of the current window scaled for the new DPI.
  const RECT* rect = reinterpret_cast<RECT*>(lParam);
  SetBounds(rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top);
}

bool RootWindowWin::OnEraseBkgnd() {
  // Erase the background when the browser does not exist.
  return (GetBrowser() == NULL);
}

bool RootWindowWin::OnCommand(UINT id) {
  if (id >= ID_TESTS_FIRST && id <= ID_TESTS_LAST) {
    delegate_->OnTest(this, id);
    return true;
  }

  switch (id) {
    case IDM_ABOUT:
      OnAbout();
      return true;
    case IDM_EXIT:
      delegate_->OnExit(this);
      return true;
    case ID_FIND:
      OnFind();
      return true;
    case IDC_NAV_BACK:  // Back button
      if (CefRefPtr<CefBrowser> browser = GetBrowser())
        browser->GoBack();
      return true;
    case IDC_NAV_FORWARD:  // Forward button
      if (CefRefPtr<CefBrowser> browser = GetBrowser())
        browser->GoForward();
      return true;
    case IDC_NAV_RELOAD:  // Reload button
      if (CefRefPtr<CefBrowser> browser = GetBrowser())
        browser->Reload();
      return true;
    case IDC_NAV_STOP:  // Stop button
      if (CefRefPtr<CefBrowser> browser = GetBrowser())
        browser->StopLoad();
      return true;
  }

  return false;
}

void RootWindowWin::OnFind() {
  if (find_hwnd_) {
    // Give focus to the existing find dialog.
    ::SetFocus(find_hwnd_);
    return;
  }

  // Configure dialog state.
  ZeroMemory(&find_state_, sizeof(find_state_));
  find_state_.lStructSize = sizeof(find_state_);
  find_state_.hwndOwner = hwnd_;
  find_state_.lpstrFindWhat = find_buff_;
  find_state_.wFindWhatLen = sizeof(find_buff_);
  find_state_.Flags = FR_HIDEWHOLEWORD | FR_DOWN;

  // Create the dialog.
  find_hwnd_ = FindText(&find_state_);

  // Override the dialog's window procedure.
  find_wndproc_old_ = SetWndProcPtr(find_hwnd_, FindWndProc);

  // Associate |self| with the dialog.
  SetUserDataPtr(find_hwnd_, this);
}

void RootWindowWin::OnFindEvent() {
  CefRefPtr<CefBrowser> browser = GetBrowser();

  if (find_state_.Flags & FR_DIALOGTERM) {
    // The find dialog box has been dismissed so invalidate the handle and
    // reset the search results.
    if (browser) {
      browser->GetHost()->StopFinding(true);
      find_what_last_.clear();
      find_next_ = false;
    }
  } else if ((find_state_.Flags & FR_FINDNEXT) && browser) {
    // Search for the requested string.
    bool match_case = ((find_state_.Flags & FR_MATCHCASE) ? true : false);
    const std::wstring& find_what = find_buff_;
    if (match_case != find_match_case_last_ || find_what != find_what_last_) {
      // The search string has changed, so reset the search results.
      if (!find_what.empty()) {
        browser->GetHost()->StopFinding(true);
        find_next_ = false;
      }
      find_match_case_last_ = match_case;
      find_what_last_ = find_buff_;
    }

    browser->GetHost()->Find(0, find_what,
                             (find_state_.Flags & FR_DOWN) ? true : false,
                             match_case, find_next_);
    if (!find_next_)
      find_next_ = true;
  }
}

void RootWindowWin::OnAbout() {
  // Show the about box.
  DialogBox(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd_,
            AboutWndProc);
}

void RootWindowWin::OnNCCreate(LPCREATESTRUCT lpCreateStruct) {
  if (IsProcessPerMonitorDpiAware()) {
    // This call gets Windows to scale the non-client area when WM_DPICHANGED
    // is fired on Windows versions >= 10.0.14393.0.
    typedef BOOL(WINAPI * EnableNonClientDpiScalingPtr)(HWND);
    static EnableNonClientDpiScalingPtr func_ptr =
        reinterpret_cast<EnableNonClientDpiScalingPtr>(GetProcAddress(
            GetModuleHandle(L"user32.dll"), "EnableNonClientDpiScaling"));
    called_enable_non_client_dpi_scaling_ = !!(func_ptr && func_ptr(hwnd_));
  }
}

void RootWindowWin::OnCreate(LPCREATESTRUCT lpCreateStruct) {
  const HINSTANCE hInstance = lpCreateStruct->hInstance;

  RECT rect;
  GetClientRect(hwnd_, &rect);

  if (with_controls_) {
    // Create the child controls.
    int x_offset = 0;

    const int button_width = GetButtonWidth(hwnd_);
    const int urlbar_height = GetURLBarHeight(hwnd_);

    back_hwnd_ = CreateWindow(
        L"BUTTON", L"Back", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        x_offset, 0, button_width, urlbar_height, hwnd_,
        reinterpret_cast<HMENU>(IDC_NAV_BACK), hInstance, 0);
    CHECK(back_hwnd_);
    x_offset += button_width;

    forward_hwnd_ =
        CreateWindow(L"BUTTON", L"Forward",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                     x_offset, 0, button_width, urlbar_height, hwnd_,
                     reinterpret_cast<HMENU>(IDC_NAV_FORWARD), hInstance, 0);
    CHECK(forward_hwnd_);
    x_offset += button_width;

    reload_hwnd_ =
        CreateWindow(L"BUTTON", L"Reload",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                     x_offset, 0, button_width, urlbar_height, hwnd_,
                     reinterpret_cast<HMENU>(IDC_NAV_RELOAD), hInstance, 0);
    CHECK(reload_hwnd_);
    x_offset += button_width;

    stop_hwnd_ = CreateWindow(
        L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        x_offset, 0, button_width, urlbar_height, hwnd_,
        reinterpret_cast<HMENU>(IDC_NAV_STOP), hInstance, 0);
    CHECK(stop_hwnd_);
    x_offset += button_width;

    edit_hwnd_ = CreateWindow(L"EDIT", 0,
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT |
                                  ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_DISABLED,
                              x_offset, 0, rect.right - button_width * 4,
                              urlbar_height, hwnd_, 0, hInstance, 0);
    CHECK(edit_hwnd_);

    // Override the edit control's window procedure.
    edit_wndproc_old_ = SetWndProcPtr(edit_hwnd_, EditWndProc);

    // Associate |this| with the edit window.
    SetUserDataPtr(edit_hwnd_, this);

    rect.top += urlbar_height;

    if (!with_osr_) {
      // Remove the menu items that are only used with OSR.
      HMENU hMenu = ::GetMenu(hwnd_);
      if (hMenu) {
        HMENU hTestMenu = ::GetSubMenu(hMenu, 2);
        if (hTestMenu) {
          ::RemoveMenu(hTestMenu, ID_TESTS_OSR_FPS, MF_BYCOMMAND);
          ::RemoveMenu(hTestMenu, ID_TESTS_OSR_DSF, MF_BYCOMMAND);
        }
      }
    }
	CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
	if (command_line->HasSwitch(switches::kHideTopMenu)) {
		::SetMenu(hwnd_, NULL);
	}
  } else {
    // No controls so also remove the default menu.
    ::SetMenu(hwnd_, NULL);
  }

  const float device_scale_factor = GetWindowScaleFactor(hwnd_);

  if (with_osr_) {
    browser_window_->SetDeviceScaleFactor(device_scale_factor);
  }

  if (!is_popup_) {
    // Create the browser window.
    CefRect cef_rect(rect.left, rect.top, rect.right - rect.left,
                     rect.bottom - rect.top);
    browser_window_->CreateBrowser(hwnd_, cef_rect, browser_settings_,
                                   delegate_->GetRequestContext(this));
  } else {
    // With popups we already have a browser window. Parent the browser window
    // to the root window and show it in the correct location.
    browser_window_->ShowPopup(hwnd_, rect.left, rect.top,
                               rect.right - rect.left, rect.bottom - rect.top);
  }
}

bool RootWindowWin::OnClose() {
  if (browser_window_ && !browser_window_->IsClosing()) {
    CefRefPtr<CefBrowser> browser = GetBrowser();
    if (browser) {
      // Notify the browser window that we would like to close it. This
      // will result in a call to ClientHandler::DoClose() if the
      // JavaScript 'onbeforeunload' event handler allows it.
      browser->GetHost()->CloseBrowser(false);

      // Cancel the close.
      return true;
    }
  }

  // Allow the close.
  return false;
}

void RootWindowWin::OnDestroyed() {
  window_destroyed_ = true;
  NotifyDestroyedIfDone();
}

void RootWindowWin::OnBrowserCreated(CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();

  if (is_popup_) {
    // For popup browsers create the root window once the browser has been
    // created.
    CreateRootWindow(CefBrowserSettings(), false);
  } else {
    // Make sure the browser is sized correctly.
    OnSize(false);
    

  }

  delegate_->OnBrowserCreated(this, browser);

  
}

void RootWindowWin::OnBrowserWindowDestroyed() {
  REQUIRE_MAIN_THREAD();

  browser_window_.reset();

  if (!window_destroyed_) {
    // The browser was destroyed first. This could be due to the use of
    // off-screen rendering or execution of JavaScript window.close().
    // Close the RootWindow.
    Close(true);
  }

  browser_destroyed_ = true;
  NotifyDestroyedIfDone();
}

void RootWindowWin::OnSetAddress(const std::string& url) {
  REQUIRE_MAIN_THREAD();

  if (edit_hwnd_)
    SetWindowText(edit_hwnd_, CefString(url).ToWString().c_str());
}

void RootWindowWin::OnSetTitle(const std::string& title) {
  REQUIRE_MAIN_THREAD();

  if (hwnd_)
    SetWindowText(hwnd_, CefString(title).ToWString().c_str());
}

void RootWindowWin::OnSetFullscreen(bool fullscreen) {
  REQUIRE_MAIN_THREAD();

  CefRefPtr<CefBrowser> browser = GetBrowser();
  if (browser) {
    scoped_ptr<window_test::WindowTestRunnerWin> test_runner(
        new window_test::WindowTestRunnerWin());
    if (fullscreen)
      test_runner->Maximize(browser);
    else
      test_runner->Restore(browser);
  }
}

void RootWindowWin::OnAutoResize(const CefSize& new_size) {
  REQUIRE_MAIN_THREAD();

  if (!hwnd_)
    return;

  int new_width = new_size.width;

  // Make the window wide enough to drag by the top menu bar.
  if (new_width < 200)
    new_width = 200;

  const float device_scale_factor = GetWindowScaleFactor(hwnd_);
  RECT rect = {0, 0, LogicalToDevice(new_width, device_scale_factor),
               LogicalToDevice(new_size.height, device_scale_factor)};
  DWORD style = GetWindowLong(hwnd_, GWL_STYLE);
  DWORD ex_style = GetWindowLong(hwnd_, GWL_EXSTYLE);
  bool has_menu = !(style & WS_CHILD) && (GetMenu(hwnd_) != NULL);

  // The size value is for the client area. Calculate the whole window size
  // based on the current style.
  AdjustWindowRectEx(&rect, style, has_menu, ex_style);

  // Size the window. The left/top values may be negative.
  // Also show the window if it's not currently visible.
  SetWindowPos(hwnd_, NULL, 0, 0, rect.right - rect.left,
               rect.bottom - rect.top,
               SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void RootWindowWin::OnSetLoadingState(bool isLoading,
                                      bool canGoBack,
                                      bool canGoForward) {
  REQUIRE_MAIN_THREAD();

  if (with_controls_) {
    EnableWindow(back_hwnd_, canGoBack);
    EnableWindow(forward_hwnd_, canGoForward);
    EnableWindow(reload_hwnd_, !isLoading);
    EnableWindow(stop_hwnd_, isLoading);
    EnableWindow(edit_hwnd_, TRUE);
  }

  if (!isLoading && GetWindowLongPtr(hwnd_, GWL_EXSTYLE) & WS_EX_NOACTIVATE) {
    // Done with the initial navigation. Remove the WS_EX_NOACTIVATE style so
    // that future mouse clicks inside the browser correctly activate and focus
    // the window. For the top-level window removing this style causes Windows
    // to display the task bar button.
    SetWindowLongPtr(hwnd_, GWL_EXSTYLE,
                     GetWindowLongPtr(hwnd_, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);

    if (browser_window_) {
      HWND browser_hwnd = browser_window_->GetWindowHandle();
      SetWindowLongPtr(
          browser_hwnd, GWL_EXSTYLE,
          GetWindowLongPtr(browser_hwnd, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
    }
  }
}

namespace {

LPCWSTR kParentWndProc = L"CefParentWndProc";
LPCWSTR kDraggableRegion = L"CefDraggableRegion";

LRESULT CALLBACK SubclassedWindowProc(HWND hWnd,
                                      UINT message,
                                      WPARAM wParam,
                                      LPARAM lParam) {
  WNDPROC hParentWndProc =
      reinterpret_cast<WNDPROC>(::GetPropW(hWnd, kParentWndProc));
  HRGN hRegion = reinterpret_cast<HRGN>(::GetPropW(hWnd, kDraggableRegion));

  if (message == WM_NCHITTEST) {
    LRESULT hit = CallWindowProc(hParentWndProc, hWnd, message, wParam, lParam);
    if (hit == HTCLIENT) {
      POINTS points = MAKEPOINTS(lParam);
      POINT point = {points.x, points.y};
      ::ScreenToClient(hWnd, &point);
      if (::PtInRegion(hRegion, point.x, point.y)) {
        // Let the parent window handle WM_NCHITTEST by returning HTTRANSPARENT
        // in child windows.
        return HTTRANSPARENT;
      }
    }
    return hit;
  }

  return CallWindowProc(hParentWndProc, hWnd, message, wParam, lParam);
}

void SubclassWindow(HWND hWnd, HRGN hRegion) {
  HANDLE hParentWndProc = ::GetPropW(hWnd, kParentWndProc);
  if (hParentWndProc) {
    return;
  }

  SetLastError(0);
  LONG_PTR hOldWndProc = SetWindowLongPtr(
      hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassedWindowProc));
  if (hOldWndProc == 0 && GetLastError() != ERROR_SUCCESS) {
    return;
  }

  ::SetPropW(hWnd, kParentWndProc, reinterpret_cast<HANDLE>(hOldWndProc));
  ::SetPropW(hWnd, kDraggableRegion, reinterpret_cast<HANDLE>(hRegion));
}

void UnSubclassWindow(HWND hWnd) {
  LONG_PTR hParentWndProc =
      reinterpret_cast<LONG_PTR>(::GetPropW(hWnd, kParentWndProc));
  if (hParentWndProc) {
    LONG_PTR hPreviousWndProc =
        SetWindowLongPtr(hWnd, GWLP_WNDPROC, hParentWndProc);
    ALLOW_UNUSED_LOCAL(hPreviousWndProc);
    DCHECK_EQ(hPreviousWndProc,
              reinterpret_cast<LONG_PTR>(SubclassedWindowProc));
  }

  ::RemovePropW(hWnd, kParentWndProc);
  ::RemovePropW(hWnd, kDraggableRegion);
}

BOOL CALLBACK SubclassWindowsProc(HWND hwnd, LPARAM lParam) {
  SubclassWindow(hwnd, reinterpret_cast<HRGN>(lParam));
  return TRUE;
}

BOOL CALLBACK UnSubclassWindowsProc(HWND hwnd, LPARAM lParam) {
  UnSubclassWindow(hwnd);
  return TRUE;
}

}  // namespace

void RootWindowWin::OnSetDraggableRegions(
    const std::vector<CefDraggableRegion>& regions) {
  REQUIRE_MAIN_THREAD();

  // Reset draggable region.
  ::SetRectRgn(draggable_region_, 0, 0, 0, 0);

  // Determine new draggable region.
  std::vector<CefDraggableRegion>::const_iterator it = regions.begin();
  for (; it != regions.end(); ++it) {
    HRGN region = ::CreateRectRgn(it->bounds.x, it->bounds.y,
                                  it->bounds.x + it->bounds.width,
                                  it->bounds.y + it->bounds.height);
    ::CombineRgn(draggable_region_, draggable_region_, region,
                 it->draggable ? RGN_OR : RGN_DIFF);
    ::DeleteObject(region);
  }

  // Subclass child window procedures in order to do hit-testing.
  // This will be a no-op, if it is already subclassed.
  if (hwnd_) {
    WNDENUMPROC proc =
        !regions.empty() ? SubclassWindowsProc : UnSubclassWindowsProc;
    ::EnumChildWindows(hwnd_, proc,
                       reinterpret_cast<LPARAM>(draggable_region_));
  }
}

void RootWindowWin::NotifyDestroyedIfDone() {
  // Notify once both the window and the browser have been destroyed.
  if (window_destroyed_ && browser_destroyed_)
    delegate_->OnRootWindowDestroyed(this);
}

void ModifyZoom(CefRefPtr<CefBrowser> browser, double zoom) {
    if (!CefCurrentlyOn(TID_UI)) {
        LOG(INFO) << "post task ModifyZoom to the ui thread:" << zoom;
        // Execute on the UI thread.
        CefPostTask(TID_UI, base::Bind(&ModifyZoom, browser, zoom));
        return;
    }
    double old_zoom = browser->GetHost()->GetZoomLevel();
    LOG(INFO) << "ModifyZoom on the ui thread:" << old_zoom << " -> " << zoom;
    browser->GetHost()->SetZoomLevel(zoom);

    LOG(INFO) << "after ModifyZoom on the ui thread:" << browser->GetHost()->GetZoomLevel();
}

void CallJavaScriptFunc(CefRefPtr<CefBrowser> browser, std::string strFile, std::string strParams) {
	if (!browser)
		return;
	CefRefPtr<CefFrame> cefFrame = browser->GetMainFrame();
	if (cefFrame)
	{
		std::string strJsCode = "ActivateJsFunction(\"";
		strJsCode += strFile;
		strJsCode += "\",\"";
		strJsCode += strParams;
		strJsCode += "\")";
		cefFrame->ExecuteJavaScript(CefString(strJsCode), cefFrame->GetURL(), 0);
	}
}

void HandleCustomTask(RootWindowWin* rootWnd, nlohmann::json input) {
	LOG(INFO) << "HandleCustomTask====================";
    if (!rootWnd)
    {
        LOG(INFO) << "HandleCustomTask: rootWnd is null";
        return;
    }
	LOG(INFO) << "HandleCustomTask====================1";
    std::string cmd = input["cmd"];

    std::string id = input["id"];
    std::string url = input["url"];
    bool resize = input["resize"];
    bool visible = input["visible"];
    bool enabled = input["enabled"];
    int x = input["x"];
    int y = input["y"];
	LOG(INFO) << "HandleCustomTask====================2";
    int width = input["width"];
    int height = input["height"];
    double zoom = input["zoom"];
    std::string parent_handle_s = input["parent_handle"];
    std::string cefclient_config_filename = input["cefclient_config_filename"];
    std::string pid = input["pid"];
	LOG(INFO) << "HandleCustomTask====================3";
	//std::string machine_code = input["machine_code"];
	LOG(INFO) << "HandleCustomTask====================4";
    std::string key = id + "_" + parent_handle_s;


    LOG(INFO) << "handle cmd:" << cmd;
    LOG(INFO) << "handle msg:" << input;

	if (!bFirstInit)
	{
		LOG(INFO) << "handle first cmd:" << cmd;
		CefClientToNpl::GetInstance()->SetParentHandle(parent_handle_s);
		bFirstInit = true;
	}
    CefRefPtr<CefBrowser> b = rootWnd->GetBrowser();
    if (cmd == "CheckCefWindow")
    {

        LOG(INFO) << "CheckCefWindow:" << key;
        if (b)
        {
            LOG(INFO) << "CheckCefWindow CefBrowser is found:" << key;
            CefRefPtr<CefBrowserHost> browser_host = b->GetHost();
            if (browser_host)
            {
                LOG(INFO) << "CheckCefWindow CefBrowser is created:" << key;
                bool has_browser = false;
                LOG(INFO) << "CheckCefWindow: read value " << key;
                CefClientConfig::GetInstance(cefclient_config_filename, pid)->ReadJsonValue(key, has_browser);
                LOG(INFO) << "before CheckCefWindow result:" << has_browser;
                if (!has_browser)
                {
                    has_browser = true;
                    CefClientConfig::GetInstance(cefclient_config_filename, pid)->WriteJsonValue(key, true);
                    LOG(INFO) << "after CheckCefWindow result:" << has_browser;
                }
            }
        }
        else {
            LOG(INFO) << "CheckCefWindow: browser is null:" << key;
        }
        return;
    }

    if (!b) {
        return;
    }
	LOG(INFO) << " command is ============"<< cmd;
    if (cmd == "Start")
    {
        //do nothing
    }
    else if (cmd == "Open")
    {
        if (!url.empty())
        {
            b->GetMainFrame().get()->LoadURL(url);
        }
        if (resize)
        {
            rootWnd->SetBounds(x, y, width, height);
        }
        if (visible)
        {
            rootWnd->Show(RootWindow::ShowMode::ShowNormal);
            ModifyZoom(b, zoom);

        }
    }
    else if (cmd == "ChangePosSize")
    {
        rootWnd->SetBounds(x, y, width, height);
    }
    else if (cmd == "Show")
    {
        if (visible)
        {
            rootWnd->Show(RootWindow::ShowMode::ShowNormal);
            ModifyZoom(b, zoom);
        }
        else
        {
            rootWnd->Hide();
        }
    }
    else if (cmd == "Zoom")
    {
        ModifyZoom(b, zoom);
    }
    else if (cmd == "EnableWindow")
    {
        ::EnableWindow(rootWnd->GetWindowHandle(), enabled);
    }
    else if (cmd == "Quit")
    {
        rootWnd->Close(true);
    }
	else if (cmd == "CallJsFunc")
	{
		std::string jsFile = input["file"];
		std::string jsParams = input["params"];
		CallJavaScriptFunc(b, jsFile, jsParams);
	}
}


void RootWindowWin::HandleCustomMsg(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
	try
	{

        REQUIRE_MAIN_THREAD();
        RootWindowWin* self = GetUserDataPtr<RootWindowWin*>(hWnd);
        DCHECK(self);
        DCHECK(hWnd == self->find_hwnd_);//这个地方debug会检测失败
		COPYDATASTRUCT* pcds = (COPYDATASTRUCT*)lParam;
		std::string s = (const char*)(pcds->lpData);
		nlohmann::json input = nlohmann::json::parse(s);
        std::string cmd = input["cmd"];
        LOG(INFO) << "post custom task on main thread:" << cmd;
        MAIN_POST_CLOSURE(base::Bind(&HandleCustomTask, self, input));
	}
	catch (...)
	{
        LOG(INFO) << "handle custom msg failed";
	}
	
}

}  // namespace client

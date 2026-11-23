#include "scene.h"

#include <dwmapi.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace {

const int MAX_WINDOWS = 31;   // plus the taskbar
const float CORNER = 8;       // corner radius of the windows under Windows 11, logical pixels

struct Found {
  HWND hwnd;
  RECT rect;
  float opacity;
  bool zoomed;
  wchar_t cls[64];
  LONG_PTR style, ex;
};

struct Collect {
  const Monitor* m;
  const std::vector<HWND>* own;
  std::vector<Found> list;   // front to back
};

bool isShell(const wchar_t* cls) {
  static const wchar_t* names[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};
  for (const wchar_t* n : names) {
    if (!wcscmp(cls, n)) return true;
  }
  return false;
}

BOOL CALLBACK onWindow(HWND h, LPARAM lp) {
  Collect& c = *reinterpret_cast<Collect*>(lp);
  if (std::find(c.own->begin(), c.own->end(), h) != c.own->end()) return TRUE;
  if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
  // Windows on other virtual desktops and suspended apps count as visible but are
  // cloaked.
  DWORD cloaked = 0;
  DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  if (cloaked) return TRUE;

  Found f{};
  f.hwnd = h;
  GetClassNameW(h, f.cls, 64);
  if (isShell(f.cls)) return TRUE;
  f.style = GetWindowLongPtrW(h, GWL_STYLE);
  f.ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  // Click-through overlays of other programs cover nothing.
  if ((f.ex & WS_EX_LAYERED) && (f.ex & WS_EX_TRANSPARENT)) return TRUE;
  f.opacity = 1;
  if (f.ex & WS_EX_LAYERED) {
    BYTE alpha = 255;
    DWORD flags = 0;
    COLORREF key = 0;
    if (GetLayeredWindowAttributes(h, &key, &alpha, &flags) && (flags & LWA_ALPHA)) f.opacity = alpha / 255.0f;
    if (f.opacity < 0.05f) return TRUE;
  }
  if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &f.rect, sizeof(f.rect)))) GetWindowRect(h, &f.rect);
  RECT on;
  if (!IntersectRect(&on, &f.rect, &c.m->rect)) return TRUE;
  if (on.right - on.left < 24 || on.bottom - on.top < 24) return TRUE;
  f.zoomed = IsZoomed(h) != 0;
  c.list.push_back(f);
  return TRUE;
}

Collect collect(const Monitor& m, const std::vector<HWND>& own) {
  Collect c{&m, &own, {}};
  EnumWindows(onWindow, reinterpret_cast<LPARAM>(&c));
  return c;
}

Occluder toOccluder(const Monitor& m, const RECT& r, float radius, float opacity, int kind, HWND h) {
  float s = m.scale;
  return {(r.left - m.rect.left) / s, (r.top - m.rect.top) / s, (r.right - m.rect.left) / s, (r.bottom - m.rect.top) / s,
          radius, opacity, kind, uint64_t(reinterpret_cast<uintptr_t>(h))};
}

// The view of the desktop in the shell and its list window with the icons.
ComPtr<IFolderView2> desktopView(HWND* list) {
  ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return nullptr;
  VARIANT where, none;
  VariantInit(&where);
  VariantInit(&none);
  where.vt = VT_I4;
  where.lVal = CSIDL_DESKTOP;
  long hwnd = 0;
  ComPtr<IDispatch> disp;
  if (windows->FindWindowSW(&where, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK) return nullptr;
  ComPtr<IServiceProvider> provider;
  ComPtr<IShellBrowser> browser;
  ComPtr<IShellView> view;
  ComPtr<IFolderView2> folder;
  if (FAILED(disp.As(&provider))) return nullptr;
  if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) return nullptr;
  if (FAILED(browser->QueryActiveShellView(&view))) return nullptr;
  if (FAILED(view.As(&folder))) return nullptr;
  HWND def = nullptr;
  view->GetWindow(&def);
  if (list) *list = FindWindowExW(def, nullptr, L"SysListView32", nullptr);
  return folder;
}

struct Icon {
  POINT at;            // top-left corner of the cell in the list window, device pixels
  std::wstring name;
  ComPtr<IShellItemImageFactory> image;
};

struct IconSet {
  std::vector<Icon> icons;
  int size = 48;       // edge length of the image, device pixels
  POINT spacing{};     // grid of the cells, device pixels
  POINT origin{};      // origin of the list window on the screen
  uint64_t stamp = 0;
};

bool readIcons(const Monitor& m, IconSet& set, bool withImages) {
  HWND list = nullptr;
  ComPtr<IFolderView2> view = desktopView(&list);
  if (!view || !list) return false;
  FOLDERVIEWMODE mode;
  int logical = 48;
  view->GetViewModeAndIconSize(&mode, &logical);
  set.size = int(std::lround(logical * m.scale));
  view->GetSpacing(&set.spacing);
  set.origin = {0, 0};
  ClientToScreen(list, &set.origin);
  ComPtr<IShellFolder> folder;
  view->GetFolder(IID_PPV_ARGS(&folder));

  int count = 0;
  view->ItemCount(SVGIO_ALLVIEW, &count);
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(count);
  mix(set.size);
  mix(uint32_t(set.origin.x));
  mix(uint32_t(set.origin.y));
  for (int i = 0; i < count; i++) {
    PITEMID_CHILD pidl = nullptr;
    if (FAILED(view->Item(i, &pidl)) || !pidl) continue;
    Icon icon;
    if (SUCCEEDED(view->GetItemPosition(pidl, &icon.at))) {
      mix(uint32_t(icon.at.x));
      mix(uint32_t(icon.at.y));
      if (folder) {
        STRRET str;
        wchar_t* name = nullptr;
        if (SUCCEEDED(folder->GetDisplayNameOf(pidl, SHGDN_NORMAL, &str)) && SUCCEEDED(StrRetToStrW(&str, pidl, &name))) {
          icon.name = name;
          for (wchar_t ch : icon.name) mix(ch);
          CoTaskMemFree(name);
        }
        if (withImages) SHCreateItemWithParent(nullptr, folder.Get(), pidl, IID_PPV_ARGS(&icon.image));
      }
      set.icons.push_back(std::move(icon));
    }
    CoTaskMemFree(pidl);
  }
  set.stamp = h;
  return true;
}

// Small drawing surface in memory, 32 bits, top-down.
struct Canvas {
  HDC dc = nullptr;
  HBITMAP bmp = nullptr, old = nullptr;
  uint32_t* bits = nullptr;
  int w = 0, h = 0;
  Canvas(int width, int height) : w(width), h(height) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dc = CreateCompatibleDC(nullptr);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    old = static_cast<HBITMAP>(SelectObject(dc, bmp));
  }
  ~Canvas() {
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
  }
};

void stamp(std::vector<uint8_t>& mask, int mw, int mh, int x, int y, uint8_t v) {
  if (x >= 0 && y >= 0 && x < mw && y < mh) mask[size_t(y) * mw + x] = std::max(mask[size_t(y) * mw + x], v);
}

}  // namespace

Monitor primaryMonitor() {
  Monitor m;
  HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  m.rect = mi.rcMonitor;
  UINT dx = 96, dy = 96;
  if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) m.scale = dx / 96.0f;
  return m;
}

Scene captureScene(const Monitor& m, const std::vector<HWND>& own) {
  Scene s;
  Collect c = collect(m, own);
  // Only the frontmost ones count if there are more than the light processes.
  if (int(c.list.size()) > MAX_WINDOWS) c.list.resize(MAX_WINDOWS);

  // Full screen: the frontmost window covers the whole screen including the
  // taskbar.
  if (!c.list.empty()) {
    const Found& top = c.list.front();
    bool covers = top.rect.left <= m.rect.left && top.rect.top <= m.rect.top && top.rect.right >= m.rect.right &&
                  top.rect.bottom >= m.rect.bottom;
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    SHQueryUserNotificationState(&state);
    s.fullscreen = covers && top.opacity > 0.9f &&
                   (GetForegroundWindow() == top.hwnd || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY ||
                    state == QUNS_PRESENTATION_MODE);
  }

  for (auto it = c.list.rbegin(); it != c.list.rend(); ++it) {
    s.rects.push_back(toOccluder(m, it->rect, it->zoomed ? 0 : CORNER, it->opacity, 1, it->hwnd));
  }
  HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
  RECT r, on, work = m.rect;
  if (bar && IsWindowVisible(bar) && GetWindowRect(bar, &r) && IntersectRect(&on, &r, &m.rect) && on.bottom - on.top > 8) {
    s.rects.push_back(toOccluder(m, on, 0, 0.88f, 2, bar));
    // The rest of the screen next to the taskbar.
    if (on.right - on.left >= m.width()) {
      if (on.top > m.rect.top) work.bottom = on.top;
      else work.top = on.bottom;
    }
  }
  for (const Found& f : c.list) {
    if (f.opacity > 0.99f && f.rect.left <= work.left && f.rect.top <= work.top && f.rect.right >= work.right && f.rect.bottom >= work.bottom) {
      s.buried = true;
      break;
    }
  }
  return s;
}

bool captureIcons(const Monitor& m, std::vector<uint8_t>& mask, uint64_t& stampOut) {
  // First only count and position: that is cheap and tells whether anything has
  // happened.
  IconSet probe;
  if (!readIcons(m, probe, false)) return false;
  int mw = m.width(), mh = m.height();
  if (probe.stamp == stampOut && mask.size() == size_t(mw) * mh) return false;

  IconSet set;
  if (!readIcons(m, set, true)) return false;
  stampOut = set.stamp;
  mask.assign(size_t(mw) * mh, 0);

  // Dimensions of the icon view as in the list window of Windows: the image is
  // horizontally centered in its cell, the label centered below it.
  int cellW = set.spacing.x > 0 ? set.spacing.x : set.size + int(27 * m.scale);
  int gap = int(std::lround(5 * m.scale));
  UINT dpi = UINT(std::lround(96 * m.scale));
  LOGFONTW lf = {};
  SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0, dpi);
  HFONT font = CreateFontIndirectW(&lf);

  int labelW = cellW - int(4 * m.scale), labelH = int(std::lround(40 * m.scale));
  Canvas text(std::max(labelW, 8), labelH);
  HFONT oldFont = static_cast<HFONT>(SelectObject(text.dc, font));
  SetBkMode(text.dc, TRANSPARENT);
  SetTextColor(text.dc, RGB(255, 255, 255));

  for (const Icon& icon : set.icons) {
    // The shell reports the top-left corner of the image; the cell starts to the
    // left of it.
    int ix = set.origin.x - m.rect.left + icon.at.x, iy = set.origin.y - m.rect.top + icon.at.y;
    int cx = ix - (cellW - set.size) / 2;

    // Image: the real outline from the alpha channel, otherwise a square.
    HBITMAP bmp = nullptr;
    DIBSECTION dib = {};
    bool drawn = false;
    if (icon.image && SUCCEEDED(icon.image->GetImage({set.size, set.size}, SIIGBF_ICONONLY, &bmp)) && bmp &&
        GetObjectW(bmp, sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBitsPixel == 32 && dib.dsBm.bmBits) {
      int w = dib.dsBm.bmWidth, h = std::abs(dib.dsBm.bmHeight);
      bool bottomUp = dib.dsBmih.biHeight > 0;
      const uint32_t* px = static_cast<const uint32_t*>(dib.dsBm.bmBits);
      for (int y = 0; y < h; y++) {
        const uint32_t* row = px + size_t(bottomUp ? h - 1 - y : y) * w;
        for (int x = 0; x < w; x++) stamp(mask, mw, mh, ix + x, iy + y, uint8_t(row[x] >> 24));
      }
      drawn = true;
    }
    if (bmp) DeleteObject(bmp);
    if (!drawn) {
      for (int y = 0; y < set.size; y++) {
        for (int x = 0; x < set.size; x++) stamp(mask, mw, mh, ix + x, iy + y, 255);
      }
    }

    // Label: at most two lines, centered, as on the desktop.
    std::fill(text.bits, text.bits + size_t(text.w) * text.h, 0u);
    RECT rc = {0, 0, text.w, text.h};
    DrawTextW(text.dc, icon.name.c_str(), int(icon.name.size()), &rc,
              DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_END_ELLIPSIS);
    int lx = cx + (cellW - text.w) / 2, ly = iy + set.size + gap;
    for (int y = 0; y < text.h; y++) {
      for (int x = 0; x < text.w; x++) {
        uint8_t v = uint8_t((text.bits[size_t(y) * text.w + x] >> 8) & 255);
        if (v) stamp(mask, mw, mh, lx + x, ly + y, v);
      }
    }
  }
  SelectObject(text.dc, oldFont);
  DeleteObject(font);
  return true;
}

std::wstring describeDesktop(const Monitor& m) {
  std::wstring out;
  wchar_t line[400];
  swprintf_s(line, L"Display %d x %d at (%d, %d), scale %.2f\n", m.width(), m.height(), m.rect.left, m.rect.top, m.scale);
  out += line;

  HWND progman = FindWindowW(L"Progman", nullptr);
  HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
  HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
  swprintf_s(line, L"Progman %p (style %llx), in it icon view %p, in it WorkerW %p\n", progman,
             (unsigned long long)GetWindowLongPtrW(progman, GWL_EXSTYLE), def, worker);
  out += line;

  Collect c = collect(m, {});
  out += L"Windows front to back (class, position, style, extended style, opacity):\n";
  for (const Found& f : c.list) {
    swprintf_s(line, L"  %-40s %5d %5d %5d %5d  %08llx %08llx  %.2f%s\n", f.cls, f.rect.left, f.rect.top, f.rect.right,
               f.rect.bottom, (unsigned long long)f.style, (unsigned long long)f.ex, f.opacity, f.zoomed ? L" maximized" : L"");
    out += line;
  }
  Scene s = captureScene(m, {});
  swprintf_s(line, L"For the light: %d rects, full screen %s, desktop covered %s\n", int(s.rects.size()), s.fullscreen ? L"yes" : L"no", s.buried ? L"yes" : L"no");
  out += line;

  IconSet set;
  if (readIcons(m, set, false)) {
    swprintf_s(line, L"Icons: %d, image %d px, grid %d x %d, origin (%d, %d)\n", int(set.icons.size()), set.size,
               set.spacing.x, set.spacing.y, set.origin.x, set.origin.y);
    out += line;
    for (const Icon& i : set.icons) {
      swprintf_s(line, L"  at %5d %5d, name of %d characters\n", i.at.x, i.at.y, int(i.name.size()));
      out += line;
    }
  } else {
    out += L"Icons: desktop view not reachable\n";
  }
  return out;
}

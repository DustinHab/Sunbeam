// Reads from the desktop what is in the way of the light: windows, taskbar,
// icons.

#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "render.h"

struct Monitor {
  RECT rect{};       // in device pixels
  float scale = 1;   // device pixels per logical pixel
  int width() const { return rect.right - rect.left; }
  int height() const { return rect.bottom - rect.top; }
};

struct Scene {
  std::vector<Occluder> rects;   // windows back to front, the taskbar last
  bool fullscreen = false;       // an application in full screen is in front
  bool buried = false;           // an opaque window fills the screen up to the taskbar, nothing of the desktop is visible
};

Monitor primaryMonitor();

// Windows and taskbar on the display m. The program's own windows (own) are
// skipped.
Scene captureScene(const Monitor& m, const std::vector<HWND>& own);

// Coverage of the desktop icons including labels, one byte per device pixel.
// stamp summarizes count and position; the mask is only redrawn when it changes.
// Returns whether mask is new.
bool captureIcons(const Monitor& m, std::vector<uint8_t>& mask, uint64_t& stamp);

// For debugging: windows with class, position and style (without title), plus
// the icons as count and position, as text.
std::wstring describeDesktop(const Monitor& m);

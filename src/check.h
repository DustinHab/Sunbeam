#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

// Renders the image without a window to files and measures the timings.
// viewW and viewH in logical pixels, scale device pixels per logical pixel.
int runCheck(const std::wstring& dir, int viewW, int viewH, float scale);

// Renders the image for the real position of windows and icons to files, without
// showing anything. sunX, sunY: position of the sun as a fraction of the screen.
int runReal(const std::wstring& dir, float sunX, float sunY);

// Measures what a frame costs at rest and while a window is dragged, part by
// part, and writes it to drag.txt.
int runDrag(const std::wstring& dir, float sunX, float sunY);

// Measures, with a simulated pointer and window, how far the light is from a
// dragged window when the frame appears, and writes it to pace.txt.
int runPace(const std::wstring& dir, float sunX, float sunY);

// Renders the sky of an empty desktop for a series of times of day or sun
// positions to files.
int runSky(const std::wstring& dir, int viewW, int viewH, float scale);

// Compares drifting clouds with what would be correct at the same moment.
int runDrift(const std::wstring& dir, int viewW, int viewH, float scale);

// Draws the pages of the settings window to files.
int runPanelCheck(const std::wstring& dir);

// Fetches the weather for a place once and writes to weather.txt what arrived
// and where the sliders went with it; plus a series of sample conditions.
int runWeatherCheck(const std::wstring& dir, const std::wstring& query);

// Lets two days pass for each ground and writes to life.txt how often ships,
// aircraft and birds appeared, for how long, and where in the image.
int runLifeCheck(const std::wstring& dir);

// Measures on the real display, with a small, barely visible patch, whether a
// window like the light window adds to the image below it and can darken it.
int runBlendCheck(const std::wstring& file);

// Saves an image (BGRA, row by row from the top) as PNG, downscaled by factor;
// with cw and ch only the crop starting at (cx, cy).
bool writeImage(const std::wstring& path, const uint8_t* bgra, int w, int h, int factor, int cx = 0, int cy = 0, int cw = 0, int ch = 0);

// Reads the image that Windows actually sends to the display (in HDR mode
// linear, 1.0 = 80 cd/m²), unlike a screenshot.
struct Desk {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> last;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  HRESULT error = S_OK;
  int w = 0, h = 0;
  // The mouse pointer as Windows draws it: size, top-left corner, hot spot.
  int pointerW = 0, pointerH = 0, pointerX = 0, pointerY = 0, pointerHotX = 0, pointerHotY = 0;

  bool open(ID3D11Device* d, ID3D11DeviceContext* c);
  // Fetches the latest state; returns as soon as nothing new arrives for 50 ms, at
  // the latest after maxMs.
  void refresh(int maxMs = 300);
  // Crop as red, green, blue per pixel.
  bool read(int x, int y, int cw, int ch, std::vector<float>& rgb);
  bool mean(int x, int y, int n, double out[3]);
  // The whole image as image values (BGRA, from the top); white: brightness of
  // white in the HDR image.
  bool readSrgb(float white, std::vector<uint8_t>& bgra);
};

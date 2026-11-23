#include "app.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "astro.h"
#include "check.h"
#include "daycycle.h"
#include "panel.h"
#include "render.h"
#include "scene.h"
#include "place.h"
#include "weather.h"

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* HOST_CLASS = L"SunbeamHost";
const wchar_t* SKY_CLASS = L"SunbeamSky";
const wchar_t* LIGHT_CLASS = L"SunbeamLight";
const wchar_t* GRIP_CLASS = L"SunbeamGrip";
const UINT WM_TRAY = WM_APP + 1;
const UINT_PTR HOTKEY_QUIT = 1, HOTKEY_SPOT = 2;
enum Menu { M_PLACE = 1, M_FIXED, M_CLOCK, M_LAPSE, M_PAUSE, M_SETTINGS, M_AUTOSTART, M_QUIT };
enum Course { BY_HAND = 0, BY_CLOCK = 1, TIME_LAPSE = 2 };   // course of the sun

const double CALM = 1.0 / 60;    // without a user change, at least this much time lies between two frames
const double BUSY = 1.0 / 250;   // after a user change the light follows as fast as the display shows frames
const double SCENE_EVERY = 0.25; // the window positions are re-read at least this often
const int ICONS_EVERY_MS = 1500;
const int SETTINGS_LEVEL = 5;    // increases when defaults have been retuned

struct Setting {
  const wchar_t* key;
  float Params::* field;
};
const Setting SETTINGS[] = {
    {L"Haze", &Params::density},         {L"Mist", &Params::mist},
    {L"RayLength", &Params::length}, {L"DepthSpread", &Params::spread},
    {L"ForwardScatter", &Params::g},   {L"Dust", &Params::dust},
    {L"SunSize", &Params::sunRadius}, {L"Clouds", &Params::clouds},
    {L"Wind", &Params::wind},             {L"Brightness", &Params::exposure},
    {L"Glare", &Params::bloom},        {L"RimLight", &Params::rim},
    {L"OverWindows", &Params::overWindows}, {L"Resolution", &Params::resolution},
    {L"OverSky", &Params::overSky},  {L"Shadows", &Params::shade},
    {L"Pointer", &Params::pointer},     {L"Stars", &Params::stars},
    {L"Moonlight", &Params::moonlight},   {L"HdrGlow", &Params::glow},
    {L"MoonSize", &Params::moonRadius}, {L"Rays", &Params::rays}, {L"Ground", &Params::ground},
    {L"Life", &Params::life},
};

struct App {
  HINSTANCE inst = nullptr;
  Monitor mon;
  std::wstring dir, ini, logFile;
  FILETIME iniTime{};
  Params par;
  float sunX = 0.5f, sunY = 0.6f;   // sun position set by hand, as a fraction of the screen
  int course = BY_HAND;             // course of the sun: by hand, by the clock, time lapse
  double latitude = 51, longitude = 10;   // place for which sun and moon are computed
  float lapseMinutes = 10;          // time lapse: one day takes this many minutes
  double lapseTime = 0;             // time lapse: the time it currently shows, seconds since 1970
  Heaven heaven;                    // what was last sent to the renderer
  bool heavenSet = false;
  // Live weather: clouds and wind then follow the weather at the place. What was
  // set by hand is kept and stays in the file.
  bool realWeather = false;         // the switch
  bool weatherOn = false;           // and whether it already applies
  float manualClouds = 0.5f, manualWind = 0.5f;
  WeatherFeed feed;
  // The place: its name if it was found by name, and a search under way.
  std::wstring place;
  PlaceSearch search;
  std::wstring placeState;          // shown below the field while searching or after a failed search
  bool goalSet = false;             // a report has already arrived
  float goalClouds = 0, goalWind = 0;   // the sliders glide to these values
  bool dragMoon = false;            // the grip holds the moon, not the sun

  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGISwapChain1> skyChain, lightChain;
  ComPtr<ID3D11RenderTargetView> skyRtv, lightRtv;
  ComPtr<IDCompositionDevice> dcomp;
  ComPtr<IDCompositionTarget> dtarget;
  ComPtr<IDCompositionVisual> dvisual;
  Renderer render;

  HWND host = nullptr, sky = nullptr, light = nullptr;
  HWND iconView = nullptr;   // the icon view of the desktop; the sky belongs directly behind it
  HWND grips[2] = {};        // invisible grips over sun (0) and moon (1), for dragging
  int gripSize[2] = {};      // diameter with which each was last drawn
  POINT gripAt[2] = {};      // its top-left corner at that time, in device pixels of the screen
  bool dragging = false;     // sun or moon is currently attached to the mouse pointer
  bool gripDue = false;      // the grips have to be reordered and redrawn
  POINT dragOffset{};        // distance of the pointer from the sun center when it was grabbed
  POINT dragStart{};         // where it was grabbed
  HWINEVENTHOOK hook = nullptr;
  NOTIFYICONDATAW tray{};
  UINT taskbarCreated = 0;

  // The icon mask is drawn by a separate thread: fetching the images from the
  // shell takes time, and the desktop must not stall meanwhile.
  std::thread iconThread;
  std::mutex iconLock;
  std::vector<uint8_t> iconMask;   // finished mask, under iconLock
  std::atomic<bool> iconReady{false}, iconStop{false};
  double iconMs = 0;
  bool sceneEvent = true;   // a window has moved, the positions have to be re-read
  bool placing = false;     // the next click places the sun
  bool paused = false;
  bool covered = false;     // a full-screen application is in front, the light is idle
  bool skyUp = false;       // the sky window is shown
  std::vector<Occluder> rects;   // windows and taskbar as last read
  double freeSince = 0;     // since when the desktop has been uncovered
  bool lookArmed = false;   // a control image has been requested
  bool spotDue = false;     // Ctrl+Alt+Shift+B: capture the spot under the mouse pointer
  // The mouse pointer: shape and position as last seen.
  HCURSOR cursorHandle = nullptr;
  int cursorW = 0, cursorH = 0;   // area of the shape including the free margin, device pixels
  POINT cursorHot{}, cursorAt{};
  bool cursorOn = false;
  double cursorSeen = 0;
  float cursorCalm = 1;           // 1 at rest, 0 during fast movement
  bool hdr = false;         // the display runs in HDR mode, Windows blends linearly there
  float white = 1;          // brightness of white in the HDR image, in multiples of 80 cd/m²
  std::wstring lookFlag;    // the file that requests it: look.txt next to the program
  bool rebuild = false;     // display or shell have changed, rebuild everything
  bool quit = false;
  double probe = 0;         // probe run: run this many seconds, no icon in the notification area, then exit
};

App* app = nullptr;

void logf(const wchar_t* format, ...) {
  if (!app || app->logFile.empty()) return;
  FILE* f = nullptr;
  if (_wfopen_s(&f, app->logFile.c_str(), L"a, ccs=UTF-8") || !f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fwprintf(f, L"%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
  va_list args;
  va_start(args, format);
  vfwprintf(f, format, args);
  va_end(args);
  fputwc(L'\n', f);
  fclose(f);
}

double now() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) / double(freq.QuadPart);
}

// Seconds since 1.1.1970, UTC.
double clockNow() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return ((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 1e7 - 11644473600.0;
}

// The place that Windows knows as the user's region: the center of the country.
// That is enough as a start for the course of the sun; the settings allow more
// precision.
void guessPlace(double& latitude, double& longitude) {
  wchar_t name[16] = L"", text[32] = L"";
  if (GetUserDefaultGeoName(name, 16) <= 0) return;
  if (GetGeoInfoEx(name, GEO_LATITUDE, text, 32) > 0) latitude = _wtof(text);
  if (GetGeoInfoEx(name, GEO_LONGITUDE, text, 32) > 0) longitude = _wtof(text);
}

// ---- Settings: a small text file that takes effect as soon as it is saved ----

FILETIME fileTime(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d);
  return d.ftLastWriteTime;
}

float readNumber(const wchar_t* key, float fallback) {
  wchar_t text[64];
  GetPrivateProfileStringW(L"Sunbeam", key, L"", text, 64, app->ini.c_str());
  if (!text[0]) return fallback;
  for (wchar_t* c = text; *c; c++) {
    if (*c == L',') *c = L'.';
  }
  return float(_wtof(text));
}

void writeNumber(const wchar_t* key, float v) {
  wchar_t text[64];
  swprintf_s(text, L"%.3g", v);
  WritePrivateProfileStringW(L"Sunbeam", key, text, app->ini.c_str());
}

// Coordinates keep two decimals, about a kilometer.
void writeDegrees(const wchar_t* key, double v) {
  wchar_t text[64];
  swprintf_s(text, L"%.2f", v);
  WritePrivateProfileStringW(L"Sunbeam", key, text, app->ini.c_str());
}

void saveSettings();

void loadSettings() {
  Params before = app->par;
  Params p;
  for (const Setting& s : SETTINGS) p.*s.field = readNumber(s.key, p.*s.field);
  // A file from an earlier version: whatever has been retuned since then starts
  // with the new value instead of the old saved one.
  bool old = int(readNumber(L"Level", 1)) < SETTINGS_LEVEL;
  if (old) {
    Params fresh;
    p.overWindows = fresh.overWindows;
    p.overSky = fresh.overSky;
    p.shade = fresh.shade;
    p.wind = fresh.wind;
  }
  p.steps = int(readNumber(L"Steps", float(p.steps)));
  p.density = std::clamp(p.density, 0.0f, 1.5f);
  p.mist = std::clamp(p.mist, 0.0f, 1.0f);
  p.length = std::clamp(p.length, 0.1f, 0.9f);
  p.spread = std::clamp(p.spread, 0.0f, 0.9f);
  p.g = std::clamp(p.g, 0.3f, 0.95f);
  p.dust = std::clamp(p.dust, 0.0f, 1.0f);
  p.sunRadius = p.sunRadius <= 0 ? 0 : std::clamp(p.sunRadius, 6.0f, 60.0f);
  p.moonRadius = p.moonRadius <= 0 ? 0 : std::clamp(p.moonRadius, 8.0f, 90.0f);
  p.rays = std::clamp(p.rays, 0.0f, 1.0f);
  p.clouds = std::clamp(p.clouds, 0.0f, 2.0f);
  p.wind = std::clamp(p.wind, 0.0f, 1.0f);
  p.exposure = std::clamp(p.exposure, 0.1f, 4.0f);
  p.bloom = std::clamp(p.bloom, 0.0f, 2.0f);
  p.rim = std::clamp(p.rim, 0.0f, 2.0f);
  p.overWindows = std::clamp(p.overWindows, 0.0f, 1.0f);
  p.overSky = std::clamp(p.overSky, 0.0f, 1.0f);
  p.shade = std::clamp(p.shade, 0.0f, 1.0f);
  p.pointer = std::clamp(p.pointer, 0.0f, 1.5f);
  p.resolution = std::clamp(p.resolution, 0.25f, 1.0f);
  p.steps = std::clamp(p.steps, 8, 128);
  app->par = p;
  p.stars = std::clamp(p.stars, 0.0f, 2.0f);
  p.moonlight = std::clamp(p.moonlight, 0.0f, 3.0f);
  p.glow = std::clamp(p.glow, 1.0f, 6.0f);
  p.life = std::clamp(p.life, 0.0f, 1.0f);
  app->par = p;
  // With live weather, what is in the file counts as set by hand; the sliders stay
  // where the weather has put them.
  app->realWeather = readNumber(L"RealWeather", 0) > 0.5f;
  app->manualClouds = p.clouds, app->manualWind = p.wind;
  if (app->realWeather && app->weatherOn && app->goalSet) app->par.clouds = before.clouds, app->par.wind = before.wind;
  app->sunX = std::clamp(readNumber(L"SunX", app->sunX), 0.0f, 1.0f);
  app->sunY = std::clamp(readNumber(L"SunY", app->sunY), 0.0f, 1.0f);
  app->course = std::clamp(int(readNumber(L"Course", float(app->course))), 0, 2);
  app->latitude = std::clamp(double(readNumber(L"Latitude", float(app->latitude))), -89.0, 89.0);
  app->longitude = std::clamp(double(readNumber(L"Longitude", float(app->longitude))), -180.0, 180.0);
  app->lapseMinutes = std::clamp(readNumber(L"TimeLapse", app->lapseMinutes), 0.5f, 240.0f);
  wchar_t name[160];
  GetPrivateProfileStringW(L"Sunbeam", L"Place", L"", name, 160, app->ini.c_str());
  app->place = name;
  if (old) saveSettings();
  app->iniTime = fileTime(app->ini);
}

void saveSettings() {
  writeNumber(L"Level", float(SETTINGS_LEVEL));
  writeNumber(L"SunX", app->sunX);
  writeNumber(L"SunY", app->sunY);
  writeNumber(L"Course", float(app->course));
  writeDegrees(L"Latitude", app->latitude);
  writeDegrees(L"Longitude", app->longitude);
  WritePrivateProfileStringW(L"Sunbeam", L"Place", app->place.c_str(), app->ini.c_str());
  writeNumber(L"TimeLapse", app->lapseMinutes);
  for (const Setting& s : SETTINGS) writeNumber(s.key, app->par.*s.field);
  writeNumber(L"Steps", float(app->par.steps));
  writeNumber(L"RealWeather", app->realWeather ? 1.0f : 0.0f);
  if (app->weatherOn) {
    writeNumber(L"Clouds", app->manualClouds);
    writeNumber(L"Wind", app->manualWind);
  }
  app->iniTime = fileTime(app->ini);
}

// The place for the course of the sun and the weather.
void setPlace(const Place& p) {
  app->latitude = std::clamp(p.latitude, -89.0, 89.0);
  app->longitude = std::clamp(p.longitude, -180.0, 180.0);
  app->place = p.name;
  app->placeState.clear();
  saveSettings();
  app->sceneEvent = true;
  logf(L"Place changed");
}

// Text typed into the location field: coordinates apply at once, a name is
// looked up in the background.
void enterPlace(const std::wstring& text) {
  Place p;
  if (parseCoordinates(text, p)) {
    setPlace(p);
    return;
  }
  app->search.start(text);
  app->placeState = L"Searching";
}

// Takes the result of a finished search. Returns whether the place changed.
bool pollPlace() {
  bool found = false;
  Place p;
  std::string error;
  if (!app->search.take(found, p, error)) return false;
  if (found) {
    setPlace(p);
  } else {
    app->placeState = error.empty() ? L"Not found" : L"No connection";
  }
  return found;
}

// Turns live weather on or off when the switch has been flipped.
void syncWeather() {
  if (app->realWeather == app->weatherOn) return;
  app->weatherOn = app->realWeather;
  app->goalSet = false;
  if (app->weatherOn) {
    app->manualClouds = app->par.clouds, app->manualWind = app->par.wind;
    app->feed.start(app->latitude, app->longitude);
    logf(L"Live weather on");
  } else {
    app->feed.stop();
    app->par.clouds = app->manualClouds, app->par.wind = app->manualWind;
    logf(L"Live weather off");
  }
}

// Once per second: if a new report has arrived, clouds and wind glide to it, in
// just under a minute from one end of the slider to the other.
void tickWeather() {
  if (!app->weatherOn) return;
  // The same place again changes nothing; if it was changed, the fetch starts for
  // the new one.
  app->feed.start(app->latitude, app->longitude);
  WeatherNow w;
  if (app->feed.take(w)) {
    weatherSliders(w, app->goalClouds, app->goalWind);
    bool first = !app->goalSet;
    app->goalSet = true;
    logf(L"Weather: cloud cover %.0f %%, precipitation %.1f mm/h, wind %.1f m/s, gusts %.1f m/s, code %d; clouds %.2f, wind %.2f", w.cover * 100,
         w.rain, w.wind, w.gust, w.code, app->goalClouds, app->goalWind);
    // As long as the sky is not visible at all yet, it applies immediately.
    if (first && !app->skyUp) {
      app->par.clouds = app->goalClouds, app->par.wind = app->goalWind;
      app->render.setParams(app->par);
      return;
    }
  }
  if (!app->goalSet) return;
  float clouds = app->par.clouds + std::clamp(app->goalClouds - app->par.clouds, -0.03f, 0.03f);
  float wind = app->par.wind + std::clamp(app->goalWind - app->par.wind, -0.02f, 0.02f);
  if (clouds == app->par.clouds && wind == app->par.wind) return;
  app->par.clouds = clouds, app->par.wind = wind;
  app->render.drift(clouds, wind);
}

// Places sun and moon in the sky: where they were put by hand, where they are by
// the clock, or in time lapse. Returns whether anything has changed.
bool updateHeaven(double t) {
  static double last = 0;
  static int course = -1;
  double dt = last > 0 ? std::clamp(t - last, 0.0, 5.0) : 0;
  last = t;
  double clock = clockNow();
  // The time lapse starts with the current time every time.
  if (course != app->course) app->lapseTime = 0;
  course = app->course;
  if (app->course == TIME_LAPSE) {
    if (app->lapseTime <= 0) app->lapseTime = clock;
    app->lapseTime += dt * 1440.0 / app->lapseMinutes;
  } else {
    app->lapseTime = 0;
  }
  float vw = app->render.viewW(), vh = app->render.viewH();
  Heaven h;
  bool gentle = true;
  if (app->course == BY_HAND) {
    // The phase of the moon is today's; it changes so slowly that a look every ten
    // minutes is enough.
    static Bodies today;
    static double seen = -1e9;
    if (clock - seen > 600) {
      today = bodiesAt(clock, app->latitude, app->longitude);
      seen = clock;
    }
    h = heavenOfHand(app->sunX * vw, app->sunY * vh, today, app->render);
    gentle = app->heavenSet && h.sunX == app->heaven.sunX && h.sunY == app->heaven.sunY;
  } else {
    double when = app->course == TIME_LAPSE ? app->lapseTime : clock;
    h = heavenOfBodies(bodiesAt(when, app->latitude, app->longitude), app->latitude, app->render);
    // By the clock, sun and moon move so slowly that a new frame is only worthwhile
    // after a fraction of a pixel.
    if (app->heavenSet && std::hypot(h.sunX - app->heaven.sunX, h.sunY - app->heaven.sunY) < 0.2f &&
        std::hypot(h.moonX - app->heaven.moonX, h.moonY - app->heaven.moonY) < 0.2f) {
      return false;
    }
  }
  if (app->heavenSet && h == app->heaven) return false;
  app->heaven = h;
  app->render.setHeaven(h, gentle && app->heavenSet);
  app->heavenSet = true;
  return true;
}

void setCourse(int course) {
  if (course == app->course) return;
  app->course = course;
  app->lapseTime = 0;
  saveSettings();
  static const wchar_t* names[] = {L"fixed", L"by clock", L"time lapse"};
  logf(L"Sun course: %s", names[course]);
  app->sceneEvent = true;
}

// ---- Start with Windows ----

const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool autostart() {
  DWORD size = 0;
  return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"Sunbeam", RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

void setAutostart(bool on) {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
  if (on) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring value = L"\"" + std::wstring(exe) + L"\"";
    RegSetValueExW(key, L"Sunbeam", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, L"Sunbeam");
  }
  RegCloseKey(key);
}

// ---- Icon in the notification area: a small drawn sun ----

HICON makeSunIcon(int size) {
  BITMAPV5HEADER bi = {};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = size;
  bi.bV5Height = -size;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00ff0000;
  bi.bV5GreenMask = 0x0000ff00;
  bi.bV5BlueMask = 0x000000ff;
  bi.bV5AlphaMask = 0xff000000;
  uint32_t* px = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0);
  ReleaseDC(nullptr, screen);
  float c = (size - 1) / 2.0f;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      float dx = x - c, dy = y - c, r = std::sqrt(dx * dx + dy * dy) / (size / 2.0f);
      float disc = std::clamp((0.47f - r) * size / 2.0f + 0.5f, 0.0f, 1.0f);
      // Eight rays around the disk.
      float ang = std::atan2(dy, dx), ray = std::abs(std::fmod(std::abs(ang) / 0.7853982f + 0.5f, 1.0f) - 0.5f);
      float beam = std::clamp((0.11f - ray * r) * size, 0.0f, 1.0f) * std::clamp((r - 0.6f) * size / 2.0f, 0.0f, 1.0f) *
                   std::clamp((0.97f - r) * size / 2.0f, 0.0f, 1.0f);
      float a = std::max(disc, beam);
      uint32_t A = uint32_t(a * 255), R = uint32_t(255 * a), G = uint32_t(185 * a), B = uint32_t(56 * a);
      px[y * size + x] = (A << 24) | (R << 16) | (G << 8) | B;
    }
  }
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO ii = {TRUE, 0, 0, mask, color};
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon;
}

void addTray() {
  NOTIFYICONDATAW& t = app->tray;
  t = {};
  t.cbSize = sizeof(t);
  t.hWnd = app->host;
  t.uID = 1;
  t.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  t.uCallbackMessage = WM_TRAY;
  t.hIcon = makeSunIcon(GetSystemMetrics(SM_CXSMICON));
  wcscpy_s(t.szTip, L"Sunbeam");
  Shell_NotifyIconW(NIM_ADD, &t);
}

// ---- Windows ----

void showLight(bool on) {
  if (app->light) ShowWindow(app->light, on ? SW_SHOWNOACTIVATE : SW_HIDE);
}

// Places the sky directly behind the icon view and shows it on request. Showing
// alone is not enough: it can then move in front of the icons, cover them and
// intercept the mouse.
void placeSky(bool show) {
  if (!app->sky) return;
  SetWindowPos(app->sky, app->iconView ? app->iconView : HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
}

// Is the sky still directly behind the icon view?
bool skyInPlace() {
  return !app->iconView || !app->sky || GetWindow(app->iconView, GW_HWNDNEXT) == app->sky;
}

// Gives a grip its content: a disk with coverage 1 of 255, nothing outside. This
// is not visible, but where there is coverage it accepts the mouse. Where a
// desktop icon is below it, it stays open: the click there belongs to the icon.
// left, top: its position on the screen in device pixels.
void paintGrip(int body, int d, int left, int top) {
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = d;
  bi.bmiHeader.biHeight = -d;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  uint32_t* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
  if (bmp && bits) {
    float c = (d - 1) / 2.0f;
    int w = app->mon.width(), h = app->mon.height();
    {
      std::lock_guard<std::mutex> hold(app->iconLock);
      bool icons = app->iconMask.size() == size_t(w) * h;
      for (int y = 0; y < d; y++) {
        for (int x = 0; x < d; x++) {
          bool inside = (x - c) * (x - c) + (y - c) * (y - c) <= c * c;
          int sx = left + x, sy = top + y;
          bool icon = icons && sx >= 0 && sy >= 0 && sx < w && sy < h && app->iconMask[size_t(sy) * w + sx] > 127;
          bits[size_t(y) * d + x] = inside && !icon ? 0x01000000u : 0u;
        }
      }
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    SIZE size = {d, d};
    POINT from = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(app->grips[body], screen, nullptr, &size, mem, &from, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    app->gripSize[body] = d;
    app->gripAt[body] = {left, top};
  }
  if (bmp) DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

// Is a grip in the desktop in front of the icon view? Behind it, the icon view
// would intercept every click.
bool gripInFront(HWND grip) {
  for (HWND c = GetWindow(GetAncestor(grip, GA_PARENT), GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
    if (c == grip) return true;
    if (c == app->iconView) return false;
  }
  return false;
}

// The grips: small invisible windows exactly over sun and moon. They are in the
// desktop in front of the icon view and therefore below all program windows.
// What is in the image can be grabbed: the sun even when it has set behind the
// mountains (if it was dragged there, it can be brought back this way), the moon
// as soon as it is above the horizon. repaint: the icons have changed, the grips
// have to be redrawn.
void placeGrip(bool repaint = false) {
  if (app->dragging) return;
  int w = app->mon.width(), h = app->mon.height();
  float s = app->mon.scale;
  for (int body = 0; body < 2; body++) {
    HWND grip = app->grips[body];
    if (!grip) continue;
    bool moon = body == 1;
    float bx = moon ? app->heaven.moonX : app->heaven.sunX, by = moon ? app->heaven.moonY : app->heaven.sunY;
    float disc = moon ? app->render.moonDiscRadius() : app->render.sunDiscRadius();
    int r = int(std::lround(std::max(40.0f, disc + 14) * s));
    int cx = int(std::lround(bx * s)), cy = int(std::lround(by * s));
    bool there = app->heavenSet && cx > 0 && cx < w && cy > 0 && cy < h - int(std::lround(10 * s));
    if (moon) there = there && app->heaven.moonLit > 0.001f && by < app->render.horizonRow();
    if (!there || app->paused || app->placing || !app->skyUp) {
      ShowWindow(grip, SW_HIDE);
      continue;
    }
    int left = cx - r, top = cy - r;
    bool moved = left != app->gripAt[body].x || top != app->gripAt[body].y || 2 * r != app->gripSize[body];
    if (moved) {
      POINT at = {app->mon.rect.left + left, app->mon.rect.top + top};
      ScreenToClient(GetAncestor(grip, GA_PARENT), &at);
      SetWindowPos(grip, HWND_TOP, at.x, at.y, 2 * r, 2 * r, SWP_NOACTIVATE);
    }
    if (moved || repaint) paintGrip(body, 2 * r, left, top);
    if (!IsWindowVisible(grip) || !gripInFront(grip)) SetWindowPos(grip, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
}

void setPlacing(bool on) {
  app->placing = on;
  placeGrip();
  if (!app->light) return;
  // For placing, the light window briefly accepts clicks; otherwise it lets
  // everything through.
  LONG_PTR ex = GetWindowLongPtrW(app->light, GWL_EXSTYLE);
  SetWindowLongPtrW(app->light, GWL_EXSTYLE, on ? ex & ~WS_EX_TRANSPARENT : ex | WS_EX_TRANSPARENT);
  if (on) showLight(true);
}

void openSettings() {
  PanelHost h;
  h.par = &app->par;
  h.course = &app->course;
  h.latitude = &app->latitude;
  h.longitude = &app->longitude;
  h.lapseMinutes = &app->lapseMinutes;
  h.realWeather = &app->realWeather;
  h.place = [] { return app->place.empty() ? coordinateText(app->latitude, app->longitude) : app->place; };
  h.placeNote = [] {
    if (!app->placeState.empty()) return app->placeState;
    return app->place.empty() ? std::wstring() : coordinateText(app->latitude, app->longitude);
  };
  h.setPlace = enterPlace;
  h.render = &app->render;
  h.icon = app->tray.hIcon;
  h.shownTime = [] { return app->course == TIME_LAPSE && app->lapseTime > 0 ? app->lapseTime : clockNow(); };
  h.changed = [](bool done) {
    syncWeather();
    app->render.setParams(app->par);
    app->sceneEvent = true;
    if (done) saveSettings();
  };
  h.autostart = autostart;
  h.setAutostart = setAutostart;
  openPanel(app->inst, h);
}

void trayMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, M_PLACE, L"Move sun");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, M_FIXED, L"Sun fixed");
  AppendMenuW(m, MF_STRING, M_CLOCK, L"Sun follows clock");
  AppendMenuW(m, MF_STRING, M_LAPSE, L"Time lapse");
  CheckMenuRadioItem(m, M_FIXED, M_LAPSE, M_FIXED + app->course, MF_BYCOMMAND);
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, M_PAUSE, app->paused ? L"Resume" : L"Pause");
  AppendMenuW(m, MF_STRING, M_SETTINGS, L"Settings");
  AppendMenuW(m, MF_STRING | (autostart() ? MF_CHECKED : 0), M_AUTOSTART, L"Start with Windows");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, M_QUIT, L"Quit");
  POINT p;
  GetCursorPos(&p);
  SetForegroundWindow(app->host);
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, app->host, nullptr);
  DestroyMenu(m);
  switch (cmd) {
    case M_PLACE:
      if (!app->paused) setPlacing(true);
      break;
    case M_FIXED:
    case M_CLOCK:
    case M_LAPSE:
      setCourse(cmd - M_FIXED);
      break;
    case M_PAUSE:
      app->paused = !app->paused;
      if (app->paused) ShowWindow(app->sky, SW_HIDE);
      else placeSky(true);
      placeGrip();
      showLight(!app->paused && !app->covered);
      app->sceneEvent = true;
      break;
    case M_SETTINGS:
      openSettings();
      break;
    case M_AUTOSTART:
      setAutostart(!autostart());
      break;
    case M_QUIT:
      app->quit = true;
      break;
  }
}

LRESULT CALLBACK hostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (app && msg == app->taskbarCreated && msg != 0) {
    app->rebuild = true;   // Explorer has restarted: attach icon and sky again
    return 0;
  }
  switch (msg) {
    case WM_TRAY:
      // Left opens the settings, right the menu.
      if (LOWORD(lp) == WM_LBUTTONUP) openSettings();
      else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) trayMenu();
      return 0;
    case WM_HOTKEY:
      if (wp == HOTKEY_QUIT) app->quit = true;
      if (wp == HOTKEY_SPOT) app->spotDue = true;
      return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      app->rebuild = true;
      return 0;
    case WM_ENDSESSION:
    case WM_CLOSE:
      app->quit = true;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK lightProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCHITTEST:
      return app && app->placing ? HTCLIENT : HTTRANSPARENT;
    case WM_SETCURSOR:
      if (app && app->placing) {
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN:
      if (app && app->placing) {
        app->sunX = std::clamp(float(GET_X_LPARAM(lp)) / app->mon.width(), 0.0f, 1.0f);
        app->sunY = std::clamp(float(GET_Y_LPARAM(lp)) / app->mon.height(), 0.0f, 1.0f);
        setPlacing(false);
        app->course = BY_HAND;
        saveSettings();
        app->sceneEvent = true;
      }
      return 0;
    case WM_RBUTTONDOWN:
    case WM_KEYDOWN:
      if (app && app->placing) setPlacing(false);
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// A grip accepts the mouse: click and hold grabs sun or moon, they follow the
// pointer, and on release they stay where they are.
LRESULT CALLBACK gripProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (!app) return DefWindowProcW(h, msg, wp, lp);
  switch (msg) {
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
      return TRUE;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;   // draw nothing
    case WM_PAINT:
      ValidateRect(h, nullptr);
      return 0;
    case WM_LBUTTONDOWN: {
      POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ClientToScreen(h, &p);
      float s = app->mon.scale;
      app->dragMoon = h == app->grips[1];
      float bx = app->dragMoon ? app->heaven.moonX : app->heaven.sunX, by = app->dragMoon ? app->heaven.moonY : app->heaven.sunY;
      app->dragOffset = {p.x - app->mon.rect.left - LONG(std::lround(bx * s)), p.y - app->mon.rect.top - LONG(std::lround(by * s))};
      app->dragStart = p;
      app->dragging = true;
      SetCapture(h);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (app->dragging) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(h, &p);
        // If the sun follows the clock, it keeps doing so as long as there are only
        // clicks; only when it is actually dragged away does it switch to by hand.
        if (app->course != BY_HAND && std::abs(p.x - app->dragStart.x) + std::abs(p.y - app->dragStart.y) < LONG(8 * app->mon.scale)) return 0;
        float x = float(p.x - app->dragOffset.x - app->mon.rect.left) / app->mon.scale;
        float y = float(p.y - app->dragOffset.y - app->mon.rect.top) / app->mon.scale;
        // Dragging the moon moves the sun, which is opposite to it.
        if (app->dragMoon) sunOfMoon(x, y, app->render, x, y);
        app->sunX = std::clamp(x / app->render.viewW(), 0.02f, 0.98f);
        app->sunY = std::clamp(y / app->render.viewH(), 0.03f, 1.0f);
        if (app->course != BY_HAND) setCourse(BY_HAND);
        app->sceneEvent = true;
      }
      return 0;
    case WM_LBUTTONUP:
      if (app->dragging) ReleaseCapture();
      return 0;
    case WM_CAPTURECHANGED:
      if (app->dragging) {
        app->dragging = false;
        saveSettings();
        app->gripDue = true;
        app->sceneEvent = true;
        static int told = 0;
        if (told++ < 20) logf(L"%s dragged by its grip, sun now at %.3f %.3f", app->dragMoon ? L"Moon" : L"Sun", app->sunX, app->sunY);
      }
      return 0;
    case WM_RBUTTONUP:
      trayMenu();
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK skyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;   // clicks belong to the desktop below
  if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return DefWindowProcW(h, msg, wp, lp);
}

// ---- Mouse pointer ----

const int POINTER_PAD = 16;   // free margin around the shape so that its penumbra has room

// Reads the shape of the mouse pointer as coverage, one byte per device pixel,
// with a free margin all around. For this the pointer is drawn once on black and
// once on white: where both results are equal it is opaque; where nothing of it
// remains it is transparent. This holds for every kind of pointer.
bool readPointerShape(HCURSOR cursor, std::vector<uint8_t>& cover, int& w, int& h, POINT& hot) {
  ICONINFO ii = {};
  if (!GetIconInfo(cursor, &ii)) return false;
  BITMAP bm = {};
  GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
  int cw = bm.bmWidth, ch = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  if (cw <= 0 || ch <= 0 || cw > 256 || ch > 256) return false;
  hot = {LONG(ii.xHotspot), LONG(ii.yHotspot)};
  w = cw + 2 * POINTER_PAD;
  h = ch + 2 * POINTER_PAD;
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  HDC dc = CreateCompatibleDC(nullptr);
  uint32_t* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
  bool ok = bmp && bits;
  if (ok) {
    HGDIOBJ old = SelectObject(dc, bmp);
    size_t n = size_t(w) * h;
    std::fill(bits, bits + n, 0u);
    DrawIconEx(dc, POINTER_PAD, POINTER_PAD, cursor, cw, ch, 0, nullptr, DI_NORMAL);
    GdiFlush();
    std::vector<uint32_t> onBlack(bits, bits + n);
    std::fill(bits, bits + n, 0x00ffffffu);
    DrawIconEx(dc, POINTER_PAD, POINTER_PAD, cursor, cw, ch, 0, nullptr, DI_NORMAL);
    GdiFlush();
    cover.resize(n);
    for (size_t i = 0; i < n; i++) {
      int dark = (onBlack[i] >> 8) & 255, light = (bits[i] >> 8) & 255;
      cover[i] = uint8_t(std::clamp(255 - (light - dark), 0, 255));
    }
    SelectObject(dc, old);
  }
  if (bmp) DeleteObject(bmp);
  DeleteDC(dc);
  return ok;
}

// Reports shape and position of the mouse pointer to the renderer. Returns
// whether anything has changed and a new frame is due.
bool trackPointer(double t) {
  CURSORINFO ci = {sizeof(ci)};
  bool on = app->par.pointer > 0 && GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
  bool changed = false;
  if (on && ci.hCursor != app->cursorHandle) {
    std::vector<uint8_t> cover;
    int w = 0, h = 0;
    POINT hot{};
    if (readPointerShape(ci.hCursor, cover, w, h, hot)) {
      app->render.setCursorShape(cover.data(), w, h);
      app->cursorW = w;
      app->cursorH = h;
      app->cursorHot = hot;
      static int told = 0;
      if (told++ < 3) logf(L"Pointer: %d x %d pixels, hot spot at %ld %ld", w - 2 * POINTER_PAD, h - 2 * POINTER_PAD, hot.x, hot.y);
    } else {
      app->cursorW = 0;
    }
    app->cursorHandle = ci.hCursor;
    changed = true;
  }
  on = on && app->cursorW > 0;
  if (!on) {
    if (!app->cursorOn) return changed;
    app->render.setCursor(false, 0, 0, 0, 0, 1);
    app->cursorOn = false;
    return true;
  }
  POINT at = ci.ptScreenPos;
  double dt = std::max(t - app->cursorSeen, 1e-3);
  if (!app->cursorOn || at.x != app->cursorAt.x || at.y != app->cursorAt.y) {
    // During fast movement the light visibly lags behind the pointer; the fringe
    // that lies close to it is then suspended.
    double speed = app->cursorOn ? std::hypot(double(at.x - app->cursorAt.x), double(at.y - app->cursorAt.y)) / app->mon.scale / dt : 0;
    double fast = std::clamp((speed - 500) / 1500, 0.0, 1.0);
    app->cursorCalm = std::min(app->cursorCalm, float(1 - fast * fast * (3 - 2 * fast)));
    changed = true;
  } else if (app->cursorCalm < 1) {
    app->cursorCalm = std::min(1.0f, app->cursorCalm + float(dt) * 5);
    changed = true;
  }
  app->cursorAt = at;
  app->cursorSeen = t;
  app->cursorOn = true;
  if (changed) {
    float s = app->mon.scale;
    float x0 = (at.x - app->mon.rect.left - app->cursorHot.x - POINTER_PAD) / s;
    float y0 = (at.y - app->mon.rect.top - app->cursorHot.y - POINTER_PAD) / s;
    app->render.setCursor(true, x0, y0, x0 + app->cursorW / s, y0 + app->cursorH / s, app->cursorCalm);
  }
  return changed;
}

void CALLBACK onWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
  if (!app || object != OBJID_WINDOW || !hwnd) return;
  // Only top-level windows count; sliders, lists and the like move all the time.
  if (event == EVENT_OBJECT_LOCATIONCHANGE && GetAncestor(hwnd, GA_ROOT) != hwnd) return;
  app->sceneEvent = true;
}

void registerClasses() {
  WNDCLASSEXW c = {sizeof(c)};
  c.hInstance = app->inst;
  c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  c.lpfnWndProc = hostProc;
  c.lpszClassName = HOST_CLASS;
  RegisterClassExW(&c);
  c.lpfnWndProc = skyProc;
  c.lpszClassName = SKY_CLASS;
  RegisterClassExW(&c);
  c.lpfnWndProc = lightProc;
  c.lpszClassName = LIGHT_CLASS;
  RegisterClassExW(&c);
  c.lpfnWndProc = gripProc;
  c.lpszClassName = GRIP_CLASS;
  RegisterClassExW(&c);
}

// Asks Windows whether the main display runs in HDR mode and how bright white is
// shown there. In HDR mode Windows composes the image linearly and accepts light
// without coverage only from a swap chain in HDR format; from one with 8 bits it
// is dropped (not in screenshots, which therefore look different from the
// display).
void readDisplay(bool& hdr, float& white) {
  hdr = false;
  white = 1;
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
  UINT32 np = 0, nm = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return;
  for (UINT32 i = 0; i < np; i++) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name = {};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = paths[i].sourceInfo.adapterId;
    name.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || wcscmp(name.viewGdiDeviceName, mi.szDevice)) continue;
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
    color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    color.header.size = sizeof(color);
    color.header.adapterId = paths[i].targetInfo.adapterId;
    color.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) hdr = color.advancedColorEnabled != 0;
    DISPLAYCONFIG_SDR_WHITE_LEVEL level = {};
    level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    level.header.size = sizeof(level);
    level.header.adapterId = paths[i].targetInfo.adapterId;
    level.header.id = paths[i].targetInfo.id;
    if (hdr && DisplayConfigGetDeviceInfo(&level.header) == ERROR_SUCCESS && level.SDRWhiteLevel > 0) white = level.SDRWhiteLevel / 1000.0f;
    return;
  }
}

// Attaches the sky window behind the desktop icons.
bool attachSky() {
  HWND progman = FindWindowW(L"Progman", nullptr);
  if (!progman) return false;
  // Asks the shell to create the layer between wallpaper and icons.
  SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, nullptr);

  HWND parent = nullptr, above = nullptr;
  HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
  HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
  if (def && worker) {
    // Since Windows 11 24H2, icon view and background are children of Progman. The
    // sky goes in between.
    parent = progman;
    above = def;
  } else {
    // Before that, the icon view is in a WorkerW of its own; the next WorkerW behind
    // it carries the wallpaper and takes the sky.
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr;) {
      if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) {
        parent = FindWindowExW(nullptr, w, L"WorkerW", nullptr);
        break;
      }
    }
  }
  logf(L"Desktop: Progman %p, icon view %p, WorkerW %p, sky goes into %p", progman, def, worker, parent);
  if (!parent) return false;

  // Attach as a real child window. A top-level window that is merely reparented
  // does not keep its order among the siblings.
  // Without WS_CLIPSIBLINGS: otherwise every sibling window in front of it cuts a
  // hole into the sky, including the invisible grip at the sun; the old wallpaper
  // showed through the hole.
  LONG_PTR style = GetWindowLongPtrW(app->sky, GWL_STYLE);
  SetWindowLongPtrW(app->sky, GWL_STYLE, ((style & ~LONG_PTR(WS_POPUP)) | WS_CHILD) & ~LONG_PTR(WS_CLIPSIBLINGS));
  SetParent(app->sky, parent);
  app->iconView = above;
  POINT at = {app->mon.rect.left, app->mon.rect.top};
  ScreenToClient(parent, &at);
  // Do not show yet: the window appears only with the first finished frame.
  SetWindowPos(app->sky, above ? above : HWND_TOP, at.x, at.y, app->mon.width(), app->mon.height(), SWP_NOACTIVATE);
  if (above && worker) SetWindowPos(worker, app->sky, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  HWND below = above ? GetWindow(above, GW_HWNDNEXT) : nullptr;
  // For this window type GetParent returns the owner, hence GetAncestor.
  HWND holder = GetAncestor(app->sky, GA_PARENT);
  logf(L"Sky %p: parent %p, directly behind icon view %d", app->sky, holder, below == app->sky);
  return holder == parent;
}

bool makeRtv(IDXGISwapChain1* chain, ComPtr<ID3D11RenderTargetView>& rtv) {
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  return SUCCEEDED(app->dev->CreateRenderTargetView(back.Get(), nullptr, &rtv));
}

// Reads the desktop icons at intervals and redraws their mask when count,
// position or names have changed.
void iconWorker(Monitor mon) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::vector<uint8_t> mask;
  uint64_t stamp = 0;
  while (!app->iconStop) {
    double before = now();
    if (captureIcons(mon, mask, stamp)) {
      std::lock_guard<std::mutex> hold(app->iconLock);
      app->iconMask = mask;
      app->iconMs = (now() - before) * 1000;
      app->iconReady = true;
    }
    for (int waited = 0; waited < ICONS_EVERY_MS && !app->iconStop; waited += 50) Sleep(50);
  }
  CoUninitialize();
}

void teardown() {
  if (app->iconThread.joinable()) {
    app->iconStop = true;
    app->iconThread.join();
  }
  if (app->hook) UnhookWinEvent(app->hook);
  app->hook = nullptr;
  if (app->tray.hWnd) {
    Shell_NotifyIconW(NIM_DELETE, &app->tray);
    if (app->tray.hIcon) DestroyIcon(app->tray.hIcon);
    app->tray = {};
  }
  app->skyRtv.Reset();
  app->lightRtv.Reset();
  app->dvisual.Reset();
  app->dtarget.Reset();
  app->dcomp.Reset();
  app->skyChain.Reset();
  app->lightChain.Reset();
  for (HWND& grip : app->grips) {
    if (grip) DestroyWindow(grip);
    grip = nullptr;
  }
  app->gripSize[0] = app->gripSize[1] = 0;
  app->dragging = false;
  if (app->light) DestroyWindow(app->light);
  if (app->sky) DestroyWindow(app->sky);
  app->light = app->sky = nullptr;
  app->iconView = nullptr;
  if (app->ctx) {
    app->ctx->ClearState();
    app->ctx->Flush();
  }
}

bool setup() {
  app->mon = primaryMonitor();
  int w = app->mon.width(), h = app->mon.height();
  readDisplay(app->hdr, app->white);
  logf(L"Display %d x %d, scale %.2f, HDR %d, white %.0f cd/m2", w, h, app->mon.scale, app->hdr, app->white * 80);

  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(app->dev.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

  // Sky: a window behind the icons.
  app->sky = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, SKY_CLASS, L"", WS_POPUP, app->mon.rect.left,
                             app->mon.rect.top, w, h, nullptr, nullptr, app->inst, nullptr);
  if (!app->sky) return false;
  SetLayeredWindowAttributes(app->sky, 0, 255, LWA_ALPHA);
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = w;
  sd.Height = h;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  HRESULT hr = factory->CreateSwapChainForHwnd(app->dev.Get(), app->sky, &sd, nullptr, nullptr, &app->skyChain);
  if (FAILED(hr)) {
    logf(L"Sky: cannot create swap chain (%08x)", hr);
    return false;
  }
  factory->MakeWindowAssociation(app->sky, DXGI_MWA_NO_ALT_ENTER);
  if (!makeRtv(app->skyChain.Get(), app->skyRtv)) return false;
  if (!attachSky()) {
    logf(L"Sky: layer behind the icons not found");
    return false;
  }
  // Grip at the sun. Only in the structure since Windows 11 24H2, where sky and
  // icon view are siblings; otherwise "Move sun" stays in the menu.
  if (app->iconView && app->probe <= 0) {
    // A window with per-pixel coverage, like the icon view itself: only this way is
    // it in front of the sky without covering it. (With a fixed coverage for the
    // whole window it stood as a black disk over the sun; with no surface at all it
    // cut a hole into the sky.)
    for (HWND& grip : app->grips) {
      grip = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, GRIP_CLASS, L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, app->inst, nullptr);
      if (!grip) continue;
      LONG_PTR style = GetWindowLongPtrW(grip, GWL_STYLE);
      SetWindowLongPtrW(grip, GWL_STYLE, (style & ~LONG_PTR(WS_POPUP)) | WS_CHILD);
      SetParent(grip, GetAncestor(app->sky, GA_PARENT));
    }
  }

  // Light: a window above everything that lets clicks through and adds to the
  // image.
  app->light = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               LIGHT_CLASS, L"", WS_POPUP, app->mon.rect.left, app->mon.rect.top, w, h, nullptr, nullptr, app->inst, nullptr);
  if (!app->light) {
    logf(L"Light: cannot create window (%u)", GetLastError());
    return false;
  }
  SetLayeredWindowAttributes(app->light, 0, 255, LWA_ALPHA);
  sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  sd.Format = app->hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  hr = factory->CreateSwapChainForComposition(app->dev.Get(), &sd, nullptr, &app->lightChain);
  if (SUCCEEDED(hr) && app->hdr) {
    // Linear, 1.0 = 80 cd/m²: this is how Windows blends in HDR mode without
    // conversion.
    ComPtr<IDXGISwapChain3> wide;
    hr = app->lightChain.As(&wide);
    if (SUCCEEDED(hr)) hr = wide->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
  }
  if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&app->dcomp));
  if (SUCCEEDED(hr)) hr = app->dcomp->CreateTargetForHwnd(app->light, TRUE, &app->dtarget);
  if (SUCCEEDED(hr)) hr = app->dcomp->CreateVisual(&app->dvisual);
  if (SUCCEEDED(hr)) hr = app->dvisual->SetContent(app->lightChain.Get());
  if (SUCCEEDED(hr)) hr = app->dtarget->SetRoot(app->dvisual.Get());
  if (SUCCEEDED(hr)) hr = app->dcomp->Commit();
  if (FAILED(hr) || !makeRtv(app->lightChain.Get(), app->lightRtv)) {
    logf(L"Light: cannot create swap chain or composition (%08x)", hr);
    return false;
  }
  // Start empty: alpha zero means nothing is added.
  float clear[4] = {0, 0, 0, 0};
  app->ctx->ClearRenderTargetView(app->lightRtv.Get(), clear);
  app->lightChain->Present(0, 0);
  ShowWindow(app->light, SW_SHOWNOACTIVATE);

  // Safety: the light window must never swallow clicks. If a point in the center
  // of the image hits the program's own window, it is removed again at once.
  POINT mid = {(app->mon.rect.left + app->mon.rect.right) / 2, (app->mon.rect.top + app->mon.rect.bottom) / 2};
  if (WindowFromPoint(mid) == app->light) {
    logf(L"Light: window does not pass clicks through, removing it");
    return false;
  }

  app->render.resize(w, h, app->mon.scale);
  app->render.setParams(app->par);
  app->render.setOutput(app->hdr, app->white);
  app->heavenSet = false;
  updateHeaven(now());
  app->sceneEvent = true;
  app->covered = false;
  app->skyUp = false;
  app->iconStop = false;
  app->iconReady = false;
  app->cursorHandle = nullptr;
  app->cursorOn = false;
  app->iconThread = std::thread(iconWorker, app->mon);

  if (app->probe <= 0) addTray();
  app->hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_LOCATIONCHANGE, nullptr, onWinEvent, 0, 0,
                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  logf(L"Set up: sky %p, light %p, sun %.0f px", app->sky, app->light, app->render.sunRadius());
  return true;
}

void readScene() {
  Scene s = captureScene(app->mon, {app->sky, app->light, app->host});
  // In full screen and when a maximized window covers the desktop, everything is
  // idle.
  bool covered = (s.fullscreen || s.buried) && !app->placing;
  // New windows that want to stay always on top would otherwise lie over the
  // light.
  static double raised = 0;
  if (!covered && now() - raised > 1.0) {
    raised = now();
    SetWindowPos(app->light, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  app->rects = s.rects;
  if (covered != app->covered) {
    app->covered = covered;
    if (!covered) app->freeSince = now();
    showLight(!covered && !app->paused);
    // The first changes are logged, later ones are not, so that the log stays small.
    static int told = 0;
    if (app->probe > 0 || told++ < 20) logf(L"Desktop covered: %d (full screen %d, maximized window %d)", covered, s.fullscreen, s.buried);
  }
  app->render.setScene(s.rects);
}

// Control image for debugging: what is actually on the screen when the desktop
// is uncovered. The content of windows, the taskbar and the icons are averaged
// into coarse blocks beforehand; nothing can be read in them. It is made only on
// request (a file look.txt next to the program) and only once.
// base: name of the file with the whole image, close: name of the crop at full
// sharpness around the spot (cx, cy) in device pixels.
void saveLook(const wchar_t* base, const wchar_t* close, int cx, int cy) {
  int w = app->mon.width(), h = app->mon.height();
  // The image read is the one that Windows sends to the display; in HDR mode an
  // ordinary screenshot differs from it.
  std::vector<uint8_t> px;
  const wchar_t* way = L"image sent to the display";
  Desk desk;
  bool got = desk.open(app->dev.Get(), app->ctx.Get());
  if (got) {
    desk.refresh(250);
    got = desk.w == w && desk.h == h && desk.readSrgb(app->hdr ? app->white : 1.0f, px);
  }
  if (!got) {
    way = L"screenshot";
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* raw = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (bmp && raw) {
      HGDIOBJ old = SelectObject(mem, bmp);
      BitBlt(mem, 0, 0, w, h, screen, app->mon.rect.left, app->mon.rect.top, SRCCOPY | CAPTUREBLT);
      GdiFlush();
      px.assign(static_cast<uint8_t*>(raw), static_cast<uint8_t*>(raw) + size_t(w) * h * 4);
      SelectObject(mem, old);
      got = true;
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
  }
  bool ok = false;
  if (got) {
    uint32_t* bits = reinterpret_cast<uint32_t*>(px.data());
    // Are the icons actually visible? Where they are, the brightness varies strongly
    // from pixel to pixel, in the sky beside them hardly at all. The mean variation
    // per cell of 16 pixels is measured.
    {
      std::lock_guard<std::mutex> hold(app->iconLock);
      if (app->iconMask.size() == size_t(w) * h) {
        double sum[2] = {};
        int count[2] = {};
        for (int by = 0; by + 16 <= h; by += 16) {
          for (int bx = 0; bx + 16 <= w && bx < w / 4; bx += 16) {
            int set = 0;
            double m = 0, q = 0;
            for (int y = by; y < by + 16; y++) {
              for (int x = bx; x < bx + 16; x++) {
                uint32_t v = bits[size_t(y) * w + x];
                double l = 0.0722 * (v & 255) + 0.7152 * ((v >> 8) & 255) + 0.2126 * ((v >> 16) & 255);
                m += l, q += l * l;
                set += app->iconMask[size_t(y) * w + x] > 127;
              }
            }
            m /= 256, q = std::sqrt(std::max(q / 256 - m * m, 0.0));
            int k = set > 64 ? 0 : set == 0 ? 1 : 2;
            if (k < 2) sum[k] += q, count[k]++;
          }
        }
        logf(L"Icons in the image: variation %.1f in %d cells, in the sky beside them %.1f in %d cells (levels of 255)",
             count[0] ? sum[0] / count[0] : 0.0, count[0], count[1] ? sum[1] / count[1] : 0.0, count[1]);
      }
    }
    // What does the mouse hit on the uncovered desktop? It must be the list of the
    // icons.
    {
      POINT at = {app->mon.rect.left + w * 3 / 4, app->mon.rect.top + h / 5};
      HWND hit = WindowFromPoint(at);
      wchar_t cls[64] = L"";
      if (hit) GetClassNameW(hit, cls, 64);
      logf(L"Mouse at %ld %ld hits: %s", at.x, at.y, cls);
      // Does the pointer shape that was read match the one that Windows draws?
      logf(L"Pointer as Windows reports it: %d x %d at %d %d, hot spot %d %d; as read: %d x %d at %ld %ld, hot spot %ld %ld", desk.pointerW,
           desk.pointerH, desk.pointerX, desk.pointerY, desk.pointerHotX, desk.pointerHotY, app->cursorW - 2 * POINTER_PAD,
           app->cursorH - 2 * POINTER_PAD, app->cursorAt.x - app->cursorHot.x, app->cursorAt.y - app->cursorHot.y, app->cursorHot.x,
           app->cursorHot.y);
    }
    auto block = [&](int x0, int y0, int x1, int y1) {
      uint64_t sum[3] = {};
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          uint32_t v = bits[size_t(y) * w + x];
          sum[0] += v & 255, sum[1] += (v >> 8) & 255, sum[2] += (v >> 16) & 255;
        }
      }
      uint64_t n = uint64_t(x1 - x0) * (y1 - y0);
      uint32_t mean = uint32_t(sum[0] / n) | uint32_t(sum[1] / n) << 8 | uint32_t(sum[2] / n) << 16;
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) bits[size_t(y) * w + x] = mean;
      }
    };
    float s = app->mon.scale;
    for (const Occluder& o : app->rects) {
      int x0 = std::clamp(int(o.x0 * s), 0, w), y0 = std::clamp(int(o.y0 * s), 0, h);
      int x1 = std::clamp(int(o.x1 * s) + 1, 0, w), y1 = std::clamp(int(o.y1 * s) + 1, 0, h);
      for (int y = y0; y < y1; y += 48) {
        for (int x = x0; x < x1; x += 48) block(x, y, std::min(x1, x + 48), std::min(y1, y + 48));
      }
    }
    {
      std::lock_guard<std::mutex> hold(app->iconLock);
      if (app->iconMask.size() == size_t(w) * h) {
        for (int by = 0; by < h; by += 16) {
          for (int bx = 0; bx < w; bx += 16) {
            int x1 = std::min(w, bx + 16), y1 = std::min(h, by + 16);
            bool any = false;
            for (int y = by; y < y1 && !any; y++) {
              for (int x = bx; x < x1 && !any; x++) any = app->iconMask[size_t(y) * w + x] > 0;
            }
            if (any) block(bx, by, x1, y1);
          }
        }
      }
    }
    ok = writeImage(app->dir + L"\\" + base + L".png", px.data(), w, h, 2);
    ok &= writeImage(app->dir + L"\\" + close + L".png", px.data(), w, h, 1, cx - 640, cy - 360, 1280, 720);
  }
  logf(L"Control image %s (%s), %s at %.0f %.0f", ok ? L"written" : L"not written", way, app->render.moonShines() ? L"moon" : L"sun",
       app->render.sourceX(), app->render.sourceY());
  for (const Occluder& o : app->rects) logf(L"  rect %6.0f %6.0f %6.0f %6.0f  kind %d  opacity %.2f", o.x0, o.y0, o.x1, o.y1, o.kind, o.opacity);
}

// The loop, until the program is to exit or to be rebuilt.
void run() {
  double lastDraw = 0, lastScene = 0, lastIni = 0;
  bool dirty = true;
  double begin = now(), spent = 0, worst = 0;
  int frames = 0, skies = 0, lights = 0;
  bool settledTold = false;
  HRESULT skyResult = S_OK, lightResult = S_OK;
  // If water moves in the image, a frame is made on every display refresh.
  bool lively = false;
  while (!app->quit && !app->rebuild) {
    double t = now();
    if (app->probe > 0 && t - begin > app->probe) {
      POINT mid = {(app->mon.rect.left + app->mon.rect.right) / 2, (app->mon.rect.top + app->mon.rect.bottom) / 2};
      logf(L"Probe run: %d frames, mean %.2f ms, longest %.2f ms, sky shown %d times (%08x), light %d times (%08x)", frames,
           frames ? spent / frames * 1000 : 0.0, worst * 1000, skies, skyResult, lights, lightResult);
      logf(L"Sky visible %d, light visible %d, click at screen center hits the light: %d", IsWindowVisible(app->sky),
           IsWindowVisible(app->light), WindowFromPoint(mid) == app->light);
      app->quit = true;
      break;
    }
    // A covered desktop is idle, but only once the sky is fully computed: when the
    // desktop is uncovered, the finished image is visible at once.
    bool resting = app->covered && app->render.skySettled();
    bool active = app->probe > 0 || (!app->paused && !resting);
    if (!active) lively = false;
    double due = lastDraw + (dirty ? BUSY : CALM);
    DWORD wait = !active ? 250 : DWORD(std::clamp((due - t) * 1000.0, 0.0, 250.0));
    if (lively && !dirty) {
      // Wait for the next refresh. If the answer comes immediately several times in a
      // row (display off, session locked), there is no refresh: then continue at the
      // calm rate. Immediately once only means that the refresh was just due.
      static int instant = 0;
      double before = now();
      bool failed = FAILED(DwmFlush());
      instant = now() - before < 0.0005 ? instant + 1 : 0;
      if (failed || instant > 8) lively = false;
      wait = 0;
    }
    MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) app->quit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (app->quit || app->rebuild) break;
    t = now();
    if (pollPlace()) dirty = true;

    if (t - lastIni > 1.0) {
      lastIni = t;
      app->lookArmed = GetFileAttributesW(app->lookFlag.c_str()) != INVALID_FILE_ATTRIBUTES;
      placeGrip();
      // The sky must stay behind the icons, whatever the shell reorders.
      if (app->skyUp && !app->paused && !skyInPlace()) {
        placeSky(false);
        static int told = 0;
        if (told++ < 10) logf(L"Sky was no longer behind the icons, reordered: %d", skyInPlace());
      }
      // If the display switches into HDR mode or out of it, the light needs a
      // different swap chain; if only the brightness of white changes, the new value
      // is enough.
      bool hdr = false;
      float white = 1;
      readDisplay(hdr, white);
      if (hdr != app->hdr) {
        app->rebuild = true;
      } else if (white != app->white) {
        app->white = white;
        app->render.setOutput(hdr, white);
        dirty = true;
        logf(L"White now %.0f cd/m2", white * 80);
      }
      FILETIME ft = fileTime(app->ini);
      if (CompareFileTime(&ft, &app->iniTime) != 0) {
        loadSettings();
        syncWeather();
        app->render.setParams(app->par);
        app->sceneEvent = true;
      }
      tickWeather();
    }
    if (app->paused) continue;
    if (app->sceneEvent || t - lastScene > SCENE_EVERY) {
      dirty |= app->sceneEvent;
      app->sceneEvent = false;
      lastScene = t;
      readScene();
    }
    if (!settledTold && app->render.skySettled()) {
      settledTold = true;
      logf(L"Sky finished: %d passes in %.1f s", skies, now() - begin);
    }
    if (app->covered && app->probe <= 0 && app->render.skySettled()) {
      dirty = true;   // as soon as the desktop is uncovered again, a frame at once
      continue;
    }
    if (updateHeaven(t)) dirty = true;
    if (trackPointer(t)) dirty = true;
    if (app->iconReady.exchange(false)) {
      std::lock_guard<std::mutex> hold(app->iconLock);
      app->render.setIconMask(app->iconMask.data(), app->mon.width(), app->mon.height());
      app->gripDue = true;
      dirty = true;
      if (app->probe > 0) {
        size_t set = 0;
        for (uint8_t v : app->iconMask) set += v > 127;
        logf(L"Icons drawn in %.0f ms, %zu pixels covered", app->iconMs, set);
      }
    }
    if (app->gripDue) {
      app->gripDue = false;
      placeGrip(true);
    }
    if (t < lastDraw + (dirty || lively ? BUSY : CALM) - 0.0005) continue;

    double dt = lastDraw > 0 ? std::min(t - lastDraw, 0.1) : 0;
    lastDraw = t;
    dirty = false;
    Renderer::Drawn drawn = app->render.render(dt, app->skyRtv.Get(), app->lightRtv.Get());
    lively = drawn.lively;
    if (drawn.sky) {
      skyResult = app->skyChain->Present(0, 0);
      skies++;
      // Show only now, otherwise an empty surface would briefly be behind the icons.
      if (!app->skyUp) {
        app->skyUp = true;
        placeSky(true);
        app->gripDue = true;
        logf(L"Sky shown (%08x), visible %d, behind the icons %d", skyResult, IsWindowVisible(app->sky), skyInPlace());
      }
    }
    if (drawn.light) {
      lightResult = app->lightChain->Present(0, 0);
      lights++;
      if (lightResult == DXGI_ERROR_DEVICE_REMOVED || lightResult == DXGI_ERROR_DEVICE_RESET) app->quit = true;
    }
    if (app->lookArmed && !app->covered && app->probe <= 0 && t - app->freeSince > 2.0) {
      app->lookArmed = false;
      DeleteFileW(app->lookFlag.c_str());
      saveLook(L"look", L"look_source", int(app->render.sourceX() * app->mon.scale), int(app->render.sourceY() * app->mon.scale));
    }
    if (app->spotDue) {
      // Capture a defect in the image: what is on the screen around the mouse pointer,
      // plus the depth map by which the clouds are moved along.
      app->spotDue = false;
      POINT at;
      GetCursorPos(&at);
      int cx = at.x - app->mon.rect.left, cy = at.y - app->mon.rect.top;
      saveLook(L"spot", L"spot_near", cx, cy);
      std::vector<uint8_t> map;
      int mw = 0, mh = 0;
      if (app->render.dumpDepth(map, mw, mh)) writeImage(app->dir + L"\\spot_depth.png", map.data(), mw, mh, 1);
      logf(L"Spot captured at %d %d (device pixels), depth map %d x %d, course %d, cloud shift %.4f", cx, cy, mw, mh, app->course,
           app->render.cloudShift());
    }
    if (app->probe > 0) {
      app->render.sync();
      double took = now() - t;
      spent += took;
      worst = std::max(worst, took);
      frames++;
    }
  }
}

}  // namespace

int runApp(HINSTANCE instance, double probeSeconds, const std::wstring& probeDir) {
  // Only one running Sunbeam. A probe run does not hold this lock, so that it
  // never blocks the actual start.
  HANDLE once = nullptr;
  if (probeSeconds <= 0) {
    once = CreateMutexW(nullptr, TRUE, L"Sunbeam-Desktop");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  }

  App state;
  app = &state;
  app->inst = instance;
  app->probe = probeSeconds;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  if (probeSeconds > 0) {
    app->dir = probeDir;
  } else {
    PWSTR local = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
    app->dir = std::wstring(local ? local : L".") + L"\\Sunbeam";
    CoTaskMemFree(local);
  }
  CreateDirectoryW(app->dir.c_str(), nullptr);
  app->ini = app->dir + L"\\sunbeam.ini";
  app->logFile = app->dir + L"\\sunbeam.log";
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring folder = exe;
  folder.resize(std::min(folder.size(), folder.find_last_of(L'\\')));
  app->lookFlag = folder + L"\\look.txt";
  DeleteFileW(app->logFile.c_str());
  bool fresh = GetFileAttributesW(app->ini.c_str()) == INVALID_FILE_ATTRIBUTES;
  if (fresh) {
    // A new settings file starts as UTF-16, so that place names keep their letters.
    FILE* f = nullptr;
    if (!_wfopen_s(&f, app->ini.c_str(), L"wb") && f) {
      fwrite("\xff\xfe", 1, 2, f);
      fclose(f);
    }
  }
  guessPlace(app->latitude, app->longitude);
  loadSettings();
  syncWeather();
  if (fresh) saveSettings();

  registerClasses();
  app->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  app->host = CreateWindowExW(WS_EX_TOOLWINDOW, HOST_CLASS, L"Sunbeam", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  // Emergency exit: Ctrl+Alt+Shift+Q quits the program from anywhere.
  if (probeSeconds <= 0) RegisterHotKey(app->host, HOTKEY_QUIT, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Q');
  // Ctrl+Alt+Shift+B captures what is on the screen under the mouse pointer (for
  // debugging).
  if (probeSeconds <= 0) RegisterHotKey(app->host, HOTKEY_SPOT, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'B');

  D3D_FEATURE_LEVEL level;
  int result = 0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &app->dev, &level, &app->ctx)) ||
      !app->render.init(app->dev.Get(), app->ctx.Get())) {
    logf(L"Direct3D 11 is not available");
    result = 1;
  }

  while (result == 0 && !app->quit) {
    app->rebuild = false;
    if (!setup()) {
      teardown();
      result = 2;
      break;
    }
    run();
    teardown();
    // After a change of display or shell, wait briefly until everything is in place.
    if (app->rebuild && !app->quit) Sleep(1500);
  }

  logf(L"Exit with %d", result);
  closePanel();
  UnregisterHotKey(app->host, HOTKEY_QUIT);
  UnregisterHotKey(app->host, HOTKEY_SPOT);
  if (app->host) DestroyWindow(app->host);
  CoUninitialize();
  if (once) CloseHandle(once);
  app = nullptr;
  return result;
}

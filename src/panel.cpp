#include "panel.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

#include "astro.h"
#include "daycycle.h"

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* PANEL_CLASS = L"SunbeamSettings";
// All dimensions in logical pixels; the drawing surface converts them itself.
const float W = 440, H = 596;
const float PAD = 24, TABS = 52, ROW = 36;
const float TAB_W = 92, TRACK_X0 = 180, TRACK_X1 = 336, VALUE_X1 = W - PAD;
const UINT_PTR TICK = 1;

const UINT32 BACK = 0x1c1c1e, CARD = 0x242428, LINE = 0x2e2e33, TRACK = 0x3c3c42, TEXT = 0xf2f2f4, DIM = 0x9c9ca4, GOLD = 0xf5b84a,
             MOON = 0xcfd6e6;

struct Row {
  enum Kind { SLIDER, RADIO, TOGGLE, BUTTON, CHART, FIELD } kind = SLIDER;
  const wchar_t* label = L"";
  std::function<float()> get;
  std::function<void(float)> set;
  float lo = 0, hi = 1, step = 0.01f, def = 0;
  std::function<std::wstring(float)> text;
  std::function<bool()> locked;  // cannot be adjusted right now: something else sets the value
  // A text field: what it shows, the note below it, and what happens with entered text.
  std::function<std::wstring()> shown, note;
  std::function<void(const std::wstring&)> enter;
  float height = ROW, gap = 0;   // height and gap above
  float y = 0;                   // top edge, set during layout
};

struct Panel {
  HWND wnd = nullptr;
  PanelHost host;
  float scale = 1;
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dw;
  ComPtr<ID2D1HwndRenderTarget> target;
  ComPtr<IDWriteTextFormat> label, note, tab;
  std::vector<Row> rows[4];
  int page = 0;
  int hover = -1, drag = -1, focus = -1, hoverTab = -1;
  bool keys = false;     // last operated with the keyboard: then it is visible what has the focus
  bool pending = false;  // changed, but not yet reported as complete
  bool tracking = false;
  int edit = -1;         // the text field being typed into
  std::wstring typed;    // its text so far
  bool caret = true;     // the caret blinks
};

Panel* panel = nullptr;
const wchar_t* TAB_NAMES[4] = {L"Sun", L"Sky", L"Light", L"System"};

std::wstring number(float v, int decimals) {
  wchar_t t[32];
  swprintf_s(t, L"%.*f", decimals, v);
  return t;
}

std::wstring percent(float v) { return number(std::round(v * 100), 0) + L" %"; }

Row slider(const wchar_t* label, float* field, float lo, float hi, std::function<std::wstring(float)> text = percent, float step = 0.01f) {
  Row r;
  r.label = label;
  r.get = [field] { return *field; };
  r.set = [field](float v) { *field = v; };
  r.lo = lo, r.hi = hi, r.step = step;
  r.def = *field;
  r.text = std::move(text);
  return r;
}

// Creates the pages. The default of a slider is the value from fresh; a
// double-click restores it.
void build(Panel& p) {
  PanelHost& h = p.host;
  Params fresh;
  auto with = [&](Row r, float def) {
    r.def = def;
    return r;
  };
  auto factor = [](float v) { return number(v, 2); };

  auto& sun = p.rows[0];
  static const wchar_t* courses[3] = {L"Fixed", L"Follows clock", L"Time lapse"};
  for (int i = 0; i < 3; i++) {
    Row r;
    r.kind = Row::RADIO;
    r.label = courses[i];
    r.lo = float(i);
    r.height = 32;
    r.get = [c = h.course] { return float(*c); };
    r.set = [c = h.course](float v) { *c = int(v); };
    sun.push_back(r);
  }
  Row chart;
  chart.kind = Row::CHART;
  chart.height = 168;
  chart.gap = 10;
  sun.push_back(chart);
  // The place for the course of the sun and the weather: a name or coordinates.
  Row where;
  where.kind = Row::FIELD;
  where.label = L"Location";
  where.height = 56;
  where.gap = 8;
  where.shown = h.place;
  where.note = h.placeNote;
  where.enter = h.setPlace;
  sun.push_back(where);
  sun.push_back(with(slider(L"Day length", h.lapseMinutes, 1, 60, [](float v) { return number(v, 0) + L" min"; }, 1), 10));
  auto pixels = [](float v) { return v <= 0 ? std::wstring(L"Auto") : number(v, 0) + L" px"; };
  Row size = slider(L"Sun size", &h.par->sunRadius, 0, 60, pixels, 1);
  size.set = [f = &h.par->sunRadius](float v) { *f = v < 6 ? 0 : v; };
  sun.push_back(with(size, fresh.sunRadius));
  Row moon = slider(L"Moon size", &h.par->moonRadius, 0, 90, pixels, 1);
  moon.set = [f = &h.par->moonRadius](float v) { *f = v < 8 ? 0 : v; };
  sun.push_back(with(moon, fresh.moonRadius));
  sun.push_back(with(slider(L"Rays", &h.par->rays, 0, 1), fresh.rays));

  auto& sky = p.rows[1];
  // Above 100 % the cloud deck closes, then it rains, then the thunderstorm comes.
  auto weather = [](float v) {
    return v > 1.62f ? std::wstring(L"Thunderstorm") : v > 1.3f ? std::wstring(L"Rain") : v > 1.04f ? std::wstring(L"Overcast") : percent(v);
  };
  // Live weather: clouds and wind then follow the weather at the place on the Sun
  // page.
  Row real;
  real.kind = Row::TOGGLE;
  real.label = L"Live weather";
  real.get = [f = h.realWeather] { return f && *f ? 1.0f : 0.0f; };
  real.set = [f = h.realWeather](float v) {
    if (f) *f = v > 0.5f;
  };
  real.text = [](float) { return std::wstring(L"Open-Meteo"); };
  sky.push_back(real);
  auto live = [f = h.realWeather] { return f && *f; };
  Row clouds = with(slider(L"Clouds", &h.par->clouds, 0, 2, weather), fresh.clouds);
  clouds.locked = live;
  sky.push_back(clouds);
  Row wind = with(slider(L"Wind", &h.par->wind, 0, 1), fresh.wind);
  wind.locked = live;
  sky.push_back(wind);
  sky.push_back(with(slider(L"Stars", &h.par->stars, 0, 2), fresh.stars));
  sky.push_back(with(slider(L"Moonlight", &h.par->moonlight, 0, 3), fresh.moonlight));
  sky.push_back(with(slider(L"Life", &h.par->life, 0, 1), fresh.life));
  static const wchar_t* grounds[3] = {L"Mountains", L"Skyline", L"Sea"};
  for (int i = 0; i < 3; i++) {
    Row r;
    r.kind = Row::RADIO;
    r.label = grounds[i];
    r.lo = float(i);
    r.height = 32;
    r.gap = i == 0 ? 12.0f : 0.0f;
    r.get = [f = &h.par->ground] { return *f; };
    r.set = [f = &h.par->ground](float v) { *f = v; };
    sky.push_back(r);
  }

  auto& light = p.rows[2];
  light.push_back(with(slider(L"Haze", &h.par->density, 0, 1.5f), fresh.density));
  light.push_back(with(slider(L"Mist", &h.par->mist, 0, 1), fresh.mist));
  light.push_back(with(slider(L"Ray length", &h.par->length, 0.1f, 0.9f), fresh.length));
  light.push_back(with(slider(L"Depth spread", &h.par->spread, 0, 0.9f), fresh.spread));
  light.push_back(with(slider(L"Forward scatter", &h.par->g, 0.3f, 0.95f, factor), fresh.g));
  light.push_back(with(slider(L"Dust", &h.par->dust, 0, 1), fresh.dust));
  light.push_back(with(slider(L"Brightness", &h.par->exposure, 0.1f, 4), fresh.exposure));
  light.push_back(with(slider(L"Glare", &h.par->bloom, 0, 2), fresh.bloom));
  light.push_back(with(slider(L"Rim light", &h.par->rim, 0, 2), fresh.rim));
  light.push_back(with(slider(L"Over windows", &h.par->overWindows, 0, 1), fresh.overWindows));
  light.push_back(with(slider(L"Over sky", &h.par->overSky, 0, 1), fresh.overSky));
  light.push_back(with(slider(L"Shadows", &h.par->shade, 0, 1), fresh.shade));
  light.push_back(with(slider(L"Pointer", &h.par->pointer, 0, 1.5f), fresh.pointer));
  light.push_back(with(slider(L"HDR glow", &h.par->glow, 1, 6, [](float v) { return v <= 1.001f ? std::wstring(L"Off") : number(v, 1) + L"×"; }, 0.1f),
                       fresh.glow));

  auto& system = p.rows[3];
  system.push_back(with(slider(L"Resolution", &h.par->resolution, 0.25f, 1, percent, 0.05f), fresh.resolution));
  Row steps;
  steps.label = L"Steps";
  steps.get = [par = h.par] { return float(par->steps); };
  steps.set = [par = h.par](float v) { par->steps = int(std::lround(v)); };
  steps.lo = 8, steps.hi = 128, steps.step = 1, steps.def = float(fresh.steps);
  steps.text = [](float v) { return number(v, 0); };
  system.push_back(steps);
  Row start;
  start.kind = Row::TOGGLE;
  start.label = L"Start with Windows";
  start.gap = 8;
  start.get = [fn = h.autostart] { return fn && fn() ? 1.0f : 0.0f; };
  start.set = [fn = h.setAutostart](float v) {
    if (fn) fn(v > 0.5f);
  };
  system.push_back(start);
  Row reset;
  reset.kind = Row::BUTTON;
  reset.label = L"Reset";
  reset.gap = 10;
  reset.height = 44;
  reset.set = [par = h.par](float) { *par = Params(); };
  system.push_back(reset);

  for (auto& list : p.rows) {
    float y = TABS + 14;
    for (Row& r : list) {
      y += r.gap;
      r.y = y;
      y += r.height;
    }
  }
}

D2D1_COLOR_F color(UINT32 rgb, float alpha = 1) { return D2D1::ColorF(rgb, alpha); }

float fraction(const Row& r) { return std::clamp((r.get() - r.lo) / (r.hi - r.lo), 0.0f, 1.0f); }

// Midnight of the day that contains time, in the local time of the computer.
double dayStart(double time) {
  ULONGLONG ticks = ULONGLONG((time + 11644473600.0) * 1e7);
  FILETIME ft = {DWORD(ticks), DWORD(ticks >> 32)};
  SYSTEMTIME utc, local;
  FileTimeToSystemTime(&ft, &utc);
  SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
  local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
  TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
  SystemTimeToFileTime(&utc, &ft);
  return ((ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 1e7 - 11644473600.0;
}

std::wstring clockText(double time) {
  if (time <= 0) return L"";
  ULONGLONG ticks = ULONGLONG((time + 30 + 11644473600.0) * 1e7);
  FILETIME ft = {DWORD(ticks), DWORD(ticks >> 32)};
  SYSTEMTIME utc, local;
  FileTimeToSystemTime(&ft, &utc);
  SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
  wchar_t t[16];
  swprintf_s(t, L"%02d:%02d", local.wHour, local.wMinute);
  return t;
}

struct Pen {
  ID2D1RenderTarget* rt;
  ID2D1SolidColorBrush* brush;
  Panel* p;
  void text(const std::wstring& s, IDWriteTextFormat* f, float x0, float y0, float x1, float y1, UINT32 rgb, float alpha = 1,
            DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    f->SetTextAlignment(align);
    brush->SetColor(color(rgb, alpha));
    rt->DrawText(s.c_str(), UINT32(s.size()), f, D2D1::RectF(x0, y0, x1, y1), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  void fill(float x0, float y0, float x1, float y1, float radius, UINT32 rgb, float alpha = 1) {
    brush->SetColor(color(rgb, alpha));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x1, y1), radius, radius), brush);
  }
  void disc(float x, float y, float r, UINT32 rgb, float alpha = 1) {
    brush->SetColor(color(rgb, alpha));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush);
  }
  void ring(float x, float y, float r, float width, UINT32 rgb, float alpha = 1) {
    brush->SetColor(color(rgb, alpha));
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush, width);
  }
  void line(float x0, float y0, float x1, float y1, float width, UINT32 rgb, float alpha = 1) {
    brush->SetColor(color(rgb, alpha));
    rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush, width);
  }
};

// The path of the sun across the screen on this day, where it and the moon are
// right now, and when it rises and sets.
void drawChart(Pen& pen, const Row& r) {
  const PanelHost& h = pen.p->host;
  float x0 = PAD, x1 = W - PAD, y0 = r.y, y1 = r.y + r.height - 28;
  pen.fill(x0, y0, x1, y1, 8, CARD);
  if (!h.render) return;
  float vw = h.render->viewW(), line = h.render->horizonRow(), span = line * 1.14f;
  auto px = [&](float x) { return x0 + 12 + (x1 - x0 - 24) * x / vw; };
  auto py = [&](float y) { return y0 + 12 + (y1 - y0 - 20) * y / span; };
  pen.line(x0 + 12, py(line), x1 - 12, py(line), 1, 0x55555c);

  double shown = h.shownTime ? h.shownTime() : 0;
  double start = dayStart(shown);
  float before[2] = {0, 0};
  for (int k = 0; k <= 144; k++) {
    Bodies b = bodiesAt(start + k * 600.0, *h.latitude, *h.longitude);
    Heaven at = heavenOfBodies(b, *h.latitude, *h.render);
    bool inside = at.sunX > 0 && at.sunX < vw && at.sunY < span;
    if (k > 0 && inside && before[0] > 0 && before[0] < vw && before[1] < span) {
      bool up = at.sunY < line && before[1] < line;
      pen.line(px(before[0]), py(before[1]), px(at.sunX), py(at.sunY), 1.6f, GOLD, up ? 0.75f : 0.22f);
    }
    before[0] = at.sunX, before[1] = at.sunY;
  }
  // What is shining right now, as a dot: the sun golden, the moon pale.
  float sx = h.render->sourceX(), sy = h.render->sourceY();
  if (sx > 0 && sx < vw && sy > 0 && sy < span) {
    bool moon = h.render->moonShines();
    pen.disc(px(sx), py(sy), moon ? 5.0f : 6.0f, moon ? MOON : GOLD);
    pen.ring(px(sx), py(sy), moon ? 8.5f : 10.0f, 1.2f, moon ? MOON : GOLD, 0.35f);
  }
  DayTimes times = dayTimes(start, *h.latitude, *h.longitude);
  float ty = y1 + 6;
  if (times.rise > 0) pen.text(L"Sunrise " + clockText(times.rise), pen.p->note.Get(), x0, ty, x1, ty + 18, DIM);
  if (times.set > 0) pen.text(L"Sunset " + clockText(times.set), pen.p->note.Get(), x0, ty, x1, ty + 18, DIM, 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
  pen.text(clockText(shown), pen.p->note.Get(), x0, ty, x1, ty + 18, TEXT, 0.9f, DWRITE_TEXT_ALIGNMENT_CENTER);
}

HRESULT draw(ID2D1RenderTarget* rt, Panel& p) {
  rt->BeginDraw();
  rt->SetTransform(D2D1::Matrix3x2F::Identity());
  rt->Clear(color(BACK));
  ComPtr<ID2D1SolidColorBrush> brush;
  rt->CreateSolidColorBrush(color(TEXT), &brush);
  Pen pen = {rt, brush.Get(), &p};

  for (int i = 0; i < 4; i++) {
    float x = PAD - 14 + i * TAB_W;
    bool on = i == p.page;
    pen.text(TAB_NAMES[i], p.tab.Get(), x, 10, x + TAB_W, TABS - 8, on ? TEXT : DIM, on || i == p.hoverTab ? 1.0f : 0.85f, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (on) pen.fill(x + TAB_W / 2 - 12, TABS - 9, x + TAB_W / 2 + 12, TABS - 6, 1.5f, GOLD);
  }
  pen.line(0, TABS, W, TABS, 1, LINE);

  const std::vector<Row>& rows = p.rows[p.page];
  for (int i = 0; i < int(rows.size()); i++) {
    const Row& r = rows[i];
    float cy = r.y + r.height / 2;
    bool hot = i == p.hover || i == p.drag, focused = p.keys && i == p.focus;
    switch (r.kind) {
      case Row::SLIDER: {
        // When locked, the slider only displays: pale, with a small knob.
        bool off = r.locked && r.locked();
        pen.text(r.label, p.label.Get(), PAD, r.y, TRACK_X0 - 8, r.y + r.height, TEXT, off ? 0.5f : 0.94f);
        float t = fraction(r), x = TRACK_X0 + (TRACK_X1 - TRACK_X0) * t;
        pen.fill(TRACK_X0, cy - 2, TRACK_X1, cy + 2, 2, TRACK);
        pen.fill(TRACK_X0, cy - 2, x, cy + 2, 2, off ? DIM : GOLD, off ? 0.6f : 1.0f);
        if (focused) pen.ring(x, cy, 12, 1.5f, GOLD, 0.8f);
        if (off) {
          pen.disc(x, cy, 5, DIM);
        } else {
          pen.disc(x, cy + 0.5f, hot ? 9.5f : 8.5f, 0x000000, 0.28f);
          pen.disc(x, cy, hot ? 8.5f : 7.5f, 0xffffff);
        }
        pen.text(r.text(r.get()), p.label.Get(), TRACK_X1 + 8, r.y, VALUE_X1, r.y + r.height, i == p.drag ? TEXT : DIM, off ? 0.7f : 1.0f,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        break;
      }
      case Row::RADIO: {
        bool on = std::lround(r.get()) == std::lround(r.lo);
        float x = PAD + 9;
        if (focused) pen.ring(x, cy, 12.5f, 1.5f, GOLD, 0.8f);
        pen.ring(x, cy, 8, 1.6f, on ? GOLD : (hot ? TEXT : DIM));
        if (on) pen.disc(x, cy, 4.2f, GOLD);
        pen.text(r.label, p.label.Get(), PAD + 30, r.y, W - PAD, r.y + r.height, TEXT, on || hot ? 1.0f : 0.88f);
        break;
      }
      case Row::TOGGLE: {
        bool on = r.get() > 0.5f;
        pen.text(r.label, p.label.Get(), PAD, r.y, W - PAD - 56, r.y + r.height, TEXT, 0.94f);
        float sx1 = W - PAD, sx0 = sx1 - 40;
        // Where the switch takes its data from is written in small type beside it.
        if (r.text) pen.text(r.text(0), p.note.Get(), PAD, r.y, sx0 - 14, r.y + r.height, DIM, 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (focused) {
          pen.brush->SetColor(color(GOLD, 0.8f));
          rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sx0 - 3, cy - 13, sx1 + 3, cy + 13), 13, 13), pen.brush, 1.5f);
        }
        if (on) {
          pen.fill(sx0, cy - 10, sx1, cy + 10, 10, GOLD);
          pen.disc(sx1 - 10, cy, hot ? 7 : 6, BACK);
        } else {
          pen.brush->SetColor(color(hot ? TEXT : DIM));
          rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sx0 + 0.75f, cy - 9.25f, sx1 - 0.75f, cy + 9.25f), 9.25f, 9.25f), pen.brush, 1.5f);
          pen.disc(sx0 + 10, cy, hot ? 6 : 5, hot ? TEXT : DIM);
        }
        break;
      }
      case Row::BUTTON: {
        float bw = 132, by0 = r.y + r.height - 32;
        if (focused) {
          pen.brush->SetColor(color(GOLD, 0.8f));
          rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(PAD - 3, by0 - 3, PAD + bw + 3, by0 + 35), 8, 8), pen.brush, 1.5f);
        }
        pen.fill(PAD, by0, PAD + bw, by0 + 32, 6, hot ? 0x3a3a40 : 0x2c2c31);
        pen.text(r.label, p.label.Get(), PAD, by0, PAD + bw, by0 + 32, TEXT, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        break;
      }
      case Row::CHART:
        drawChart(pen, r);
        break;
      case Row::FIELD: {
        bool editing = i == p.edit;
        float x0 = TRACK_X0 - 6, x1 = VALUE_X1, y0 = r.y + 3, y1 = r.y + 31;
        pen.text(r.label, p.label.Get(), PAD, y0, x0 - 8, y1, TEXT, 0.94f);
        pen.fill(x0, y0, x1, y1, 6, CARD);
        pen.brush->SetColor(color(editing || focused ? GOLD : (hot ? DIM : TRACK), editing || focused ? 0.85f : 1.0f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0 + 0.5f, y0 + 0.5f, x1 - 0.5f, y1 - 0.5f), 6, 6), pen.brush, 1);
        std::wstring text = editing ? p.typed : (r.shown ? r.shown() : std::wstring());
        // Text wider than the field shows its end, where typing continues.
        float wide = 0;
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(p.dw->CreateTextLayout(text.c_str(), UINT32(text.size()), p.label.Get(), 4000, 28, &layout))) {
          DWRITE_TEXT_METRICS tm;
          layout->GetMetrics(&tm);
          wide = tm.widthIncludingTrailingWhitespace;
        }
        float room = x1 - x0 - 20;
        pen.text(text, p.label.Get(), x0 + 10, y0, x1 - 10, y1, TEXT, editing ? 1.0f : 0.94f,
                 wide > room ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        if (editing && p.caret) {
          float cx = x0 + 10 + std::min(wide, room) + 1;
          pen.line(cx, y0 + 6, cx, y1 - 6, 1.2f, TEXT);
        }
        if (r.note) pen.text(r.note(), p.note.Get(), x0 + 10, y1 + 1, x1, r.y + r.height - 2, DIM);
        break;
      }
    }
  }
  return rt->EndDraw();
}

bool makeFormats(Panel& p) {
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(p.dw.GetAddressOf())))) return false;
  auto format = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
    HRESULT hr = p.dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &out);
    if (FAILED(hr)) return false;
    out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
  };
  return format(14, DWRITE_FONT_WEIGHT_NORMAL, p.label) && format(12, DWRITE_FONT_WEIGHT_NORMAL, p.note) && format(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, p.tab);
}

// ---- Input ----

bool usable(const Row& r) { return r.kind != Row::CHART && !(r.locked && r.locked()); }

int rowAt(const Panel& p, float x, float y) {
  const std::vector<Row>& rows = p.rows[p.page];
  for (int i = 0; i < int(rows.size()); i++) {
    const Row& r = rows[i];
    if (!usable(r) || y < r.y || y >= r.y + r.height) continue;
    if (r.kind == Row::BUTTON && (x < PAD || x > PAD + 132 || y < r.y + r.height - 32)) continue;
    return i;
  }
  return -1;
}

void changed(Panel& p, bool done) {
  if (p.host.changed) p.host.changed(done);
  p.pending = !done;
  InvalidateRect(p.wnd, nullptr, FALSE);
}

void setSlider(Panel& p, Row& r, float v) {
  v = r.lo + std::round((std::clamp(v, r.lo, r.hi) - r.lo) / r.step) * r.step;
  v = std::clamp(v, r.lo, r.hi);
  if (v == r.get()) return;
  r.set(v);
  changed(p, false);
}

void activate(Panel& p, Row& r) {
  switch (r.kind) {
    case Row::RADIO:
      r.set(r.lo);
      break;
    case Row::TOGGLE:
      r.set(r.get() > 0.5f ? 0.0f : 1.0f);
      break;
    case Row::BUTTON:
      r.set(0);
      break;
    default:
      return;
  }
  changed(p, true);
}

// Typing into a text field: starts empty, Enter passes the text on, Escape
// or a click elsewhere drops it.
void beginEdit(Panel& p, int row) {
  p.edit = row;
  p.typed.clear();
  p.caret = true;
  InvalidateRect(p.wnd, nullptr, FALSE);
}

void endEdit(Panel& p, bool keep) {
  if (p.edit < 0) return;
  Row& r = p.rows[p.page][p.edit];
  std::wstring text = p.typed;
  p.edit = -1;
  while (!text.empty() && iswspace(text.back())) text.pop_back();
  while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
  if (keep && !text.empty() && r.enter) r.enter(text);
  InvalidateRect(p.wnd, nullptr, FALSE);
}

// Text from the clipboard, one line.
std::wstring pasted(HWND wnd) {
  std::wstring out;
  if (!OpenClipboard(wnd)) return out;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data))) {
      for (const wchar_t* c = text; *c && *c != L'\r' && *c != L'\n' && out.size() < 80; c++) out += *c;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return out;
}

void resizeTarget(Panel& p) {
  if (!p.target) return;
  RECT rc;
  GetClientRect(p.wnd, &rc);
  p.target->Resize(D2D1::SizeU(rc.right, rc.bottom));
  p.target->SetDpi(96 * p.scale, 96 * p.scale);
}

void paint(Panel& p) {
  if (!p.target) {
    RECT rc;
    GetClientRect(p.wnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = 96 * p.scale;
    if (FAILED(p.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(p.wnd, D2D1::SizeU(rc.right, rc.bottom)), &p.target))) return;
  }
  // If the graphics card has discarded the surface, it is created again at the
  // next draw.
  if (draw(p.target.Get(), p) == D2DERR_RECREATE_TARGET) {
    p.target.Reset();
    InvalidateRect(p.wnd, nullptr, FALSE);
  }
}

LRESULT CALLBACK panelProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  Panel* p = panel;
  if (!p || p->wnd != wnd) return DefWindowProcW(wnd, msg, wp, lp);
  auto at = [&](LPARAM l, float& x, float& y) {
    x = GET_X_LPARAM(l) / p->scale;
    y = GET_Y_LPARAM(l) / p->scale;
  };
  std::vector<Row>& rows = p->rows[p->page];
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(wnd, &ps);
      paint(*p);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      resizeTarget(*p);
      return 0;
    case WM_DPICHANGED: {
      p->scale = HIWORD(wp) / 96.0f;
      const RECT* to = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(wnd, nullptr, to->left, to->top, to->right - to->left, to->bottom - to->top, SWP_NOZORDER | SWP_NOACTIVATE);
      resizeTarget(*p);
      return 0;
    }
    case WM_TIMER:
      // The dot in the day arc moves along; and what was adjusted with wheel or keys
      // counts as complete after a moment of rest.
      if (p->pending && p->drag < 0) changed(*p, true);
      if (p->edit >= 0) p->caret = !p->caret;
      // With live weather the sliders Clouds and Wind also move by themselves.
      if (p->page == 0 || (p->page == 1 && p->host.realWeather && *p->host.realWeather)) InvalidateRect(wnd, nullptr, FALSE);
      return 0;
    case WM_MOUSEMOVE: {
      float x, y;
      at(lp, x, y);
      if (!p->tracking) {
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, wnd, 0};
        TrackMouseEvent(&t);
        p->tracking = true;
      }
      if (p->drag >= 0) {
        Row& r = rows[p->drag];
        setSlider(*p, r, r.lo + (r.hi - r.lo) * (x - TRACK_X0) / (TRACK_X1 - TRACK_X0));
        return 0;
      }
      int row = rowAt(*p, x, y), tab = y < TABS && x >= PAD - 14 && x < PAD - 14 + 4 * TAB_W ? int((x - PAD + 14) / TAB_W) : -1;
      if (row != p->hover || tab != p->hoverTab) {
        p->hover = row;
        p->hoverTab = tab;
        InvalidateRect(wnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      p->tracking = false;
      p->hover = p->hoverTab = -1;
      InvalidateRect(wnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      float x, y;
      at(lp, x, y);
      p->keys = false;
      endEdit(*p, false);
      if (y < TABS) {
        if (p->hoverTab >= 0 && p->hoverTab != p->page) {
          p->page = p->hoverTab;
          p->hover = p->focus = -1;
          InvalidateRect(wnd, nullptr, FALSE);
        }
        return 0;
      }
      int i = rowAt(*p, x, y);
      if (i < 0) return 0;
      Row& r = rows[i];
      p->focus = i;
      if (r.kind == Row::FIELD) {
        beginEdit(*p, i);
      } else if (r.kind != Row::SLIDER) {
        activate(*p, r);
      } else if (msg == WM_LBUTTONDBLCLK) {
        // Double-click: back to the default.
        r.set(r.def);
        changed(*p, true);
      } else if (x >= TRACK_X0 - 12 && x <= TRACK_X1 + 12) {
        p->drag = i;
        SetCapture(wnd);
        setSlider(*p, r, r.lo + (r.hi - r.lo) * (x - TRACK_X0) / (TRACK_X1 - TRACK_X0));
        InvalidateRect(wnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP:
      if (p->drag >= 0) ReleaseCapture();
      return 0;
    case WM_CAPTURECHANGED:
      if (p->drag >= 0) {
        p->drag = -1;
        changed(*p, true);
      }
      return 0;
    case WM_MOUSEWHEEL: {
      if (p->hover < 0 || rows[p->hover].kind != Row::SLIDER) return 0;
      Row& r = rows[p->hover];
      float notch = std::max(r.step, (r.hi - r.lo) / 50);
      setSlider(*p, r, r.get() + notch * GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
      return 0;
    }
    case WM_CHAR:
      if (p->edit < 0) return 0;
      if (wp == 8) {
        if (!p->typed.empty()) p->typed.pop_back();
      } else if (wp == 22) {
        p->typed += pasted(wnd);
      } else if (wp >= 32 && p->typed.size() < 80) {
        p->typed += wchar_t(wp);
      }
      p->caret = true;
      InvalidateRect(wnd, nullptr, FALSE);
      return 0;
    case WM_KILLFOCUS:
      endEdit(*p, false);
      return 0;
    case WM_KEYDOWN: {
      // While a text field is being typed into, the keys belong to it.
      if (p->edit >= 0) {
        if (wp == VK_RETURN) endEdit(*p, true);
        else if (wp == VK_ESCAPE || wp == VK_TAB) endEdit(*p, false);
        return 0;
      }
      p->keys = true;
      bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
      int n = int(rows.size());
      if (wp == VK_ESCAPE) {
        DestroyWindow(wnd);
        return 0;
      }
      if (wp == VK_TAB && ctrl) {
        p->page = (p->page + (shift ? 3 : 1)) % 4;
        p->focus = p->hover = -1;
      } else if (wp == VK_TAB || wp == VK_DOWN || wp == VK_UP) {
        int step = (wp == VK_TAB && shift) || wp == VK_UP ? -1 : 1;
        for (int k = 0, i = p->focus; k < n; k++) {
          i = (i + step + n + (i < 0 && step < 0 ? 1 : 0)) % n;
          if (usable(rows[i])) {
            p->focus = i;
            break;
          }
        }
      } else if (p->focus >= 0 && p->focus < n) {
        Row& r = rows[p->focus];
        if (r.kind == Row::SLIDER && (wp == VK_LEFT || wp == VK_RIGHT)) {
          float notch = shift ? (r.hi - r.lo) / 10 : std::max(r.step, (r.hi - r.lo) / 100);
          setSlider(*p, r, r.get() + (wp == VK_RIGHT ? notch : -notch));
        } else if (wp == VK_SPACE || wp == VK_RETURN) {
          if (r.kind == Row::FIELD) beginEdit(*p, p->focus);
          else activate(*p, r);
        }
      }
      InvalidateRect(wnd, nullptr, FALSE);
      return 0;
    }
    case WM_DESTROY:
      if (p->pending) changed(*p, true);
      KillTimer(wnd, TICK);
      delete p;
      panel = nullptr;
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

}  // namespace

void openPanel(HINSTANCE inst, const PanelHost& host) {
  if (panel) {
    ShowWindow(panel->wnd, SW_SHOWNORMAL);
    SetForegroundWindow(panel->wnd);
    return;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW c = {sizeof(c)};
    c.style = CS_DBLCLKS;
    c.hInstance = inst;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.lpfnWndProc = panelProc;
    c.lpszClassName = PANEL_CLASS;
    c.hIcon = host.icon;
    c.hIconSm = host.icon;
    RegisterClassExW(&c);
    registered = true;
  }
  Panel* p = new Panel;
  p->host = host;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, p->d2d.GetAddressOf())) || !makeFormats(*p)) {
    delete p;
    return;
  }
  build(*p);

  // At the bottom right above the taskbar, where the icon is as well.
  POINT cursor;
  GetCursorPos(&cursor);
  HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  UINT dpi = 96, dpiY = 96;
  HMODULE shcore = LoadLibraryW(L"shcore.dll");
  if (shcore) {
    using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (auto fn = reinterpret_cast<GetDpi>(GetProcAddress(shcore, "GetDpiForMonitor"))) fn(mon, 0, &dpi, &dpiY);
  }
  p->scale = dpi / 96.0f;
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT rc = {0, 0, LONG(std::lround(W * p->scale)), LONG(std::lround(H * p->scale))};
  AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
  int w = rc.right - rc.left, h = rc.bottom - rc.top, margin = int(16 * p->scale);
  panel = p;
  p->wnd = CreateWindowExW(0, PANEL_CLASS, L"Sunbeam", style, mi.rcWork.right - w - margin, mi.rcWork.bottom - h - margin, w, h, nullptr, nullptr, inst, nullptr);
  if (!p->wnd) {
    panel = nullptr;
    delete p;
    return;
  }
  // Dark title bar in the color of the surface.
  BOOL dark = TRUE;
  DwmSetWindowAttribute(p->wnd, 20, &dark, sizeof(dark));
  COLORREF caption = RGB(0x1c, 0x1c, 0x1e);
  DwmSetWindowAttribute(p->wnd, 35, &caption, sizeof(caption));
  SetTimer(p->wnd, TICK, 400, nullptr);
  ShowWindow(p->wnd, SW_SHOWNORMAL);
  SetForegroundWindow(p->wnd);
}

void closePanel() {
  if (panel) DestroyWindow(panel->wnd);
}

bool renderPanel(const std::wstring& dir, const PanelHost& host, float scale) {
  Panel p;
  p.host = host;
  p.scale = scale;
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, p.d2d.GetAddressOf())) || !makeFormats(p) ||
      FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
    return false;
  }
  build(p);
  UINT w = UINT(std::lround(W * scale)), h = UINT(std::lround(H * scale));
  bool ok = true;
  for (int page = 0; page < 4; page++) {
    p.page = page;
    // For viewing the states: on every page one slider under the mouse.
    p.hover = page == 2 ? 3 : -1;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    if (FAILED(p.d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return false;
    draw(rt.Get(), p);
    rt.Reset();

    wchar_t name[32];
    swprintf_s(name, L"\\page%d.png", page + 1);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename((dir + name).c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    ok &= SUCCEEDED(hr);
  }
  return ok;
}

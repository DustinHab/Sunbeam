// Check run without a window: renders the image with a simulated layout to files
// and measures the timings.
// Usage: Sunbeam.exe --check <folder> [width height scale]

#include "check.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "astro.h"
#include "daycycle.h"
#include "panel.h"
#include "place.h"
#include "weather.h"
#include "render.h"
#include "scene.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // BGRA
  uint8_t* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
};

bool savePng(const std::wstring& path, const Image& im) {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  HRESULT hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
  if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
  if (SUCCEEDED(hr)) hr = frame->SetSize(im.w, im.h);
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr)) hr = frame->WritePixels(im.h, im.w * 4, UINT(im.px.size()), const_cast<BYTE*>(im.px.data()));
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  return SUCCEEDED(hr);
}

struct Offscreen {
  ComPtr<ID3D11Texture2D> tex, copy;
  ComPtr<ID3D11RenderTargetView> rtv;
};

Offscreen makeOffscreen(ID3D11Device* dev, int w, int h, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
  Offscreen o;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = d.ArraySize = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_RENDER_TARGET;
  dev->CreateTexture2D(&d, nullptr, &o.tex);
  dev->CreateRenderTargetView(o.tex.Get(), nullptr, &o.rtv);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  dev->CreateTexture2D(&d, nullptr, &o.copy);
  return o;
}

Image readBack(ID3D11DeviceContext* ctx, Offscreen& o, int w, int h) {
  Image im;
  im.w = w;
  im.h = h;
  im.px.resize(size_t(w) * h * 4);
  ctx->CopyResource(o.copy.Get(), o.tex.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(o.copy.Get(), 0, D3D11_MAP_READ, 0, &m))) {
    for (int y = 0; y < h; y++) memcpy(im.at(0, y), static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(w) * 4);
    ctx->Unmap(o.copy.Get(), 0);
  }
  return im;
}

// Downscaled by an integer factor, mean over the pixels.
Image shrink(const Image& src, int f) {
  Image out;
  out.w = src.w / f;
  out.h = src.h / f;
  out.px.resize(size_t(out.w) * out.h * 4);
  for (int y = 0; y < out.h; y++) {
    for (int x = 0; x < out.w; x++) {
      int sum[4] = {};
      for (int j = 0; j < f; j++) {
        for (int i = 0; i < f; i++) {
          const uint8_t* p = &src.px[(size_t(y * f + j) * src.w + x * f + i) * 4];
          for (int c = 0; c < 4; c++) sum[c] += p[c];
        }
      }
      for (int c = 0; c < 4; c++) out.at(x, y)[c] = uint8_t(sum[c] / (f * f));
    }
  }
  return out;
}

Image crop(const Image& src, int x0, int y0, int w, int h) {
  Image out;
  x0 = std::clamp(x0, 0, std::max(0, src.w - w));
  y0 = std::clamp(y0, 0, std::max(0, src.h - h));
  out.w = std::min(w, src.w);
  out.h = std::min(h, src.h);
  out.px.resize(size_t(out.w) * out.h * 4);
  for (int y = 0; y < out.h; y++) memcpy(out.at(0, y), &src.px[(size_t(y0 + y) * src.w + x0) * 4], size_t(out.w) * 4);
  return out;
}

// Composes the image as on the desktop: sky, icons, dark window areas, plus
// light. With block > 1 the icons are drawn as coarse blocks so that their
// labels cannot be read.
Image groundOf(const Image& sky, const std::vector<Occluder>& scene, const std::vector<uint8_t>& mask, float scale, int block) {
  int pw = sky.w, ph = sky.h;
  Image out = sky;
  auto dot = [&](int x, int y) {
    uint8_t* p = out.at(x, y);
    p[0] = 200, p[1] = 205, p[2] = 210;
  };
  if (block <= 1) {
    for (int y = 0; y < ph; y++) {
      for (int x = 0; x < pw; x++) {
        if (mask[size_t(y) * pw + x]) dot(x, y);
      }
    }
  } else {
    for (int by = 0; by < ph; by += block) {
      for (int bx = 0; bx < pw; bx += block) {
        int x1 = std::min(pw, bx + block), y1 = std::min(ph, by + block);
        bool any = false;
        for (int y = by; y < y1 && !any; y++) {
          for (int x = bx; x < x1 && !any; x++) any = mask[size_t(y) * pw + x] > 127;
        }
        if (!any) continue;
        for (int y = by; y < y1; y++) {
          for (int x = bx; x < x1; x++) dot(x, y);
        }
      }
    }
  }
  auto panelFill = [&](const Occluder& o, int cr, int cg, int cb, float a) {
    int x0 = std::max(0, int(o.x0 * scale)), y0 = std::max(0, int(o.y0 * scale));
    int x1 = std::min(pw, int(o.x1 * scale)), y1 = std::min(ph, int(o.y1 * scale));
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        uint8_t* p = out.at(x, y);
        p[0] = uint8_t(p[0] * (1 - a) + cb * a);
        p[1] = uint8_t(p[1] * (1 - a) + cg * a);
        p[2] = uint8_t(p[2] * (1 - a) + cr * a);
      }
    }
  };
  for (const Occluder& o : scene) {
    if (o.kind == 2) panelFill(o, 24, 24, 27, 0.72f);
    else if (o.opacity < 1) panelFill(o, 14, 14, 16, 0.66f);
    else panelFill(o, 30, 30, 33, block > 1 ? 1.0f : 0.82f);
  }
  for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
  return out;
}

Image compose(const Image& sky, Image& light, const std::vector<Occluder>& scene, const std::vector<uint8_t>& mask, float scale,
              int block) {
  Image out = groundOf(sky, scene, mask, scale, block);
  // The light is premultiplied: its coverage darkens, its color is added.
  for (size_t i = 0; i < out.px.size(); i += 4) {
    int keep = 255 - light.px[i + 3];
    for (int c = 0; c < 3; c++) out.px[i + c] = uint8_t(std::min(255, out.px[i + c] * keep / 255 + light.px[i + c]));
  }
  return out;
}

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float v) {
  v = std::clamp(v, 0.0f, 1.0f);
  return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
}

// Composes the way Windows does in HDR mode (measured): linear, color plus
// background times one minus coverage. light: per pixel red, green, blue,
// coverage, linear; white: brightness of white.
Image composeLinear(const Image& sky, const std::vector<float>& light, float white, const std::vector<Occluder>& scene,
                    const std::vector<uint8_t>& mask, float scale, int block) {
  Image out = groundOf(sky, scene, mask, scale, block);
  float lut[256];
  for (int i = 0; i < 256; i++) lut[i] = srgbToLinear(i / 255.0f);
  for (size_t i = 0, n = out.px.size() / 4; i < n; i++) {
    const float* l = &light[i * 4];
    uint8_t* p = &out.px[i * 4];
    for (int c = 0; c < 3; c++) {
      float v = l[2 - c] / white + lut[p[c]] * (1 - l[3]);
      p[c] = uint8_t(std::lround(linearToSrgb(v) * 255));
    }
  }
  return out;
}

// Reads back a target in floating-point format: red, green, blue, coverage per
// pixel.
std::vector<float> readBackWide(ID3D11DeviceContext* ctx, Offscreen& o, int w, int h) {
  std::vector<float> out(size_t(w) * h * 4);
  ctx->CopyResource(o.copy.Get(), o.tex.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(o.copy.Get(), 0, D3D11_MAP_READ, 0, &m))) {
    for (int y = 0; y < h; y++) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
      for (int x = 0; x < w * 4; x++) {
        uint16_t v = row[x];
        int e = (v >> 10) & 31, f = v & 1023;
        float a = e == 0 ? std::ldexp(float(f), -24) : e == 31 ? 65504.0f : std::ldexp(float(f + 1024), e - 25);
        out[size_t(y) * w * 4 + x] = (v & 0x8000) ? -a : a;
      }
    }
    ctx->Unmap(o.copy.Get(), 0);
  }
  return out;
}

// For trying things out: values.txt in the folder, per line a name from the
// settings file and a number. Shift sets the state of the clouds.
bool readValues(const std::wstring& dir, FILE* log, Params& p, double& shift) {
  FILE* in = nullptr;
  if (_wfopen_s(&in, (dir + L"\\values.txt").c_str(), L"r") || !in) return false;
  const struct {
    const char* key;
    float Params::* field;
  } keys[] = {{"Haze", &Params::density},       {"Mist", &Params::mist},        {"RayLength", &Params::length},
              {"DepthSpread", &Params::spread},   {"ForwardScatter", &Params::g},   {"Dust", &Params::dust},
              {"Clouds", &Params::clouds},       {"Brightness", &Params::exposure},   {"Glare", &Params::bloom},
              {"RimLight", &Params::rim},     {"OverWindows", &Params::overWindows},
              {"OverSky", &Params::overSky}, {"Shadows", &Params::shade},        {"Wind", &Params::wind},
              {"Stars", &Params::stars},        {"Moonlight", &Params::moonlight},   {"HdrGlow", &Params::glow},
              {"SunSize", &Params::sunRadius}, {"MoonSize", &Params::moonRadius}, {"Rays", &Params::rays}, {"Ground", &Params::ground},
              {"Life", &Params::life}};
  char key[64];
  float v;
  while (fscanf_s(in, "%63s %f", key, unsigned(sizeof(key)), &v) == 2) {
    if (!strcmp(key, "Shift")) shift = v;
    for (const auto& k : keys) {
      if (!strcmp(key, k.key)) {
        p.*k.field = v;
        fprintf(log, "  %s = %g\n", key, v);
      }
    }
  }
  fclose(in);
  return true;
}

}  // namespace

int runLifeCheck(const std::wstring& dir) {
  CreateDirectoryW(dir.c_str(), nullptr);
  FILE* log = nullptr;
  if (_wfopen_s(&log, (dir + L"\\life.txt").c_str(), L"w") || !log) return 2;
  static const char* names[MODEL_COUNT] = {"container", "freighter", "dinghy", "airliner", "gull"};
  static const char* grounds[3] = {"Mountains", "Skyline", "Sea"};
  // The wind at the ground at half slider, as the renderer passes it on.
  const double wind[3] = {6.0, 2.4, 6.0}, eye[3] = {0, 9, 26};
  const double hours = 48, dt = 0.05;
  for (int ground = 0; ground < 3; ground++) {
    Surroundings s;
    s.ground = ground;
    s.water = ground > 0;
    s.eye = eye[ground];
    s.wind = wind[ground];
    s.windX = -0.747, s.windZ = -0.665;
    s.halfWidth = 0.5 * 2560 / 2816;
    s.top = 1180.8 / 2816;
    const float length[MODEL_COUNT] = {300, 127, 7.1f, 30.5f, 1.34f};
    for (int i = 0; i < MODEL_COUNT; i++) s.has[i] = true, s.length[i] = length[i];
    struct Tally {
      int times = 0;
      double seconds = 0, nearest = 1e12, farthest = 0, lowest = 1e9, highest = -1e9, widest = 0;
      bool there = false;
    } tally[MODEL_COUNT];
    Life life;
    double shown = 0;
    for (double t = 0; t < hours * 3600; t += dt) {
      const std::vector<Prop>& now = life.update(dt, s);
      bool there[MODEL_COUNT] = {};
      for (const Prop& p : now) {
        Tally& a = tally[p.model];
        there[p.model] = true;
        // Only what is within the image counts as seen.
        double side = std::abs(p.pos[0] / p.pos[2]) / s.halfWidth, rise = (p.pos[1] - s.eye) / p.pos[2];
        if (side > 1) continue;
        a.nearest = std::min(a.nearest, p.pos[2]), a.farthest = std::max(a.farthest, p.pos[2]);
        a.lowest = std::min(a.lowest, rise), a.highest = std::max(a.highest, rise);
        a.widest = std::max(a.widest, length[p.model] / p.pos[2] * 2816);
      }
      for (int i = 0; i < MODEL_COUNT; i++) {
        if (there[i] && !tally[i].there) tally[i].times++;
        if (there[i]) tally[i].seconds += dt;
        tally[i].there = there[i];
      }
      if (!now.empty()) shown += dt;
    }
    fprintf(log, "%s, %g hours of daylight, slider at half: something in the image %.1f %% of the time\n", grounds[ground], hours, 100 * shown / (hours * 3600));
    for (int i = 0; i < MODEL_COUNT; i++) {
      const Tally& a = tally[i];
      if (!a.times) {
        fprintf(log, "  %-10s never\n", names[i]);
        continue;
      }
      fprintf(log, "  %-10s %3d times, %6.1f s each, %4.1f %% of the time, %5.0f to %5.0f m away, largest %4.0f px, height in the image %+.3f to %+.3f\n",
              names[i], a.times, a.seconds / a.times, 100 * a.seconds / (hours * 3600), a.nearest, a.farthest, a.widest, a.lowest, a.highest);
    }
  }
  fclose(log);
  return 0;
}

bool writeImage(const std::wstring& path, const uint8_t* bgra, int w, int h, int factor, int cx, int cy, int cw, int ch) {
  Image im;
  im.w = w;
  im.h = h;
  im.px.assign(bgra, bgra + size_t(w) * h * 4);
  for (size_t i = 3; i < im.px.size(); i += 4) im.px[i] = 255;
  if (cw > 0 && ch > 0) im = crop(im, cx, cy, cw, ch);
  if (factor > 1) im = shrink(im, factor);
  return savePng(path, im);
}

int runCheck(const std::wstring& dir, int viewW, int viewH, float scale) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  CreateDirectoryW(dir.c_str(), nullptr);
  FILE* log = nullptr;
  _wfopen_s(&log, (dir + L"\\check.txt").c_str(), L"w");
  if (!log) return 2;

  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, &level, &ctx))) {
    fprintf(log, "no Direct3D device\n");
    fclose(log);
    return 3;
  }
  Renderer r;
  if (!r.init(dev.Get(), ctx.Get())) {
    fprintf(log, "cannot create shaders\n");
    fclose(log);
    return 4;
  }
  // Nothing appears by chance in images that are compared.
  r.stage({});
  int pw = int(std::lround(viewW * scale)), ph = int(std::lround(viewH * scale));
  r.resize(pw, ph, scale);

  // The same layout as in the preview: four windows around the sun, on the right
  // the window with the settings, at the bottom the taskbar.
  const float bar = 48, panel = 290;
  float W = float(viewW), H = float(viewH) - bar;
  float fw = W >= 900 ? W - panel - 16 : W;
  float sx = std::round(0.565f * fw), sy = std::round(0.625f * H);
  auto lim = [](float share, float total, float max) { return std::round(std::min(share * total, max)); };
  struct Box {
    float x, y, w, h, opacity;
  };
  std::vector<Box> boxes;
  {
    float w = lim(0.36f, fw, 560), h = lim(0.495f, H, 460);
    boxes.push_back({sx - lim(0.075f, fw, 90) - w, sy - lim(0.06f, H, 60) - h, w, h, 1});
    w = lim(0.42f, fw, 640), h = lim(0.455f, H, 430);
    boxes.push_back({sx + lim(0.005f, fw, 12), sy - lim(0.07f, H, 64) - h, w, h, 1});
    w = lim(0.38f, fw, 600), h = lim(0.285f, H, 300);
    boxes.push_back({sx - lim(0.045f, fw, 60) - w, sy + lim(0.06f, H, 60), w, h, 1});
    w = lim(0.36f, fw, 560), h = lim(0.235f, H, 260);
    boxes.push_back({sx + lim(0.06f, fw, 80), sy + lim(0.075f, H, 72), w, h, 0.8f});
    boxes.push_back({W - panel - 16, 16, panel, std::min(700.0f, H - 32), 1});
  }
  std::vector<Occluder> scene;
  for (const Box& b : boxes) scene.push_back({b.x, b.y, b.x + b.w, b.y + b.h, 8, b.opacity, 1});
  scene.push_back({0, H, W, H + bar, 0, 0.88f, 2});
  r.setScene(scene, sx, sy);

  // Six icons at the left edge: image and label as coverage.
  std::vector<uint8_t> mask(size_t(pw) * ph, 0);
  auto fill = [&](float x, float y, float w, float h) {
    int x0 = std::max(0, int(x * scale)), y0 = std::max(0, int(y * scale));
    int x1 = std::min(pw, int((x + w) * scale)), y1 = std::min(ph, int((y + h) * scale));
    for (int yy = y0; yy < y1; yy++) memset(&mask[size_t(yy) * pw + x0], 255, size_t(std::max(0, x1 - x0)));
  };
  for (int i = 0; i < 6; i++) {
    fill(8 + 14, 8 + i * 96.0f + 7, 48, 48);
    fill(8 + 38 - 28, 8 + i * 96.0f + 62, 56, 12);
  }
  r.setIconMask(mask.data(), pw, ph);

  Offscreen skyOut = makeOffscreen(dev.Get(), pw, ph), lightOut = makeOffscreen(dev.Get(), pw, ph);
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

  // Warm up, let half a minute pass, then measure.
  auto t0 = Clock::now();
  for (int i = 0; i < 120; i++) r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
  for (int i = 0; i < 600; i++) r.render(1.0 / 20, skyOut.rtv.Get(), lightOut.rtv.Get());
  r.sync();
  fprintf(log, "Surface %d x %d device pixels, scale %.2f, sun %.0f px\n", pw, ph, scale, r.sunRadius());
  fprintf(log, "Warm-up: %.0f ms for 720 frames\n", ms(t0, Clock::now()));

  const int N = 480;
  double sum = 0, worst = 0;
  int over4 = 0;
  for (int i = 0; i < N; i++) {
    auto a = Clock::now();
    r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
    r.sync();
    double t = ms(a, Clock::now());
    sum += t;
    worst = std::max(worst, t);
    over4 += t > 4.0;
  }
  fprintf(log, "Run: mean %.2f ms, longest %.2f ms, over 4 ms: %d of %d\n", sum / N, worst, over4, N);

  fprintf(log, "Sky pass in steps:");
  double pass = 0;
  for (double t : r.timeSteps(skyOut.rtv.Get())) {
    fprintf(log, " %.1f", t);
    pass += t;
  }
  fprintf(log, " = %.1f ms\n", pass);

  // Dragging the sun: every frame a whole sky pass.
  sum = worst = 0;
  for (int i = 0; i < 24; i++) {
    auto a = Clock::now();
    r.setScene(scene, sx + i * 6.0f, sy - i * 4.0f);
    r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
    r.sync();
    double t = ms(a, Clock::now());
    sum += t;
    worst = std::max(worst, t);
  }
  fprintf(log, "Dragging the sun: mean %.2f ms, longest %.2f ms\n", sum / 24, worst);
  r.setScene(scene, sx, sy);
  r.render(0, skyOut.rtv.Get(), lightOut.rtv.Get());
  savePng(dir + L"\\clouds_one.png", crop(readBack(ctx.Get(), skyOut, pw, ph), int(pw * 0.02f), int(ph * 0.12f), 640, 360));
  for (int i = 0; i < 300; i++) r.render(0, skyOut.rtv.Get(), lightOut.rtv.Get());
  r.sync();
  savePng(dir + L"\\clouds_many.png", crop(readBack(ctx.Get(), skyOut, pw, ph), int(pw * 0.02f), int(ph * 0.12f), 640, 360));

  // Compose the image as on the desktop: sky, dark window areas, plus light.
  Image sky = readBack(ctx.Get(), skyOut, pw, ph), light = readBack(ctx.Get(), lightOut, pw, ph);
  Image out = compose(sky, light, scene, mask, scale, 1);
  for (size_t i = 3; i < light.px.size(); i += 4) light.px[i] = 255;
  int f = std::max(1, pw / 1280);
  bool ok = savePng(dir + L"\\image.png", shrink(out, f));
  ok &= savePng(dir + L"\\light.png", shrink(light, f));
  ok &= savePng(dir + L"\\crop.png", crop(out, int(sx * scale) - 640, int(sy * scale) - 500, 1280, 720));
  ok &= savePng(dir + L"\\clouds.png", crop(out, int(pw * 0.02f), int(ph * 0.12f), 1280, 720));

  // Drifting clouds: three images of the sky, six seconds apart each, downscaled
  // and as a crop at full sharpness. The clouds must move on and stay sharp while
  // doing so.
  for (int n = 0; n < 3; n++) {
    for (int i = 0; i < 360; i++) r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
    r.sync();
    Image shot = readBack(ctx.Get(), skyOut, pw, ph);
    wchar_t name[32];
    swprintf_s(name, L"\\drift%d.png", n + 1);
    ok &= savePng(dir + name, shrink(shot, f));
    swprintf_s(name, L"\\drift%d_near.png", n + 1);
    ok &= savePng(dir + name, crop(shot, int(pw * 0.05f), int(ph * 0.25f), 1280, 720));
  }

  // A window in motion, but the image still shows its position one frame earlier:
  // around its edges the light must be suspended, otherwise it would be offset.
  std::vector<Occluder> moving = scene;
  for (size_t i = 0; i < moving.size(); i++) moving[i].id = 100 + i;
  r.setScene(moving, sx, sy);
  for (int i = 0; i < 90; i++) r.render(1.0 / 120, skyOut.rtv.Get(), lightOut.rtv.Get());
  std::vector<Occluder> shown = moving;
  for (int i = 0; i < 8; i++) {
    shown = moving;
    moving[1].x0 += 9, moving[1].x1 += 9;
    r.setScene(shown, sx, sy);
    r.render(1.0 / 120, skyOut.rtv.Get(), lightOut.rtv.Get());
  }
  r.sync();
  sky = readBack(ctx.Get(), skyOut, pw, ph);
  light = readBack(ctx.Get(), lightOut, pw, ph);
  out = compose(sky, light, moving, mask, scale, 1);
  ok &= savePng(dir + L"\\moved.png", crop(out, int(moving[1].x0 * scale) - 200, int(moving[1].y1 * scale) - 520, 1280, 720));

  // A mouse pointer diagonally below the sun: its shadow extends away from the sun
  // and fades out, its outline glows.
  {
    const int P = 16, S = 48, C = S + 2 * P;
    std::vector<uint8_t> arrow(size_t(C) * C, 0);
    for (int y = 0; y < 30; y++) {
      for (int x = 0; x < 20; x++) {
        // Arrow tip at the top left: a triangle with a vertical left edge.
        if (x <= y * 0.68f && y <= 30 - x * 0.45f) arrow[size_t(P + y) * C + P + x] = 255;
      }
    }
    r.setScene(scene, sx, sy);
    r.setCursorShape(arrow.data(), C, C);
    const float spots[3][2] = {{sx - 150, sy + 110}, {sx + 24, sy + 150}, {sx - 270, sy + 14}};
    for (int n = 0; n < 3; n++) {
      float cx = spots[n][0], cy = spots[n][1];
      r.setCursor(true, cx - P / scale, cy - P / scale, cx + (S + P) / scale, cy + (S + P) / scale, 1);
      for (int i = 0; i < 40; i++) r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
      r.sync();
      sky = readBack(ctx.Get(), skyOut, pw, ph);
      light = readBack(ctx.Get(), lightOut, pw, ph);
      Image with = compose(sky, light, scene, mask, scale, 1);
      // The pointer itself is above everything: white with a dark border.
      for (int y = 0; y < C; y++) {
        for (int x = 0; x < C; x++) {
          if (!arrow[size_t(y) * C + x]) continue;
          int X = int(cx * scale) - P + x, Y = int(cy * scale) - P + y;
          if (X < 0 || Y < 0 || X >= pw || Y >= ph) continue;
          bool edge = !arrow[size_t(y - 1) * C + x] || !arrow[size_t(y + 1) * C + x] || !arrow[size_t(y) * C + x - 1] || !arrow[size_t(y) * C + x + 1];
          uint8_t v = edge ? 20 : 250;
          with.at(X, Y)[0] = with.at(X, Y)[1] = with.at(X, Y)[2] = v;
        }
      }
      wchar_t name[32];
      swprintf_s(name, L"\\pointer%d.png", n + 1);
      ok &= savePng(dir + name, crop(with, int(cx * scale) - 640, int(cy * scale) - 300, 1280, 720));
    }
    r.setCursor(false, 0, 0, 0, 0, 1);
  }

  // HDR mode simulated: the light is output linearly and composed the way Windows
  // does it there. The result must be the same image as on the previous path.
  {
    const float white = 3.0f;
    r.setScene(scene, sx, sy);
    for (int i = 0; i < 90; i++) r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
    for (int i = 0; i < 40; i++) r.render(0, skyOut.rtv.Get(), lightOut.rtv.Get());
    r.sync();
    sky = readBack(ctx.Get(), skyOut, pw, ph);
    light = readBack(ctx.Get(), lightOut, pw, ph);
    Image plain = compose(sky, light, scene, mask, scale, 1);
    Offscreen wideOut = makeOffscreen(dev.Get(), pw, ph, DXGI_FORMAT_R16G16B16A16_FLOAT);
    r.setOutput(true, white);
    for (int i = 0; i < 40; i++) r.render(0, skyOut.rtv.Get(), wideOut.rtv.Get());
    r.sync();
    std::vector<float> wide = readBackWide(ctx.Get(), wideOut, pw, ph);
    Image hdr = composeLinear(sky, wide, white, scene, mask, scale, 1);
    r.setOutput(false, 1);
    // Deviation over open sky and over windows, in levels of 255.
    std::vector<uint8_t> covered(size_t(pw) * ph, 0);
    for (const Occluder& o : scene) {
      int x0 = std::max(0, int(o.x0 * scale)), y0 = std::max(0, int(o.y0 * scale));
      int x1 = std::min(pw, int(o.x1 * scale)), y1 = std::min(ph, int(o.y1 * scale));
      for (int y = y0; y < y1; y++) memset(&covered[size_t(y) * pw + x0], 1, size_t(std::max(0, x1 - x0)));
    }
    double sum[2] = {}, most[2] = {};
    size_t count[2] = {};
    for (size_t i = 0; i < covered.size(); i++) {
      int k = covered[i] || mask[i] ? 1 : 0;
      for (int c = 0; c < 3; c++) {
        double d = std::abs(int(plain.px[i * 4 + c]) - int(hdr.px[i * 4 + c]));
        sum[k] += d;
        most[k] = std::max(most[k], d);
      }
      count[k] += 3;
    }
    fprintf(log, "HDR path against the previous path: over open sky mean %.2f, at most %.0f; over windows and icons mean %.2f, at most %.0f (of 255)\n",
            sum[0] / std::max<size_t>(count[0], 1), most[0], sum[1] / std::max<size_t>(count[1], 1), most[1]);
    ok &= savePng(dir + L"\\image_hdr.png", shrink(hdr, f));
    ok &= savePng(dir + L"\\image_sdr.png", shrink(plain, f));
  }
  fprintf(log, ok ? "Images written\n" : "Images could not be written\n");
  fclose(log);
  return ok ? 0 : 5;
}

// Renders the image for the real position of windows and icons, without showing
// anything. Windows that fill the whole screen count as minimized. If the folder
// contains a windows.txt (per line x0 y0 x1 y1 in logical pixels), it replaces
// the real windows.
int runReal(const std::wstring& dir, float sunX, float sunY) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  CreateDirectoryW(dir.c_str(), nullptr);
  FILE* log = nullptr;
  _wfopen_s(&log, (dir + L"\\real.txt").c_str(), L"w");
  if (!log) return 2;

  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level;
  Renderer r;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, &level, &ctx)) ||
      !r.init(dev.Get(), ctx.Get())) {
    fprintf(log, "no Direct3D device\n");
    fclose(log);
    return 3;
  }
  r.stage({});
  Monitor m = primaryMonitor();
  int pw = m.width(), ph = m.height();
  float scale = m.scale, vw = pw / scale, vh = ph / scale;
  r.resize(pw, ph, scale);

  {
    Params p;
    double shift = -1;
    if (readValues(dir, log, p, shift)) r.setParams(p);
    if (shift >= 0) r.seek(shift);
  }
  FILE* in = nullptr;

  Scene found = captureScene(m, {});
  std::vector<Occluder> scene;
  if (!_wfopen_s(&in, (dir + L"\\windows.txt").c_str(), L"r") && in) {
    float a, b, c, d;
    while (fscanf_s(in, "%f %f %f %f", &a, &b, &c, &d) == 4) scene.push_back({a, b, c, d, 8, 1, 1});
    fclose(in);
    for (const Occluder& o : found.rects) {
      if (o.kind == 2) scene.push_back(o);
    }
  } else {
    for (const Occluder& o : found.rects) {
      bool fills = o.kind == 1 && (o.x1 - o.x0) * (o.y1 - o.y0) > 0.9f * vw * vh;
      if (!fills) scene.push_back(o);
    }
  }
  r.setScene(scene, sunX * vw, sunY * vh);
  fprintf(log, "Surface %d x %d device pixels, scale %.2f, sun at %.0f %.0f\n", pw, ph, scale, sunX * vw, sunY * vh);
  for (const Occluder& o : scene) fprintf(log, "  rect %6.0f %6.0f %6.0f %6.0f  kind %d  opacity %.2f\n", o.x0, o.y0, o.x1, o.y1, o.kind, o.opacity);

  std::vector<uint8_t> mask;
  uint64_t stamp = 0;
  if (!captureIcons(m, mask, stamp)) mask.assign(size_t(pw) * ph, 0);
  r.setIconMask(mask.data(), pw, ph);

  Offscreen skyOut = makeOffscreen(dev.Get(), pw, ph), lightOut = makeOffscreen(dev.Get(), pw, ph);
  for (int i = 0; i < 120; i++) r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
  for (int i = 0; i < 600; i++) r.render(1.0 / 20, skyOut.rtv.Get(), lightOut.rtv.Get());
  r.sync();
  Image sky = readBack(ctx.Get(), skyOut, pw, ph), light = readBack(ctx.Get(), lightOut, pw, ph);

  // How much light is added where, in levels of 255: over open sky and over
  // windows.
  std::vector<uint8_t> over(size_t(pw) * ph, 0);
  for (const Occluder& o : scene) {
    int x0 = std::max(0, int(o.x0 * scale)), y0 = std::max(0, int(o.y0 * scale));
    int x1 = std::min(pw, int(o.x1 * scale)), y1 = std::min(ph, int(o.y1 * scale));
    for (int y = y0; y < y1; y++) memset(&over[size_t(y) * pw + x0], o.kind, size_t(std::max(0, x1 - x0)));
  }
  const char* names[3] = {"open sky", "windows", "taskbar"};
  for (int kind = 0; kind < 3; kind++) {
    std::vector<int> hist(256, 0);
    double sum = 0, dim = 0;
    int deepest = 0;
    size_t n = 0;
    for (size_t i = 0; i < over.size(); i++) {
      if (over[i] != kind) continue;
      int v = std::max(light.px[i * 4], std::max(light.px[i * 4 + 1], light.px[i * 4 + 2]));
      hist[v]++;
      sum += v;
      dim += light.px[i * 4 + 3];
      deepest = std::max(deepest, int(light.px[i * 4 + 3]));
      n++;
    }
    if (!n) continue;
    auto quant = [&](double q) {
      size_t need = size_t(q * n), seen = 0;
      for (int v = 0; v < 256; v++) {
        seen += hist[v];
        if (seen >= need) return v;
      }
      return 255;
    };
    fprintf(log, "Light over %-13s: mean %5.1f, half below %3d, 90 %% below %3d, 99 %% below %3d, highest %3d; darkened by %4.1f %% on average, at most %4.1f %%  (%.0f %% of the area)\n",
            names[kind], sum / n, quant(0.5), quant(0.9), quant(0.99), quant(1.0), dim / n / 2.55, deepest / 2.55, 100.0 * n / over.size());
  }

  Image out = compose(sky, light, scene, mask, scale, 12);
  for (size_t i = 3; i < light.px.size(); i += 4) light.px[i] = 255;
  // In the light image alone the labels of the icons would be readable; it stays
  // black there.
  for (int by = 0; by < ph; by += 12) {
    for (int bx = 0; bx < pw; bx += 12) {
      int x1 = std::min(pw, bx + 12), y1 = std::min(ph, by + 12);
      bool any = false;
      for (int y = by; y < y1 && !any; y++) {
        for (int x = bx; x < x1 && !any; x++) any = mask[size_t(y) * pw + x] > 0;
      }
      if (!any) continue;
      for (int y = by; y < y1; y++) memset(light.at(bx, y), 0, size_t(x1 - bx) * 4);
    }
  }
  for (size_t i = 3; i < light.px.size(); i += 4) light.px[i] = 255;
  int f = std::max(1, pw / 1280);
  bool ok = savePng(dir + L"\\real.png", shrink(out, f));
  ok &= savePng(dir + L"\\real_light.png", shrink(light, f));
  ok &= savePng(dir + L"\\real_source.png", crop(out, int(sunX * pw) - 640, int(sunY * ph) - 360, 1280, 720));
  fprintf(log, ok ? "Images written\n" : "Images could not be written\n");
  fclose(log);
  return ok ? 0 : 5;
}

// The sky over the day: renders the image of an empty desktop for a series of
// times or of sun positions set by hand. The folder may contain a cases.txt,
// with one of these per line
//   time <Name> <year> <month> <day> <hour> <minute>  local time of the computer
//   hand <Name> <x> <y>                               sun position as a fraction
//                                                     of the image
//   moon <Name> <x> <y> <lit> <degrees>               night, moon at this spot,
//                                                     fraction and direction of
//                                                     the bright side
// If the folder contains a source.txt, the sheet shows the surroundings of the
// light source at full sharpness instead of the whole image.
// The folder may also contain a place.txt with latitude and longitude.
int runSky(const std::wstring& dir, int viewW, int viewH, float scale) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  CreateDirectoryW(dir.c_str(), nullptr);
  FILE* log = nullptr;
  _wfopen_s(&log, (dir + L"\\sky.txt").c_str(), L"w");
  if (!log) return 2;
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level;
  Renderer r;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, &level, &ctx)) ||
      !r.init(dev.Get(), ctx.Get())) {
    fprintf(log, "no Direct3D device\n");
    fclose(log);
    return 3;
  }
  r.stage({});
  int pw = int(std::lround(viewW * scale)), ph = int(std::lround(viewH * scale));
  r.resize(pw, ph, scale);
  Params p;
  double shift = -1;
  readValues(dir, log, p, shift);
  r.setParams(p);

  double latitude = 52.52, longitude = 13.41;
  FILE* in = nullptr;
  if (!_wfopen_s(&in, (dir + L"\\place.txt").c_str(), L"r") && in) {
    float a, b;
    if (fscanf_s(in, "%f %f", &a, &b) == 2) latitude = a, longitude = b;
    fclose(in);
  }

  // flash.txt: every image contains a lightning flash at full brightness.
  bool lightning = GetFileAttributesW((dir + L"\\flash.txt").c_str()) != INVALID_FILE_ATTRIBUTES;

  // props.txt: what is in the image instead of what would appear by chance. Per
  // line a model (container, freighter, dinghy, airliner, gull), its position in
  // meters (right, above the water, ahead) and its heading in degrees (0 away
  // from the viewer, 90 to the right); then optionally its speed in meters per
  // second, nose up and right side down in degrees, the strength of the wing beat
  // (0 to 1) and 1 for mirrored.
  if (!_wfopen_s(&in, (dir + L"\\props.txt").c_str(), L"r") && in) {
    static const char* names[MODEL_COUNT] = {"container", "freighter", "dinghy", "airliner", "gull"};
    std::vector<Life::Staged> staged;
    char line[256];
    fprintf(log, "In the image:\n");
    while (fgets(line, sizeof(line), in)) {
      char name[32] = "";
      float x = 0, y = 0, z = 0, heading = 0, speed = 0, pitch = 0, bank = 0, flap = 0, mirrored = 0;
      if (sscanf_s(line, "%31s %f %f %f %f %f %f %f %f %f", name, unsigned(sizeof(name)), &x, &y, &z, &heading, &speed, &pitch, &bank, &flap, &mirrored) < 5) continue;
      for (int i = 0; i < MODEL_COUNT; i++) {
        if (strcmp(name, names[i])) continue;
        const float rad = 3.14159265f / 180;
        Life::Staged s;
        s.prop.model = i;
        s.prop.pos[0] = x, s.prop.pos[1] = y, s.prop.pos[2] = z;
        s.prop.heading = heading * rad, s.prop.pitch = pitch * rad, s.prop.bank = bank * rad;
        s.prop.mirrored = mirrored > 0.5f;
        s.velocity[0] = std::sin(heading * rad) * speed, s.velocity[2] = std::cos(heading * rad) * speed;
        s.flap = flap;
        staged.push_back(s);
        fprintf(log, "  %s at %g %g %g, heading %g\n", name, x, y, z, heading);
      }
    }
    fclose(in);
    r.stage(staged);
  }

  // near.txt: center of a close-up at full sharpness, as a fraction of the image.
  float nearX = -1, nearY = -1;
  if (!_wfopen_s(&in, (dir + L"\\near.txt").c_str(), L"r") && in) {
    if (fscanf_s(in, "%f %f", &nearX, &nearY) != 2) nearX = -1;
    fclose(in);
  }

  struct Case {
    std::string name;
    bool hand = false, moon = false;
    double time = 0;
    float x = 0, y = 0, lit = 1, limb = 0;
  };
  bool closeUp = GetFileAttributesW((dir + L"\\source.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
  // If source.txt contains a number greater than 1, the surroundings of the light
  // source are shown magnified that many times, each pixel as a block: for looking
  // at edges.
  int zoom = 1;
  if (closeUp && !_wfopen_s(&in, (dir + L"\\source.txt").c_str(), L"r") && in) {
    if (fscanf_s(in, "%d", &zoom) != 1) zoom = 1;
    zoom = std::clamp(zoom, 1, 8);
    fclose(in);
  }
  std::vector<Case> cases;
  auto local = [](int y, int mo, int d, int h, int mi) {
    std::tm t = {};
    t.tm_year = y - 1900, t.tm_mon = mo - 1, t.tm_mday = d, t.tm_hour = h, t.tm_min = mi, t.tm_isdst = -1;
    return double(std::mktime(&t));
  };
  if (!_wfopen_s(&in, (dir + L"\\cases.txt").c_str(), L"r") && in) {
    char kind[16], name[64];
    while (fscanf_s(in, "%15s %63s", kind, unsigned(sizeof(kind)), name, unsigned(sizeof(name))) == 2) {
      Case c;
      c.name = name;
      if (!strcmp(kind, "hand")) {
        c.hand = true;
        if (fscanf_s(in, "%f %f", &c.x, &c.y) != 2) break;
      } else if (!strcmp(kind, "moon")) {
        c.hand = c.moon = true;
        if (fscanf_s(in, "%f %f %f %f", &c.x, &c.y, &c.lit, &c.limb) != 4) break;
      } else {
        int y, mo, d, h, mi;
        if (fscanf_s(in, "%d %d %d %d %d", &y, &mo, &d, &h, &mi) != 5) break;
        c.time = local(y, mo, d, h, mi);
      }
      cases.push_back(c);
    }
    fclose(in);
  }
  if (cases.empty()) {
    const int minutes[] = {6 * 60 + 40, 7 * 60 + 30, 8 * 60 + 5, 8 * 60 + 30, 9 * 60 + 30, 12 * 60 + 15,
                           15 * 60, 16 * 60 + 5, 16 * 60 + 35, 17 * 60, 17 * 60 + 30, 21 * 60};
    for (int m : minutes) {
      Case c;
      char name[32];
      sprintf_s(name, "day_%02d%02d", m / 60, m % 60);
      c.name = name;
      c.time = local(2026, 11, 23, m / 60, m % 60);
      cases.push_back(c);
    }
  }

  const float bar = 48;
  std::vector<Occluder> scene;
  scene.push_back({0, float(viewH) - bar, float(viewW), float(viewH), 0, 0.88f, 2});
  // windows.txt: additional windows, per line left, top, right, bottom in logical
  // pixels.
  {
    FILE* win = nullptr;
    if (!_wfopen_s(&win, (dir + L"\\windows.txt").c_str(), L"r") && win) {
      float a, b, c, d;
      while (fscanf_s(win, "%f %f %f %f", &a, &b, &c, &d) == 4) scene.insert(scene.begin(), {a, b, c, d, 8, 1, 1});
      fclose(win);
    }
  }
  r.setScene(scene);
  std::vector<uint8_t> mask(size_t(pw) * ph, 0);
  r.setIconMask(mask.data(), pw, ph);
  Offscreen skyOut = makeOffscreen(dev.Get(), pw, ph), lightOut = makeOffscreen(dev.Get(), pw, ph);

  int f = std::max(1, pw / 1280), tile = 2 * f;
  int cols = 3, rows = (int(cases.size()) + cols - 1) / cols;
  Image sheet;
  sheet.w = (pw / tile / zoom * zoom) * cols;
  sheet.h = (ph / tile / zoom * zoom) * rows;
  sheet.px.assign(size_t(sheet.w) * sheet.h * 4, 255);
  bool ok = true;
  double now = double(std::time(nullptr));
  for (size_t n = 0; n < cases.size(); n++) {
    const Case& c = cases[n];
    Bodies b = bodiesAt(c.hand ? now : c.time, latitude, longitude);
    Heaven h = c.hand ? heavenOfHand(c.x * viewW, c.y * viewH, b, r) : heavenOfBodies(b, latitude, r);
    if (c.moon) {
      // Deep night, the moon is where it is supposed to be.
      h.sunX = 0.5f * viewW, h.sunY = float(viewH);
      h.moonX = c.x * viewW, h.moonY = c.y * viewH;
      h.moonLit = c.lit;
      h.moonLimb = c.limb * 3.14159265f / 180;
      h.moonSpin = 0.3f;
    }
    r.setHeaven(h);
    // The same cloud state in every image so that they can be compared.
    if (shift >= 0) r.seek(shift);
    for (int i = 0; i < 300; i++) {
      if (lightning && i == 240) r.flashNow();
      r.render(i < 260 ? 0.0 : 1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
    }
    // glide.txt (clouds, wind): afterwards the weather glides there, step by step
    // once per second as with live weather, without the sky starting over.
    {
      FILE* glide = nullptr;
      float toClouds = 0, toWind = 0;
      if (!_wfopen_s(&glide, (dir + L"\\glide.txt").c_str(), L"r") && glide) {
        if (fscanf_s(glide, "%f %f", &toClouds, &toWind) == 2) {
          float clouds = r.params().clouds, wind = r.params().wind;
          int unsettled = 0;
          for (int i = 0; i < 6000 && (clouds != toClouds || wind != toWind); i++) {
            if (i % 60 == 0) {
              clouds = std::abs(toClouds - clouds) < 0.03f ? toClouds : clouds + (toClouds > clouds ? 0.03f : -0.03f);
              wind = std::abs(toWind - wind) < 0.02f ? toWind : wind + (toWind > wind ? 0.02f : -0.02f);
              r.drift(clouds, wind);
            }
            r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
            unsettled += !r.skySettled();
          }
          for (int i = 0; i < 240; i++) r.render(1.0 / 60, skyOut.rtv.Get(), lightOut.rtv.Get());
          fprintf(log, "             glided to clouds %.2f, wind %.2f; frames without a finished sky on the way: %d\n", clouds, wind, unsettled);
        }
        fclose(glide);
      }
    }
    r.sync();
    Image sky = readBack(ctx.Get(), skyOut, pw, ph), light = readBack(ctx.Get(), lightOut, pw, ph);
    Image out = compose(sky, light, scene, mask, scale, 1);
    std::wstring name(c.name.begin(), c.name.end());
    ok &= savePng(dir + L"\\" + name + L".png", shrink(out, f));
    if (nearX >= 0) ok &= savePng(dir + L"\\" + name + L"_near.png", crop(out, int(nearX * pw) - 640, int(nearY * ph) - 360, 1280, 720));
    // sequence.txt: a lightning flash over time, sixteen frames from half a second
    // at 240 frames per second.
    if (GetFileAttributesW((dir + L"\\sequence.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
      const int picks[16] = {0, 1, 2, 4, 7, 10, 14, 19, 24, 32, 40, 52, 64, 80, 96, 112};
      Image strip;
      // What does a new lightning flash cost? With time standing still so that no
      // cloud pass gets in between: the first frame draws the channel, the second only
      // shows it.
      for (int i = 0; i < 60; i++) r.render(0.0, skyOut.rtv.Get(), lightOut.rtv.Get());
      fprintf(log, "             Lightning, first and second frame in ms:");
      for (int k = 0; k < 4; k++) {
        r.sync();
        r.flashNow(false);
        double took[2];
        for (double& t : took) {
          auto began = std::chrono::steady_clock::now();
          r.render(0.0, skyOut.rtv.Get(), lightOut.rtv.Get());
          r.sync();
          t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        }
        fprintf(log, "  %.1f %.1f", took[0], took[1]);
      }
      fprintf(log, "\n");
      r.flashNow(false);
      bool peak = false;
      for (int i = 0, at = 0; i <= picks[15]; i++) {
        r.render(1.0 / 240, skyOut.rtv.Get(), lightOut.rtv.Get());
        // The brightest frame at full size, plus the sky alone without the light in
        // front of it.
        if (!peak && r.flashLevel() > 0.5f) {
          peak = true;
          r.sync();
          Image s3 = readBack(ctx.Get(), skyOut, pw, ph), l3 = readBack(ctx.Get(), lightOut, pw, ph);
          ok &= savePng(dir + L"\\" + name + L"_peak.png", compose(s3, l3, scene, mask, scale, 1));
          std::fill(l3.px.begin(), l3.px.end(), uint8_t(0));
          ok &= savePng(dir + L"\\" + name + L"_peak_sky.png", compose(s3, l3, scene, mask, scale, 1));
        }
        if (i != picks[at]) continue;
        r.sync();
        Image s2 = readBack(ctx.Get(), skyOut, pw, ph), l2 = readBack(ctx.Get(), lightOut, pw, ph);
        Image small2 = shrink(compose(s2, l2, scene, mask, scale, 1), 6);
        if (at == 0) {
          strip.w = small2.w * 4, strip.h = small2.h * 4;
          strip.px.assign(size_t(strip.w) * strip.h * 4, 255);
        }
        int ox2 = (at % 4) * small2.w, oy2 = (at / 4) * small2.h;
        for (int y = 0; y < small2.h; y++) memcpy(strip.at(ox2, oy2 + y), small2.at(0, y), size_t(small2.w) * 4);
        at++;
      }
      ok &= savePng(dir + L"\\" + name + L"_sequence.png", strip);
    }
    // run.txt (count, interval in seconds, height in pixels): that many crops around
    // the spot from near.txt over time, one below the other. For everything that
    // moves: water, rain.
    {
      FILE* run = nullptr;
      int count = 0, tall = 0;
      float step = 0;
      if (nearX >= 0 && !_wfopen_s(&run, (dir + L"\\run.txt").c_str(), L"r") && run) {
        if (fscanf_s(run, "%d %f %d", &count, &step, &tall) != 3) count = 0;
        fclose(run);
      }
      if (count > 0 && tall > 0) {
        Image strip;
        strip.w = 1280, strip.h = tall * count;
        strip.px.assign(size_t(strip.w) * strip.h * 4, 255);
        // The timings separately: calm frames (only the water is made anew) and full
        // ones.
        std::vector<double> quick, whole;
        for (int k = 0; k < count; k++) {
          int sub = k > 0 ? std::max(1, int(std::lround(step * 240))) : 0;
          for (int i = 0; i < sub; i++) {
            r.sync();
            auto began = std::chrono::steady_clock::now();
            Renderer::Drawn drawn = r.render(step / sub, skyOut.rtv.Get(), lightOut.rtv.Get());
            r.sync();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            (drawn.sky && !drawn.light && drawn.lively ? quick : whole).push_back(ms);
          }
          r.sync();
          Image s4 = readBack(ctx.Get(), skyOut, pw, ph), l4 = readBack(ctx.Get(), lightOut, pw, ph);
          Image part = crop(compose(s4, l4, scene, mask, scale, 1), int(nearX * pw) - 640, int(nearY * ph) - tall / 2, 1280, tall);
          for (int y = 0; y < tall; y++) memcpy(strip.at(0, k * tall + y), part.at(0, y), size_t(1280) * 4);
        }
        ok &= savePng(dir + L"\\" + name + L"_run.png", strip);
        // Median and fastest tenth: if the run shares the graphics card with other work,
        // the tenth shows the true cost better.
        auto part = [](std::vector<double>& v, size_t of) {
          if (v.empty()) return 0.0;
          std::sort(v.begin(), v.end());
          return v[v.size() / of];
        };
        fprintf(log, "             Run: %d intermediate frames, median %.2f ms, fastest tenth %.2f ms; %d full, median %.2f ms, fastest tenth %.2f ms\n",
                int(quick.size()), part(quick, 2), part(quick, 10), int(whole.size()), part(whole, 2), part(whole, 10));
      }
    }
    Image thumb = closeUp ? crop(out, int(r.sourceX() * scale) - pw / tile / zoom / 2, int(r.sourceY() * scale) - ph / tile / zoom / 2,
                                 pw / tile / zoom, ph / tile / zoom)
                          : shrink(out, tile);
    if (closeUp && zoom > 1) {
      Image big;
      big.w = thumb.w * zoom;
      big.h = thumb.h * zoom;
      big.px.resize(size_t(big.w) * big.h * 4);
      for (int y = 0; y < big.h; y++) {
        for (int x = 0; x < big.w; x++) memcpy(big.at(x, y), thumb.at(x / zoom, y / zoom), 4);
      }
      thumb = big;
    }
    int ox = int(n % cols) * thumb.w, oy = int(n / cols) * thumb.h;
    for (int y = 0; y < thumb.h; y++) memcpy(sheet.at(ox, oy + y), thumb.at(0, y), size_t(thumb.w) * 4);
    // Mean color of the sky at the top and above the horizon, for comparison.
    auto mean = [&](float y0, float y1, int out3[3]) {
      uint64_t sum[3] = {};
      uint64_t count = 0;
      for (int y = int(ph * y0); y < int(ph * y1); y += 4) {
        for (int x = 0; x < pw; x += 4) {
          const uint8_t* q = sky.at(x, y);
          sum[0] += q[2], sum[1] += q[1], sum[2] += q[0];
          count++;
        }
      }
      for (int k = 0; k < 3; k++) out3[k] = int(sum[k] / std::max<uint64_t>(count, 1));
    };
    int top[3], low[3];
    mean(0.02f, 0.2f, top);
    mean(0.55f, 0.7f, low);
    fprintf(log, "%-12s Sun %5.0f %5.0f (true altitude %6.1f)  Moon %5.0f %5.0f lit %.2f (altitude %6.1f)  Source %s  Sky top %3d %3d %3d, bottom %3d %3d %3d\n",
            c.name.c_str(), h.sunX, h.sunY, r.altitudeOfRow(h.sunY), h.moonX, h.moonY, h.moonLit, r.altitudeOfRow(h.moonY),
            r.moonShines() ? "moon" : "sun", top[0], top[1], top[2], low[0], low[1], low[2]);
    if (!c.hand) fprintf(log, "             Sun az %.1f alt %.1f, moon az %.1f alt %.1f\n", b.sunAz, b.sunAlt, b.moonAz, b.moonAlt);
    if (r.rainHere() > 0) fprintf(log, "             Rain at the viewer %.2f\n", r.rainHere());
    fflush(log);
  }
  ok &= savePng(dir + L"\\sheet.png", sheet);
  // The depth map by which the clouds are moved along.
  {
    Image map;
    if (r.dumpDepth(map.px, map.w, map.h)) ok &= savePng(dir + L"\\depth.png", shrink(map, 2));
  }
  // What does a cloud pass cost, band by band?
  double pass = 0, worst = 0;
  fprintf(log, "Sky pass in steps:");
  for (double t : r.timeSteps(skyOut.rtv.Get())) {
    fprintf(log, " %.1f", t);
    pass += t;
    worst = std::max(worst, t);
  }
  fprintf(log, " = %.1f ms, longest step %.1f ms\n", pass, worst);
  fprintf(log, ok ? "Images written\n" : "Images could not be written\n");
  fclose(log);
  return ok ? 0 : 5;
}

// Drifting clouds: between two computation passes the shown image only moves the
// clouds on. Here it is compared with what would be correct at exactly that
// moment (the same cloud state, fully computed at rest). Per sample one line in
// drift.txt and one image: shown, correct, difference amplified four times.
// Errors at the edges of the clouds are the fringe that a cloud drags behind it.
int runDrift(const std::wstring& dir, int viewW, int viewH, float scale) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  CreateDirectoryW(dir.c_str(), nullptr);
  FILE* log = nullptr;
  _wfopen_s(&log, (dir + L"\\drift.txt").c_str(), L"w");
  if (!log) return 2;
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level;
  Renderer moving, still;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, &level, &ctx)) ||
      !moving.init(dev.Get(), ctx.Get()) || !still.init(dev.Get(), ctx.Get())) {
    fclose(log);
    return 3;
  }
  int pw = int(std::lround(viewW * scale)), ph = int(std::lround(viewH * scale));
  Params p;
  double shift = -1;
  readValues(dir, log, p, shift);
  Params calm = p;
  calm.wind = 0;
  std::vector<Occluder> scene;
  scene.push_back({0, float(viewH) - 48, float(viewW), float(viewH), 0, 0.88f, 2});
  std::vector<uint8_t> mask(size_t(pw) * ph, 0);
  // sun.txt in the folder: position of the sun as a fraction of the image (far
  // down: night with moon).
  float sunX = 0.5f, sunY = 0.54f;
  FILE* in = nullptr;
  if (!_wfopen_s(&in, (dir + L"\\sun.txt").c_str(), L"r") && in) {
    if (fscanf_s(in, "%f %f", &sunX, &sunY) != 2) sunX = 0.5f, sunY = 0.54f;
    fclose(in);
  }
  for (Renderer* r : {&moving, &still}) {
    r->resize(pw, ph, scale);
    r->setParams(r == &moving ? p : calm);
    r->setScene(scene);
    r->setIconMask(mask.data(), pw, ph);
    r->setHeaven(heavenOfHand(sunX * viewW, sunY * viewH, bodiesAt(1795478400.0, 52.5, 13.4), *r));
  }
  if (shift >= 0) moving.seek(shift);
  Offscreen skyOut = makeOffscreen(dev.Get(), pw, ph), lightOut = makeOffscreen(dev.Get(), pw, ph);
  Offscreen skyRef = makeOffscreen(dev.Get(), pw, ph);
  const double dt = 1.0 / 120;
  for (int i = 0; i < 480; i++) moving.render(dt, skyOut.rtv.Get(), lightOut.rtv.Get());

  bool ok = true;
  const float spots[4][2] = {{0.30f, 0.22f}, {0.66f, 0.30f}, {0.48f, 0.42f}, {0.82f, 0.18f}};
  for (int k = 0; k < 6; k++) {
    // Seven frames further: this way every sample falls on a different point between
    // two passes.
    for (int i = 0; i < 7; i++) moving.render(dt, skyOut.rtv.Get(), lightOut.rtv.Get());
    moving.sync();
    Image shown = readBack(ctx.Get(), skyOut, pw, ph);
    still.seek(moving.cloudShift(), moving.cloudAge());
    for (int i = 0; i < 240; i++) still.render(0, skyRef.rtv.Get(), nullptr);
    still.sync();
    Image right = readBack(ctx.Get(), skyRef, pw, ph);
    // Error over the sky above the mountains: on average, and the fraction of the
    // pixels that deviate by more than 12 of 255 levels.
    double sum = 0;
    size_t off = 0, count = 0;
    for (int y = 0; y < int(ph * 0.62); y++) {
      for (int x = 0; x < pw; x++) {
        const uint8_t* a = shown.at(x, y);
        const uint8_t* b = right.at(x, y);
        int d = std::max(std::abs(a[0] - b[0]), std::max(std::abs(a[1] - b[1]), std::abs(a[2] - b[2])));
        sum += d;
        off += d > 12;
        count++;
      }
    }
    fprintf(log, "Sample %d: mean deviation %.2f of 255, more than 12 levels at %.2f %% of the pixels\n", k + 1, sum / count, 100.0 * off / count);
    fflush(log);
    if (k == 0) {
      // The difference over the whole image, amplified so that even a faint veil is
      // visible; and the shown image brightened in the same way.
      double mean = 0;
      for (int y = 0; y < ph; y += 8) {
        for (int x = 0; x < pw; x += 8) mean += shown.at(x, y)[1];
      }
      mean /= double(pw / 8) * (ph / 8);
      int amp = std::clamp(int(600 / std::max(mean, 1.0)), 6, 48), lift = std::clamp(int(90 / std::max(mean, 1.0)), 1, 8);
      Image diff = shown, bright = shown;
      for (size_t i = 0; i < diff.px.size(); i += 4) {
        for (int c = 0; c < 3; c++) {
          diff.px[i + c] = uint8_t(std::min(255, amp * std::abs(int(shown.px[i + c]) - int(right.px[i + c]))));
          bright.px[i + c] = uint8_t(std::min(255, lift * int(shown.px[i + c])));
        }
      }
      int f = std::max(1, pw / 1280);
      ok &= savePng(dir + L"\\difference.png", shrink(diff, f));
      ok &= savePng(dir + L"\\shown.png", shrink(bright, f));
      fprintf(log, "Difference amplified %d times, shown image brightened %d times\n", amp, lift);
    }
    if (k >= 2) continue;
    for (int n = 0; n < 4; n++) {
      const int cw = 640, ch = 400;
      int x0 = std::clamp(int(spots[n][0] * pw) - cw / 2, 0, pw - cw), y0 = std::clamp(int(spots[n][1] * ph) - ch / 2, 0, ph - ch);
      Image out;
      out.w = cw * 3;
      out.h = ch;
      out.px.assign(size_t(out.w) * out.h * 4, 255);
      // Dark crops (night) brightened, otherwise nothing could be made out in them.
      double sum = 0;
      for (int y = 0; y < ch; y += 4) {
        for (int x = 0; x < cw; x += 4) sum += shown.at(x0 + x, y0 + y)[1];
      }
      int gain = std::clamp(int(70 / std::max(sum / (cw / 4 * (ch / 4)), 1.0)), 1, 8);
      for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
          const uint8_t* a = shown.at(x0 + x, y0 + y);
          const uint8_t* b = right.at(x0 + x, y0 + y);
          for (int c = 0; c < 3; c++) {
            out.at(x, y)[c] = uint8_t(std::min(255, gain * a[c]));
            out.at(cw + x, y)[c] = uint8_t(std::min(255, gain * b[c]));
            out.at(2 * cw + x, y)[c] = uint8_t(std::min(255, 4 * gain * std::abs(a[c] - b[c])));
          }
        }
      }
      wchar_t name[32];
      swprintf_s(name, L"\\drift%d_%c.png", k + 1, L'a' + n);
      ok &= savePng(dir + name, out);
    }
  }
  fclose(log);
  return ok ? 0 : 5;
}

// Draws the pages of the settings window to files without showing it.
int runPanelCheck(const std::wstring& dir) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  CreateDirectoryW(dir.c_str(), nullptr);
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level;
  Renderer r;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, &level, &ctx)) ||
      !r.init(dev.Get(), ctx.Get())) {
    return 3;
  }
  r.stage({});
  r.resize(3840, 2160, 1.5f);
  Params par;
  int course = 1;
  double latitude = 52.5, longitude = 13.4;
  float lapse = 10;
  // A morning, so that something is visible in the day arc.
  std::tm t = {};
  t.tm_year = 126, t.tm_mon = 10, t.tm_mday = 23, t.tm_hour = 10, t.tm_min = 40, t.tm_isdst = -1;
  double when = double(std::mktime(&t));
  r.setHeaven(heavenOfBodies(bodiesAt(when, latitude, longitude), latitude, r));
  PanelHost h;
  h.par = &par;
  h.course = &course;
  h.latitude = &latitude;
  h.longitude = &longitude;
  h.lapseMinutes = &lapse;
  h.render = &r;
  h.shownTime = [when] { return when; };
  h.autostart = [] { return true; };
  // If the folder contains a weather.txt, the switch for live weather is on.
  bool real = GetFileAttributesW((dir + L"\\weather.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
  h.realWeather = &real;
  h.place = [] { return std::wstring(L"Berlin, Germany"); };
  h.placeNote = [&] { return coordinateText(latitude, longitude); };
  return renderPanel(dir, h, 1.5f) ? 0 : 5;
}

int runWeatherCheck(const std::wstring& dir, const std::wstring& query) {
  CreateDirectoryW(dir.c_str(), nullptr);
  FILE* log = nullptr;
  _wfopen_s(&log, (dir + L"\\weather.txt").c_str(), L"w");
  if (!log) return 2;
  // Sample conditions: cloud cover, precipitation mm/h, wind, gusts, code.
  const struct {
    const char* name;
    float cover, rain, wind, gust;
    int code;
  } cases[] = {{"clear, calm", 0, 0, 0, 0, 0},          {"fair, light breeze", 0.3f, 0, 3, 5, 2},   {"cloudy, moderate wind", 0.7f, 0, 6, 10, 3},
               {"overcast", 1, 0, 4, 7, 3},                   {"fog", 1, 0, 1, 2, 45},                     {"drizzle", 1, 0.2f, 4, 7, 51},
               {"rain", 1, 2, 7, 12, 63},                   {"showers, heavy", 0.9f, 9, 9, 16, 82},        {"snowfall", 1, 1, 3, 6, 73},
               {"thunderstorm", 1, 6, 10, 19, 95},               {"gale without rain", 0.6f, 0, 17, 26, 3}};
  fprintf(log, "Sample conditions:\n");
  for (const auto& c : cases) {
    WeatherNow w;
    w.valid = true, w.cover = c.cover, w.rain = c.rain, w.wind = c.wind, w.gust = c.gust, w.code = c.code;
    float clouds = 0, wind = 0;
    weatherSliders(w, clouds, wind);
    fprintf(log, "  %-24s clouds %.2f  wind %.2f\n", c.name, clouds, wind);
  }
  // A response as the service gives it: is it read correctly?
  WeatherNow read;
  bool parsed = parseWeather("{\"latitude\":52.52,\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\",\"cloud_cover\":\"%\"},"
                             "\"current\":{\"time\":\"2026-11-23T16:15\",\"interval\":900,\"cloud_cover\":75,\"precipitation\":0.30,"
                             "\"weather_code\":61,\"wind_speed_10m\":5.4,\"wind_gusts_10m\":11.2,\"temperature_2m\":6.8}}",
                             read);
  fprintf(log, "Reading a sample response: %s (cloud cover %.2f, precipitation %.1f mm/h, wind %.1f, gusts %.1f, code %d, %.1f degrees)\n",
          parsed ? "ok" : "ERROR", read.cover, read.rain, read.wind, read.gust, read.code, read.temperature);
  // The place: coordinates as they are, a name through the place search.
  Place place;
  std::string error;
  bool located = parseCoordinates(query, place) || findPlace(query, place, error);
  fprintf(log, "Place \"%ls\": %s", query.c_str(), located ? "found" : (error.empty() ? "not found" : error.c_str()));
  if (located) fprintf(log, ", %ls, %.2f %.2f", place.name.empty() ? L"coordinates" : place.name.c_str(), place.latitude, place.longitude);
  fprintf(log, "\n");
  if (!located) {
    fclose(log);
    return 6;
  }
  double latitude = place.latitude, longitude = place.longitude;
  WeatherNow now;
  bool ok = fetchWeather(latitude, longitude, now, error);
  if (ok) {
    float clouds = 0, wind = 0;
    weatherSliders(now, clouds, wind);
    fprintf(log, "Request for %.1f %.1f: cloud cover %.0f %%, precipitation %.1f mm/h, wind %.1f m/s, gusts %.1f m/s, code %d, %.1f degrees; clouds %.2f, wind %.2f\n",
            latitude, longitude, now.cover * 100, now.rain, now.wind, now.gust, now.code, now.temperature, clouds, wind);
  } else {
    fprintf(log, "Request for %.1f %.1f failed: %s\n", latitude, longitude, error.c_str());
  }
  fclose(log);
  return ok && parsed ? 0 : 6;
}

namespace {

float halfToFloat(uint16_t h) {
  int e = (h >> 10) & 31, m = h & 1023;
  float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.0f : std::ldexp(float(m + 1024), e - 25);
  return (h & 0x8000) ? -v : v;
}

}  // namespace

// Reads the image that Windows actually sends to the display. In HDR mode this
// is a different one from what a screenshot shows: linear, and 1.0 stands for 80
// cd/m².
bool Desk::open(ID3D11Device* d, ID3D11DeviceContext* c) {
  dev = d;
  ctx = c;
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(error = d->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(error = dxgi->GetAdapter(&adapter))) return false;
  ComPtr<IDXGIOutput> out;
  for (UINT i = 0; adapter->EnumOutputs(i, &out) == S_OK; i++) {
    DXGI_OUTPUT_DESC od;
    out->GetDesc(&od);
    if (od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0) break;
    out.Reset();
  }
  ComPtr<IDXGIOutput5> out5;
  if (!out) {
    error = E_FAIL;
    return false;
  }
  if (FAILED(error = out.As(&out5))) return false;
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
  return SUCCEEDED(error = out5->DuplicateOutput1(d, 0, 2, formats, &dupl));
}

void Desk::refresh(int maxMs) {
  if (!dupl) return;
  auto start = std::chrono::steady_clock::now();
  for (;;) {
    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dupl->AcquireNextFrame(50, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) break;
    if (FAILED(hr)) {
      error = hr;
      break;
    }
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res.As(&tex))) {
      if (!last) {
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        format = td.Format;
        w = int(td.Width);
        h = int(td.Height);
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        dev->CreateTexture2D(&td, nullptr, &last);
        if (last) ctx->CopyResource(last.Get(), tex.Get());
      } else if (info.LastPresentTime.QuadPart != 0) {
        ctx->CopyResource(last.Get(), tex.Get());
      }
    }
    if (info.PointerShapeBufferSize > 0) {
      std::vector<uint8_t> shape(info.PointerShapeBufferSize);
      UINT need = 0;
      DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
      if (SUCCEEDED(dupl->GetFramePointerShape(UINT(shape.size()), shape.data(), &need, &si))) {
        pointerW = int(si.Width);
        pointerH = int(si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? si.Height / 2 : si.Height);
        pointerHotX = si.HotSpot.x;
        pointerHotY = si.HotSpot.y;
      }
    }
    if (info.LastMouseUpdateTime.QuadPart != 0 && info.PointerPosition.Visible) {
      pointerX = info.PointerPosition.Position.x;
      pointerY = info.PointerPosition.Position.y;
    }
    dupl->ReleaseFrame();
    if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > maxMs) break;
  }
}

bool Desk::read(int x, int y, int cw, int ch, std::vector<float>& rgb) {
  if (!last || x < 0 || y < 0 || x + cw > w || y + ch > h) return false;
  D3D11_TEXTURE2D_DESC td;
  last->GetDesc(&td);
  td.Width = cw;
  td.Height = ch;
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> st;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return false;
  D3D11_BOX box = {UINT(x), UINT(y), 0, UINT(x + cw), UINT(y + ch), 1};
  ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, last.Get(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  rgb.resize(size_t(cw) * ch * 3);
  for (int yy = 0; yy < ch; yy++) {
    const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(yy) * m.RowPitch;
    float* out = &rgb[size_t(yy) * cw * 3];
    for (int xx = 0; xx < cw; xx++) {
      if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const uint16_t* p = reinterpret_cast<const uint16_t*>(row + size_t(xx) * 8);
        for (int k = 0; k < 3; k++) out[xx * 3 + k] = halfToFloat(p[k]);
      } else {
        const uint8_t* p = row + size_t(xx) * 4;
        out[xx * 3] = p[2] / 255.0f, out[xx * 3 + 1] = p[1] / 255.0f, out[xx * 3 + 2] = p[0] / 255.0f;
      }
    }
  }
  ctx->Unmap(st.Get(), 0);
  return true;
}

bool Desk::readSrgb(float white, std::vector<uint8_t>& bgra) {
  if (!last) return false;
  D3D11_TEXTURE2D_DESC td;
  last->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> st;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return false;
  ctx->CopyResource(st.Get(), last.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  bgra.resize(size_t(w) * h * 4);
  bool wide = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  std::vector<uint8_t> lut;
  if (wide) {
    // Every possible value converted once: linear, divided by the brightness of
    // white, then as an image value.
    lut.resize(65536);
    for (int i = 0; i < 65536; i++) {
      float v = halfToFloat(uint16_t(i)) / white;
      v = std::clamp(v, 0.0f, 1.0f);
      v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
      lut[i] = uint8_t(std::lround(v * 255));
    }
  }
  for (int y = 0; y < h; y++) {
    const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
    uint8_t* out = &bgra[size_t(y) * w * 4];
    if (wide) {
      const uint16_t* p = reinterpret_cast<const uint16_t*>(row);
      for (int x = 0; x < w; x++) {
        out[x * 4] = lut[p[x * 4 + 2]], out[x * 4 + 1] = lut[p[x * 4 + 1]], out[x * 4 + 2] = lut[p[x * 4]], out[x * 4 + 3] = 255;
      }
    } else {
      memcpy(out, row, size_t(w) * 4);
    }
  }
  ctx->Unmap(st.Get(), 0);
  return true;
}

bool Desk::mean(int x, int y, int n, double out[3]) {
  std::vector<float> rgb;
  if (!read(x, y, n, n, rgb)) return false;
  double sum[3] = {};
  for (size_t i = 0; i < rgb.size(); i += 3) {
    for (int k = 0; k < 3; k++) sum[k] += rgb[i + k];
  }
  for (int k = 0; k < 3; k++) out[k] = sum[k] / (double(n) * n);
  return true;
}

// Measures on the real display how a window like the light window enters the
// image. For this a small pale patch is shown at two spots for a moment. It is
// read in two ways: like a screenshot (values 0 to 255) and the way Windows
// actually sends the image to the display (linear, 1.0 = 80 cd/m²). In HDR mode
// the two differ.
int runBlendCheck(const std::wstring& file) {
  FILE* log = nullptr;
  _wfopen_s(&log, file.c_str(), L"w");
  if (!log) return 2;
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level;
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, &level, &ctx)) ||
      FAILED(dev.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    fprintf(log, "no Direct3D device\n");
    fclose(log);
    return 3;
  }
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = inst;
  wc.lpszClassName = L"SunbeamProbe";
  RegisterClassExW(&wc);

  const int N = 96;
  Monitor m = primaryMonitor();
  Desk desk;
  bool live = desk.open(dev.Get(), ctx.Get());
  if (live) desk.refresh();
  fprintf(log, "Image sent to the display: %s, format %d, %d x %d (%08lx)\n", live && desk.last ? "readable" : "not readable", int(desk.format), desk.w,
          desk.h, desk.error);

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = N;
  bi.bmiHeader.biHeight = -N;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  uint32_t* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  auto photo = [&](POINT at, double out[3]) {
    BitBlt(mem, 0, 0, N, N, screen, at.x, at.y, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    double sum[3] = {};
    for (int i = 0; i < N * N; i++) {
      sum[0] += (bits[i] >> 16) & 255;
      sum[1] += (bits[i] >> 8) & 255;
      sum[2] += bits[i] & 255;
    }
    for (int c = 0; c < 3; c++) out[c] = sum[c] / (N * N);
  };
  auto settle = [&] {
    for (int i = 0; i < 4; i++) DwmFlush();
    if (live) desk.refresh();
  };

  // Color and coverage as written to the swap chain (premultiplied, 0 to 255).
  // wide: swap chain in HDR format (16-bit floating point, linear, 1.0 = 80 cd/m²)
  // instead of 8 bits.
  struct Try {
    float color, alpha;
    bool wide;
    const char* name;
  };
  const Try tries[] = {{8 / 255.0f, 0, false, "8 bit: color 8, alpha 0"},
                       {0, 15 / 255.0f, false, "8 bit: color 0, alpha 15"},
                       {15 / 255.0f, 15 / 255.0f, false, "8 bit: color 15, alpha 15"},
                       {0.05f, 0, true, "HDR: color 0.05, alpha 0"},
                       {0, 0.0588f, true, "HDR: color 0, alpha 0.0588"},
                       {0.05f, 0.0588f, true, "HDR: color 0.05, alpha 0.0588"},
                       {0.20f, 0.0588f, true, "HDR: color 0.20, alpha 0.0588"}};
  const POINT spots[] = {{m.rect.right - N - 60, m.rect.top + m.height() / 3}, {m.rect.left + 60, m.rect.top + 60}};
  for (POINT at : spots) {
    for (const Try& t : tries) {
      double a[3], b[3] = {}, c[3], la[3] = {}, lb[3] = {};
      settle();
      photo(at, a);
      if (live) desk.mean(at.x - m.rect.left, at.y - m.rect.top, N, la);
      HWND w = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"SunbeamProbe", L"", WS_POPUP, at.x, at.y, N, N, nullptr, nullptr, inst, nullptr);
      if (!w) continue;
      SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
      DXGI_SWAP_CHAIN_DESC1 sd = {};
      sd.Width = sd.Height = N;
      sd.Format = t.wide ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
      sd.SampleDesc.Count = 1;
      sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sd.BufferCount = 2;
      sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      ComPtr<IDXGISwapChain1> chain;
      ComPtr<IDXGISwapChain3> chain3;
      ComPtr<IDCompositionDevice> dcomp;
      ComPtr<IDCompositionTarget> target;
      ComPtr<IDCompositionVisual> visual;
      ComPtr<ID3D11Texture2D> back;
      ComPtr<ID3D11RenderTargetView> rtv;
      HRESULT hr = factory->CreateSwapChainForComposition(dev.Get(), &sd, nullptr, &chain);
      if (SUCCEEDED(hr) && t.wide) {
        hr = chain.As(&chain3);
        if (SUCCEEDED(hr)) hr = chain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
      }
      if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp));
      if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(w, TRUE, &target);
      if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(&visual);
      if (SUCCEEDED(hr)) hr = visual->SetContent(chain.Get());
      if (SUCCEEDED(hr)) hr = target->SetRoot(visual.Get());
      if (SUCCEEDED(hr)) hr = chain->GetBuffer(0, IID_PPV_ARGS(&back));
      if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);
      if (SUCCEEDED(hr)) {
        float fill[4] = {t.color, t.color, t.color, t.alpha};
        ctx->ClearRenderTargetView(rtv.Get(), fill);
        hr = chain->Present(0, 0);
        dcomp->Commit();
        ShowWindow(w, SW_SHOWNOACTIVATE);
        settle();
        photo(at, b);
        if (live) desk.mean(at.x - m.rect.left, at.y - m.rect.top, N, lb);
      }
      visual.Reset();
      target.Reset();
      dcomp.Reset();
      rtv.Reset();
      back.Reset();
      chain3.Reset();
      chain.Reset();
      DestroyWindow(w);
      settle();
      photo(at, c);
      if (FAILED(hr)) {
        fprintf(log, "Spot %5ld %5ld, %s: cannot create (%08lx)\n", at.x, at.y, t.name, hr);
        continue;
      }
      fprintf(log, "Spot %5ld %5ld, %-34s\n", at.x, at.y, t.name);
      fprintf(log, "    as a screenshot:      ground %6.2f %6.2f %6.2f, with patch %6.2f %6.2f %6.2f, afterwards %6.2f %6.2f %6.2f\n", a[0], a[1], a[2],
              b[0], b[1], b[2], c[0], c[1], c[2]);
      fprintf(log, "    sent to the display:  ground %7.4f %7.4f %7.4f, with patch %7.4f %7.4f %7.4f\n", la[0], la[1], la[2], lb[0], lb[1], lb[2]);
    }
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  fclose(log);
  return 0;
}

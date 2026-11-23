#include "render.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <iterator>

#include "sh/air.h"
#include "sh/base.h"
#include "sh/bloomdown.h"
#include "sh/bloompre.h"
#include "sh/bloomup.h"
#include "sh/bolt.h"
#include "sh/boltfar.h"
#include "sh/boltfarvs.h"
#include "sh/boltvs.h"
#include "sh/column.h"
#include "sh/columnblock.h"
#include "sh/composite.h"
#include "sh/dustps.h"
#include "sh/dustvs.h"
#include "sh/face.h"
#include "sh/flash.h"
#include "sh/full.h"
#include "sh/haze.h"
#include "sh/lamp.h"
#include "sh/near.h"
#include "sh/nearblock.h"
#include "sh/noise.h"
#include "sh/pick.h"
#include "sh/prop.h"
#include "sh/propin.h"
#include "sh/propvs.h"
#include "sh/rain.h"
#include "sh/scatter.h"
#include "sh/shade.h"

using Microsoft::WRL::ComPtr;

// The same order as cbuffer SkyC in shader\sky.hlsl.
struct SkyC {
  float res[2], view[2];
  float sun[2], sub[2];
  float sunR, focal, horizon, clouds;
  float night, day, cloudT, jit;
  float sunCol[3], exposure;
  float sunDir[3], show;
  float amb[3];
  int32_t cloudSteps;
  float span, count, share, back;
  float evolve, skyGain, time, stars;
  float sunAtm[3], srcMoon;
  float cloudCol[3], moonLit;
  float cirrusCol[3], moonLimb;
  float discCol[3], moonSpin;
  float moonCol[3], widen;
  float fill[3], cover;
  float moon[2], backOld, fade;
  float after[3], moonR;
  float shape[4];
  float ground[4];
  float strip[4];
  float sunLand[3], pass;
  float skyLand[3], cloudLift;
  float land[4];
  float sea[4];
  float storm[4];
  float flash[4];
  float flashCol[4];
  float flashMore[4];
  float wind[4];
  float snow[4];
  float fog[4];
  float fogCol[4];
};
static_assert(sizeof(SkyC) == 512);

const int WAVES = 160;   // as in shader\ground.hlsli
const int SWELL = 8;     // the last of them are swell

// The same order as cbuffer WaveC in shader\ground.hlsli.
struct WaveC {
  float wave[WAVES][4];
};

// The same order as cbuffer BoltC in shader\sky.hlsl.
const int BOLT_MAX = 768;
struct BoltC {
  float seg[BOLT_MAX][4];
  float par[BOLT_MAX][4];
};

// The same order as cbuffer PropC in shader\sky.hlsl.
struct PropC {
  float row[3][4];
  float rect[4];
  float depth[4];
  float work[4];
  float air[4];
  float light[4];
  float wing[4];
  float axis[4];
};

// A model as it is in the resources: this head, then the vertices (position,
// normal as three signed bytes and one that marks thin material, texture
// position, color factor with the layer of the material in a: 28 bytes), then
// the indices with 32 bits, then the lamps. Written by tools\models\convert.py.
struct MeshHead {
  char magic[4];   // SBM2
  uint32_t vertices, indices, layers;
  float lo[3], hi[3];
  float wing[3];
  uint32_t lamps;
};
const UINT MESH_VERTEX = 28;
static_assert(sizeof(MeshHead) == 56 && sizeof(Lamp) == 52);

// The same order as cbuffer LightC in shader\light.hlsl.
struct LightC {
  float res[2], view[2];
  float sun[2], lowRes[2];
  float sunR, zS, zMax, zFar;
  float g;
  int32_t n;
  float texel[2];
  float discCol[3], sigma;
  float sunCol[3], jitter;
  float alpha, mist;
  int32_t steps;
  float threshold;
  float mistOff[3], exposure;
  float mistScale[3], overWin;
  float bloom, rim, time, dpr;
  float srcTexel[2], weight, shade;
  float overSky, linear, white, nearFade;
  float cursor[4];
  float cursorPar[4];
  float moonPar[4];
  float glow[4];
  float rect[32][4];
  float par[32][4];
  float aux[32][4];
  float edge[32][4];
};
static_assert(sizeof(LightC) == 2304);

struct NoiseC {
  float slice, size, pad[2];
};

namespace {

const int MAXO = 32;
const int ACC_FRAMES = 12;
const int NOISE_N = 128;
const int SKY_ACC = 24;             // this many passes are averaged
const int CLOUD_STEPS = 160;
const float COVER_MAX = 0.64f;        // cover at 100 % clouds; the reference works with 0.3 to 0.4
// In a thunderstorm the cloud deck opens up a little again: dark cells with
// brighter gaps. Fully closed it would be a gray surface without shape from
// below.
const float COVER_STORM = 0.53f;
const double DUST_PER_MP = 880;     // dust particles per million logical pixels at full slider
const double CLOUD_TILE = 6400;     // meters per tile of the noise, as in sky.hlsl
const double WIND_MAX = 40;         // meters per second at full slider
const double CLOUD_WRAP = 625;      // after this many tiles of shift the clouds are the same again
// The wind at the ground blows rotated by 25 degrees against the drift of the
// clouds, towards the viewer, and weaker: the friction at the ground slows and
// turns it.
const double SURF_X = -0.747, SURF_Z = -0.665;   // its direction (right, ahead), as in sky.hlsl
const double SURF_MAX = 17;         // meters per second at the ground at full slider
const double RAIN_SPEED = 8;        // the drops fall this fast, meters per second
const double SNOW_SPEED = 1.4;      // and snowflakes this fast
const double FOAM_FULL = 0.028;     // fraction of the area under foam when at 17 m/s every wave breaks in every cell
const double EVOLVE_MAX = 0.0012;   // the shapes evolve this fast at full slider, in tiles per second
const double SKY_EVERY = 0.05;      // seconds between two cloud passes while the clouds drift
const double WORLD_EVERY = 1.0 / 62;   // everything is made anew this often; in between only the water, as often as the display shows a frame
const double AIR_GUARD = 96;         // the clouds extend this many logical pixels beyond the image on each side
const double AIR_PIXELS = 3.3e6;    // air and clouds get no more pixels than this, above that it is upscaled
const double SCATTER_PIXELS = 1.0e6;  // pixels of the scattered light at resolution 50 %
// The rearmost window is this deep in the haze, as a fraction of the haze depth.
// Behind it remains haze that no window shadows: it also glows around a window
// that covers the sun.
const float WINDOW_BACK = 0.72f;
// The position of a window arrives up to one frame later than Windows draws it.
// While a window moves, a margin around its edges therefore stays without light,
// as wide as the window travels in that time.
const double EDGE_LAG = 0.014;             // seconds
const float EDGE_MIN = 3, EDGE_MAX = 40;   // logical pixels
// When a window appears or disappears, Windows plays a short animation. For that
// long its area stays without light, then it fades in.
const double APPEAR_HOLD = 0.10, APPEAR_DONE = 0.35, GHOST_HOLD = 0.12, GHOST_LIFE = 0.30;
// The clouds of one pass are spread over several frames, band by band from
// bottom to top. The near clouds are at the top of the image, where the
// computation is most expensive, and just above the horizon the path through the
// layer is longest: there the bands are narrow. The numbers are the boundaries
// as a fraction of the image height, from the bottom; measured, every band costs
// about the same.
const float AIR_CUTS[] = {0, 0.21f, 0.234f, 0.266f, 0.31f, 0.363f, 0.458f, 0.585f, 0.685f, 0.756f, 0.807f, 0.858f, 0.894f, 0.929f, 0.965f, 1};
const int AIR_BANDS = int(std::size(AIR_CUTS)) - 1;

// A sun at the bottom edge of the image is this far below the horizon: deep
// night.
const double NIGHT_DEPTH = 18;
const double CLOUD_LEVEL = 2600, CIRRUS_LEVEL = 9000;   // altitude of the cloud layers in meters, as in sky.hlsl

// What moves through the image, in the order of life.h. afloat: it lies in the
// water, what is below the water line is cut off, and the clouds shade it like
// the water around it.
struct Model {
  const wchar_t* mesh;
  const wchar_t* colors;
  bool afloat;
  float gloss;   // how strongly its surface shows the light source as a highlight
};
const Model MODELS[MODEL_COUNT] = {
    {L"MODEL_CONTAINER", L"MODEL_CONTAINER_COLORS", true, 0.12f}, {L"MODEL_FREIGHTER", L"MODEL_FREIGHTER_COLORS", true, 0.12f},
    {L"MODEL_DINGHY", L"MODEL_DINGHY_COLORS", true, 0.08f},       {L"MODEL_AIRLINER", L"MODEL_AIRLINER_COLORS", false, 0.5f},
    {L"MODEL_GULL", L"MODEL_GULL_COLORS", false, 0.0f},
};
// The work image for one model. If the model is smaller in the image, several of
// its pixels fall on one pixel of the image, up to PROP_FINE per axis.
const int PROP_W = 768, PROP_H = 512, PROP_FINE = 4;
// A lamp of strength 1 at one kilometer is this bright in its center, as a
// multiple of white before exposure; LAMP_PEAK is the most a disc holds.
const double LAMP_GAIN = 300, LAMP_PEAK = 6;
const double R_EFF = 7433e3;   // Earth radius, enlarged to account for refraction, as in ground.hlsli

const double R_PLANET = 6371e3;
const double R_ATMOS = 6471e3;
const double K_RLH[3] = {5.5e-6, 13.0e-6, 22.4e-6};
const double K_MIE = 10e-6;
const double SH_RLH = 8e3;
const double SH_MIE = 1.2e3;
const double G_MIE = 0.5;
const double SUN_E = 22;
const double PI = 3.14159265358979;

double smooth(double a, double b, double x) {
  double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Path to the edge of the atmosphere.
double reach(const double* o, const double* d) {
  double b = dot3(o, d);
  return -b + std::sqrt(std::max(b * b - dot3(o, o) + R_ATMOS * R_ATMOS, 0.0));
}

// Luminance of the sky in direction r; s is the direction to the light source in
// the image, sa its true one. The same computation as atmosphere() in sky.hlsl,
// without the multiply scattered light.
void skyRadiance(const double* r, const double* s, const double* sa, double* out) {
  const int I = 16, J = 8;
  const double o[3] = {0, R_PLANET + 600, 0};
  double ds = reach(o, r) / I;
  double mu = dot3(r, s), gg = G_MIE * G_MIE;
  double pR = 3 / (16 * PI) * (1 + mu * mu);
  double pM = 3 / (8 * PI) * ((1 - gg) * (1 + mu * mu)) / (std::pow(1 + gg - 2 * mu * G_MIE, 1.5) * (2 + gg));
  double odR = 0, odM = 0, sumR[3] = {}, sumM[3] = {};
  for (int i = 0; i < I; i++) {
    double t = ds * (i + 0.5);
    double pos[3] = {o[0] + r[0] * t, o[1] + r[1] * t, o[2] + r[2] * t};
    double h = std::sqrt(dot3(pos, pos)) - R_PLANET;
    double dR = std::exp(-h / SH_RLH) * ds, dM = std::exp(-h / SH_MIE) * ds;
    odR += dR;
    odM += dM;
    double dj = reach(pos, sa) / J, jR = 0, jM = 0;
    for (int j = 0; j < J; j++) {
      double u = dj * (j + 0.5);
      double q[3] = {pos[0] + sa[0] * u, pos[1] + sa[1] * u, pos[2] + sa[2] * u};
      double hj = std::sqrt(dot3(q, q)) - R_PLANET;
      jR += std::exp(std::min(-hj / SH_RLH, 20.0)) * dj;
      jM += std::exp(std::min(-hj / SH_MIE, 20.0)) * dj;
    }
    for (int c = 0; c < 3; c++) {
      double att = std::exp(-(K_MIE * (odM + jM) + K_RLH[c] * (odR + jR)));
      sumR[c] += dR * att;
      sumM[c] += dM * att;
    }
  }
  for (int c = 0; c < 3; c++) out[c] = SUN_E * (pR * K_RLH[c] * sumR[c] + pM * K_MIE * sumM[c]);
}

// How much light of the light source arrives at height h above the ground, per
// color. If the path grazes the earth, only a remainder of deep red light is
// left; if it dips below the horizon, nothing. That is why clouds keep glowing
// for a while after sunset, the high ones longest.
void sunlightAt(double h, const double* dir, double* out) {
  double oy = R_PLANET + h;
  double b = oy * dir[1];
  double tMax = -b + std::sqrt(std::max(b * b - oy * oy + R_ATMOS * R_ATMOS, 0.0));
  const int N = 48;
  double ds = tMax / N, odR = 0, odM = 0;
  for (int i = 0; i < N; i++) {
    double t = ds * (i + 0.5);
    double q[3] = {dir[0] * t, oy + dir[1] * t, dir[2] * t};
    double hq = std::sqrt(dot3(q, q)) - R_PLANET;
    odR += std::exp(std::min(-hq / SH_RLH, 20.0)) * ds;
    odM += std::exp(std::min(-hq / SH_MIE, 20.0)) * ds;
  }
  // Lowest point of the path: below the ground it ends.
  double low = dir[1] < 0 ? oy * std::sqrt(std::max(1 - dir[1] * dir[1], 0.0)) - R_PLANET : h;
  double open = smooth(-400, 200, low);
  for (int c = 0; c < 3; c++) out[c] = std::exp(-(K_RLH[c] * odR + K_MIE * odM)) * open;
}

// Direction with the azimuth of d and the altitude alt in degrees.
void raise(const double* d, double alt, double* out) {
  double flat = std::hypot(d[0], d[2]);
  double c = std::cos(alt * PI / 180), sn = std::sin(alt * PI / 180);
  out[0] = flat > 1e-9 ? d[0] / flat * c : 0;
  out[1] = sn;
  out[2] = flat > 1e-9 ? d[2] / flat * c : c;
}

// The grounds to choose from. The land is in the program as a strip (resources
// from ground\, computed with tools\ground); without a strip there is only
// water.
struct Ground {
  const wchar_t* strip[4];   // color, direction and distance, horizon, lights; nullptr: missing
  bool water;
  float eyeWater;            // eye above the water, meters
  float eyeSea;              // eye above the sea, meters
  float sea[3];              // color of the light that comes back out of the water
  float rough[3];            // sea state: spread of the slope in calm, at half and at full slider
  float peak[3];             // length of the largest wind waves for each, meters
  float swell;               // steepness of the swell: long waves from far away that the wind does not change
  float shelter;             // fraction of the wind that reaches the water (for the whitecaps)
  float cloudLift;           // the clouds are this much higher above the viewer, meters
  float fogTop[2];           // fog reaches up to this height above the sea: thin fog, dense fog
};
const Ground GROUNDS[] = {
    // Over the mountains the cloud base is above the highest peaks in the image;
    // otherwise all of them would be in clouds. Fog fills the valleys far below the
    // viewer: a sea of fog.
    {{L"MOUNTAINS_A", L"MOUNTAINS_B", L"MOUNTAINS_C", nullptr}, false, 0, 2365, {0, 0, 0}, {1, 1, 1}, {1, 1, 1}, 0, 0, 550, {1250, 1950}},
    // In front of the city the water is sheltered: almost a mirror at half wind,
    // choppy only in strong wind. Fog lies on the lake; thin fog leaves the towers
    // standing above it.
    {{L"CITY_A", L"CITY_B", L"CITY_C", L"CITY_D"}, true, 9, 186, {0.006f, 0.018f, 0.024f}, {0.000003f, 0.001f, 0.006f}, {0.2f, 20, 32}, 0.0006f, 0.4f, 0,
     {177 + 30, 177 + 420}},
    {{nullptr, nullptr, nullptr, nullptr}, true, 26, 26, {0.004f, 0.016f, 0.045f}, {0.00005f, 0.014f, 0.07f}, {0.25f, 60, 110}, 0.006f, 1, 0, {40, 420}},
};
const int GROUND_COUNT = int(std::size(GROUNDS));
// The strip: this much tangent is above the horizon, this much it measures in
// total (tools\ground\panorama.py).
const float STRIP_TOP = 512.2f / 4224, STRIP_SPAN = 1088.0f / 4224;

// The weather for the cloud slider. Up to 100 % the clouds increase; above that
// the cloud deck closes, it becomes thick and dark, it starts to rain, and
// finally there is lightning.
struct Sky {
  float cover;     // cloud cover
  float closed;    // how closed the sky is, 0 to 1: then the light at the ground is gray instead of blue
  float dark;      // thunderstorm darkness of the clouds, 0 to 1
  float rain;      // rain, 0 to 1
  float thunder;   // thunderstorm, 0 to 1
};

Sky skyOf(float clouds) {
  Sky s;
  s.cover = COVER_MAX * std::min(clouds, 1.0f) - (COVER_MAX - COVER_STORM) * float(smooth(1.0, 1.45, clouds));
  s.closed = float(smooth(0.38, 0.6, s.cover));
  s.dark = float(smooth(1.05, 1.7, clouds));
  s.rain = float(smooth(1.22, 1.75, clouds));
  s.thunder = float(smooth(1.5, 1.85, clouds));
  return s;
}

int groundOf(const Params& p) { return std::clamp(int(std::lround(p.ground)), 0, GROUND_COUNT - 1); }

// What follows from the temperature. Between freezing and two degrees above it,
// rain turns into snow. Snow lies from the height at which the mean of the day
// is at freezing; the air cools by 6.5 degrees per kilometer.
struct Cold {
  float snow;    // fraction of what falls that is snow
  float line;    // snow lies above this height above the sea, meters
  float cover;   // snow on the ground at the viewer's level, 0 to 1
};

Cold coldOf(const Params& p) {
  Cold c;
  c.snow = float(1 - smooth(0.3, 1.8, p.temperature));
  c.line = GROUNDS[groundOf(p)].eyeSea + p.dayTemperature / 0.0065f;
  c.cover = p.snowDepth >= 0 ? float(smooth(0.004, 0.03, p.snowDepth)) : float(1 - smooth(-1.5, 1.0, p.dayTemperature));
  return c;
}

// The fog for the slider: a layer over the ground whose visibility falls from
// 10 km to 150 m (extinction after Koschmieder, as in weather.cpp; the ordinary
// air at 10 km is left to the haze) and whose top rises as it thickens. Thin fog
// lies in banks, dense fog is closed.
struct Fog {
  float ext;     // extinction per meter inside the layer
  float top;     // its top above the sea, meters
  float soft;    // depth of the transition at the top
  float banks;   // how patchy it is
};

Fog fogOf(const Params& p) {
  const Ground& g = GROUNDS[groundOf(p)];
  double f = std::clamp(double(p.fog), 0.0, 1.0);
  Fog o;
  o.ext = f <= 0.001 ? 0.0f : float(3.912 * (std::pow(10000.0 / 150.0, std::pow(f, 1.5)) - 1) / 10000.0);
  o.top = float(g.fogTop[0] + (g.fogTop[1] - g.fogTop[0]) * f * f);
  o.soft = float(0.3 * (o.top - (g.water ? g.eyeSea - g.eyeWater : g.fogTop[0] - 250)));
  o.soft = std::clamp(o.soft, 8.0f, 90.0f);
  o.banks = float(1 - smooth(0.35, 0.9, f));
  return o;
}

bool resource(const wchar_t* name, const uint8_t*& data, size_t& size) {
  HRSRC found = FindResourceW(nullptr, name, RT_RCDATA);
  HGLOBAL held = found ? LoadResource(nullptr, found) : nullptr;
  data = held ? static_cast<const uint8_t*>(LockResource(held)) : nullptr;
  size = found ? SizeofResource(nullptr, found) : 0;
  return data != nullptr;
}

// The pixels of an image from a PNG resource: RGBA, row by row from the top.
// deep: 16 bits per channel.
bool pngPixels(const wchar_t* name, bool deep, UINT& w, UINT& h, std::vector<uint8_t>& px) {
  const uint8_t* data;
  size_t size;
  if (!resource(name, data, size)) return false;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> rgba;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
  HRESULT hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size));
  if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
  if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
  if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&rgba);
  if (SUCCEEDED(hr)) {
    hr = rgba->Initialize(frame.Get(), deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                          WICBitmapPaletteTypeCustom);
  }
  w = h = 0;
  if (SUCCEEDED(hr)) hr = rgba->GetSize(&w, &h);
  if (FAILED(hr) || !w || !h) return false;
  const UINT bytes = deep ? 8 : 4;
  px.resize(size_t(w) * h * bytes);
  return SUCCEEDED(rgba->CopyPixels(nullptr, w * bytes, UINT(px.size()), px.data()));
}

// An image from a PNG resource as a texture with all mip levels.
// deep: 16 bits per channel; colour: color values that become linear when read.
bool pngTexture(ID3D11Device* dev, const wchar_t* name, bool deep, bool colour, ComPtr<ID3D11ShaderResourceView>& out) {
  UINT w = 0, h = 0;
  const UINT bytes = deep ? 8 : 4;
  std::vector<std::vector<uint8_t>> levels(1);
  if (!pngPixels(name, deep, w, h, levels[0])) return false;
  std::vector<D3D11_SUBRESOURCE_DATA> init = {{nullptr, w * bytes, 0}};
  for (UINT lw = w, lh = h; lw > 1 && lh > 1;) {
    UINT nw = lw / 2, nh = lh / 2;
    std::vector<uint8_t> next(size_t(nw) * nh * bytes);
    const std::vector<uint8_t>& from = levels.back();
    if (deep) {
      const uint16_t* in = reinterpret_cast<const uint16_t*>(from.data());
      uint16_t* to = reinterpret_cast<uint16_t*>(next.data());
      for (UINT y = 0; y < nh; y++) {
        for (UINT x = 0; x < nw; x++) {
          for (int c = 0; c < 4; c++) {
            const uint16_t* p = &in[(size_t(y) * 2 * lw + x * 2) * 4 + c];
            to[(size_t(y) * nw + x) * 4 + c] = uint16_t((uint32_t(p[0]) + p[4] + p[size_t(lw) * 4] + p[size_t(lw) * 4 + 4] + 2) / 4);
          }
        }
      }
    } else {
      for (UINT y = 0; y < nh; y++) {
        for (UINT x = 0; x < nw; x++) {
          for (int c = 0; c < 4; c++) {
            const uint8_t* p = &from[(size_t(y) * 2 * lw + x * 2) * 4 + c];
            next[(size_t(y) * nw + x) * 4 + c] = uint8_t((p[0] + p[4] + p[size_t(lw) * 4] + p[size_t(lw) * 4 + 4] + 2) / 4);
          }
        }
      }
    }
    levels.push_back(std::move(next));
    init.push_back({nullptr, nw * bytes, 0});
    lw = nw, lh = nh;
  }
  for (size_t i = 0; i < levels.size(); i++) init[i].pSysMem = levels[i].data();
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = UINT(levels.size());
  d.ArraySize = 1;
  d.Format = deep ? DXGI_FORMAT_R16G16B16A16_UNORM : colour ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> tex;
  return SUCCEEDED(dev->CreateTexture2D(&d, init.data(), &tex)) && SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &out));
}

}  // namespace

bool Renderer::init(ID3D11Device* device, ID3D11DeviceContext* context) {
  dev_ = device;
  ctx_ = context;
  HRESULT hr = S_OK;
  hr |= dev_->CreateVertexShader(sh_full, sizeof(sh_full), nullptr, &vsFull_);
  hr |= dev_->CreateVertexShader(sh_dustvs, sizeof(sh_dustvs), nullptr, &vsDust_);
  hr |= dev_->CreateVertexShader(sh_boltvs, sizeof(sh_boltvs), nullptr, &vsBolt_);
  hr |= dev_->CreateVertexShader(sh_boltfarvs, sizeof(sh_boltfarvs), nullptr, &vsBoltFar_);
  hr |= dev_->CreateVertexShader(sh_propvs, sizeof(sh_propvs), nullptr, &vsProp_);
  const D3D11_INPUT_ELEMENT_DESC vertex[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_SNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  hr |= dev_->CreateInputLayout(vertex, UINT(std::size(vertex)), sh_propvs, sizeof(sh_propvs), &propLayout_);
  auto pixel = [&](const BYTE* code, size_t size, ComPtr<ID3D11PixelShader>& out) {
    hr |= dev_->CreatePixelShader(code, size, nullptr, &out);
  };
  pixel(sh_noise, sizeof(sh_noise), psNoise_);
  pixel(sh_air, sizeof(sh_air), psAir_);
  pixel(sh_base, sizeof(sh_base), psBase_);
  pixel(sh_haze, sizeof(sh_haze), psHaze_);
  pixel(sh_face, sizeof(sh_face), psFace_);
  pixel(sh_pick, sizeof(sh_pick), psPick_);
  pixel(sh_rain, sizeof(sh_rain), psRain_);
  pixel(sh_shade, sizeof(sh_shade), psShade_);
  pixel(sh_column, sizeof(sh_column), psColumn_);
  pixel(sh_columnblock, sizeof(sh_columnblock), psColumnBlock_);
  pixel(sh_bolt, sizeof(sh_bolt), psBolt_);
  pixel(sh_boltfar, sizeof(sh_boltfar), psBoltFar_);
  pixel(sh_near, sizeof(sh_near), psNear_);
  pixel(sh_nearblock, sizeof(sh_nearblock), psNearBlock_);
  pixel(sh_scatter, sizeof(sh_scatter), psScatter_);
  pixel(sh_bloompre, sizeof(sh_bloompre), psBloomPre_);
  pixel(sh_bloomdown, sizeof(sh_bloomdown), psBloomDown_);
  pixel(sh_bloomup, sizeof(sh_bloomup), psBloomUp_);
  pixel(sh_composite, sizeof(sh_composite), psComposite_);
  pixel(sh_flash, sizeof(sh_flash), psFlash_);
  pixel(sh_dustps, sizeof(sh_dustps), psDust_);
  pixel(sh_prop, sizeof(sh_prop), psProp_);
  pixel(sh_propin, sizeof(sh_propin), psPropIn_);
  pixel(sh_lamp, sizeof(sh_lamp), psLamp_);
  if (FAILED(hr)) return false;

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  dev_->CreateSamplerState(&sd, &clamp_);
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  dev_->CreateSamplerState(&sd, &wrap_);
  // For reflections in the water: the area that a pixel reflects is long and
  // narrow.
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.MaxAnisotropy = 16;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  dev_->CreateSamplerState(&sd, &wide_);

  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev_->CreateBlendState(&bd, &blendOff_);
  auto& rt = bd.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  // Fraction of the new image via the blend factor: this is how the passes are
  // averaged.
  rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
  rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
  dev_->CreateBlendState(&bd, &blendShare_);
  rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
  rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE;
  dev_->CreateBlendState(&bd, &blendAdd_);
  // The brighter of the two: for the segments of a lightning channel that overlap.
  rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
  dev_->CreateBlendState(&bd, &blendMax_);
  // Laid over the target by its coverage.
  rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
  rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  dev_->CreateBlendState(&bd, &blendOver_);
  rt.DestBlendAlpha = D3D11_BLEND_ONE;
  dev_->CreateBlendState(&bd, &blendOverKeep_);

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  dev_->CreateRasterizerState(&rd, &raster_);
  rd.ScissorEnable = TRUE;
  dev_->CreateRasterizerState(&rd, &rasterCut_);

  auto buffer = [&](UINT size, ComPtr<ID3D11Buffer>& out) {
    D3D11_BUFFER_DESC b = {};
    b.ByteWidth = size;
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev_->CreateBuffer(&b, nullptr, &out);
  };
  buffer(sizeof(SkyC), skyBuf_);
  buffer(sizeof(LightC), lightBuf_);
  buffer(sizeof(NoiseC), noiseBuf_);
  buffer(sizeof(WaveC), waveBuf_);
  buffer(sizeof(BoltC), boltBuf_);
  buffer(sizeof(PropC), propBuf_);

  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  dev_->CreateQuery(&qd, &event_);

  // A model that is missing or damaged simply never appears.
  for (int i = 0; i < MODEL_COUNT; i++) {
    if (!loadMesh(i)) mesh_[i] = Mesh();
  }
  if (!loadClouds()) return false;
  bindCommon();
  makeNoise();
  haze_ = makeTarget(256, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
  uint8_t none = 0;
  setIconMask(&none, 1, 1);
  return true;
}

Renderer::Target Renderer::makeTarget(int w, int h, DXGI_FORMAT format, bool mips) {
  Target t;
  t.w = w;
  t.h = h;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = mips ? 0 : 1;
  d.ArraySize = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
  dev_->CreateTexture2D(&d, nullptr, &t.tex);
  dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
  dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
  return t;
}

void Renderer::target(ID3D11RenderTargetView* rtv, int w, int h) {
  // Unbind all textures first: what is about to become a target must no longer be
  // bound as a source.
  ID3D11ShaderResourceView* none[13] = {};
  ctx_->PSSetShaderResources(0, 13, none);
  ctx_->VSSetShaderResources(0, 6, none);
  ctx_->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp = {0, 0, float(w), float(h), 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ctx_->RSSetState(raster_.Get());
  ctx_->OMSetBlendState(blendOff_.Get(), nullptr, 0xffffffff);
}

void Renderer::quad(ID3D11PixelShader* ps) {
  ctx_->VSSetShader(vsFull_.Get(), nullptr, 0);
  ctx_->PSSetShader(ps, nullptr, 0);
  ctx_->Draw(3, 0);
}

void Renderer::sync() {
  ctx_->End(event_.Get());
  BOOL done = FALSE;
  while (ctx_->GetData(event_.Get(), &done, sizeof(done), 0) != S_OK) {
  }
}

// Tileable noise in space, generated slice by slice on the graphics card.
void Renderer::makeNoise() {
  D3D11_TEXTURE3D_DESC d = {};
  d.Width = d.Height = d.Depth = NOISE_N;
  d.MipLevels = 0;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  dev_->CreateTexture3D(&d, nullptr, &noiseTex_);
  dev_->CreateShaderResourceView(noiseTex_.Get(), nullptr, &noise_);

  ctx_->PSSetConstantBuffers(0, 1, noiseBuf_.GetAddressOf());
  for (int z = 0; z < NOISE_N; z++) {
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = d.Format;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
    rv.Texture3D.FirstWSlice = z;
    rv.Texture3D.WSize = 1;
    ComPtr<ID3D11RenderTargetView> slice;
    dev_->CreateRenderTargetView(noiseTex_.Get(), &rv, &slice);
    NoiseC c = {(z + 0.5f) / NOISE_N, float(NOISE_N)};
    ctx_->UpdateSubresource(noiseBuf_.Get(), 0, nullptr, &c, 0, 0);
    target(slice.Get(), NOISE_N, NOISE_N);
    quad(psNoise_.Get());
  }
  target(nullptr, 1, 1);
  ctx_->GenerateMips(noise_.Get());
}

// The textures of the clouds are in the program (resources from clouds\).
bool Renderer::loadClouds() {
  auto bytes = [](const wchar_t* name, const uint8_t*& data, size_t& size) {
    HRSRC found = FindResourceW(nullptr, name, RT_RCDATA);
    HGLOBAL held = found ? LoadResource(nullptr, found) : nullptr;
    data = held ? static_cast<const uint8_t*>(LockResource(held)) : nullptr;
    size = found ? SizeofResource(nullptr, found) : 0;
    return data != nullptr;
  };
  auto volume = [&](const wchar_t* name, int n, ComPtr<ID3D11ShaderResourceView>& out) {
    const uint8_t* data;
    size_t size;
    if (!bytes(name, data, size) || size != size_t(n) * n * n) return false;
    // With all mip levels: for the shadow of distant clouds the shape is read
    // averaged over larger volumes.
    std::vector<std::vector<uint8_t>> levels;
    levels.emplace_back(data, data + size);
    std::vector<D3D11_SUBRESOURCE_DATA> init = {{nullptr, UINT(n), UINT(n * n)}};
    for (int m = n; m > 1; m /= 2) {
      int k = m / 2;
      const std::vector<uint8_t>& from = levels.back();
      std::vector<uint8_t> next(size_t(k) * k * k);
      for (int z = 0; z < k; z++) {
        for (int y = 0; y < k; y++) {
          for (int x = 0; x < k; x++) {
            int sum = 0;
            for (int i = 0; i < 8; i++) sum += from[(size_t(2 * z + (i >> 2)) * m + 2 * y + ((i >> 1) & 1)) * m + 2 * x + (i & 1)];
            next[(size_t(z) * k + y) * k + x] = uint8_t((sum + 4) / 8);
          }
        }
      }
      levels.push_back(std::move(next));
      init.push_back({nullptr, UINT(k), UINT(k * k)});
    }
    for (size_t i = 0; i < levels.size(); i++) init[i].pSysMem = levels[i].data();
    D3D11_TEXTURE3D_DESC d = {};
    d.Width = d.Height = d.Depth = n;
    d.MipLevels = UINT(levels.size());
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture3D> tex;
    return SUCCEEDED(dev_->CreateTexture3D(&d, init.data(), &tex)) && SUCCEEDED(dev_->CreateShaderResourceView(tex.Get(), nullptr, &out));
  };
  // An image from a PNG resource, with all mip levels: in the distance the weather
  // map is read averaged over larger areas.
  auto image = [&](const wchar_t* name, ComPtr<ID3D11ShaderResourceView>& out) {
    const uint8_t* data;
    size_t size;
    if (!bytes(name, data, size)) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> rgba;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&rgba);
    if (SUCCEEDED(hr)) hr = rgba->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = rgba->GetSize(&w, &h);
    if (FAILED(hr) || !w || !h) return false;
    std::vector<std::vector<uint8_t>> levels;
    levels.emplace_back(size_t(w) * h * 4);
    if (FAILED(rgba->CopyPixels(nullptr, w * 4, UINT(levels[0].size()), levels[0].data()))) return false;
    std::vector<D3D11_SUBRESOURCE_DATA> init = {{levels[0].data(), w * 4, 0}};
    for (UINT lw = w, lh = h; lw > 1 && lh > 1;) {
      UINT nw = lw / 2, nh = lh / 2;
      std::vector<uint8_t> next(size_t(nw) * nh * 4);
      const std::vector<uint8_t>& from = levels.back();
      for (UINT y = 0; y < nh; y++) {
        for (UINT x = 0; x < nw; x++) {
          for (int c = 0; c < 4; c++) {
            const uint8_t* p = &from[(size_t(y) * 2 * lw + x * 2) * 4 + c];
            next[(size_t(y) * nw + x) * 4 + c] = uint8_t((p[0] + p[4] + p[size_t(lw) * 4] + p[size_t(lw) * 4 + 4] + 2) / 4);
          }
        }
      }
      levels.push_back(std::move(next));
      init.push_back({nullptr, nw * 4, 0});
      lw = nw, lh = nh;
    }
    for (size_t i = 0; i < levels.size(); i++) init[i].pSysMem = levels[i].data();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = UINT(levels.size());
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    return SUCCEEDED(dev_->CreateTexture2D(&d, init.data(), &tex)) && SUCCEEDED(dev_->CreateShaderResourceView(tex.Get(), nullptr, &out));
  };
  // Slope of the moon surface: two bytes per point, 720 by 360, with all mip
  // levels.
  auto slopes = [&](const wchar_t* name, int w, int h, ComPtr<ID3D11ShaderResourceView>& out) {
    const uint8_t* data;
    size_t size;
    if (!bytes(name, data, size) || size != size_t(w) * h * 2) return false;
    std::vector<std::vector<uint8_t>> levels;
    levels.emplace_back(data, data + size);
    std::vector<D3D11_SUBRESOURCE_DATA> init = {{nullptr, UINT(w * 2), 0}};
    for (int lw = w, lh = h; lw > 1 && lh > 1;) {
      int nw = lw / 2, nh = lh / 2;
      const std::vector<uint8_t>& from = levels.back();
      std::vector<uint8_t> next(size_t(nw) * nh * 2);
      for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
          for (int c = 0; c < 2; c++) {
            const uint8_t* p = &from[(size_t(y) * 2 * lw + x * 2) * 2 + c];
            next[(size_t(y) * nw + x) * 2 + c] = uint8_t((p[0] + p[2] + p[size_t(lw) * 2] + p[size_t(lw) * 2 + 2] + 2) / 4);
          }
        }
      }
      levels.push_back(std::move(next));
      init.push_back({nullptr, UINT(nw * 2), 0});
      lw = nw, lh = nh;
    }
    for (size_t i = 0; i < levels.size(); i++) init[i].pSysMem = levels[i].data();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = UINT(levels.size());
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    return SUCCEEDED(dev_->CreateTexture2D(&d, init.data(), &tex)) && SUCCEEDED(dev_->CreateShaderResourceView(tex.Get(), nullptr, &out));
  };
  // The weather map in memory as well: from it follows whether it rains at the
  // viewer's location and where lightning strikes.
  {
    const uint8_t* data;
    size_t size;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> rgba;
    UINT w = 0, h = 0;
    if (bytes(L"WEATHER_MAP", data, size) && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&rgba)) &&
        SUCCEEDED(rgba->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(rgba->GetSize(&w, &h)) && w && h) {
      weatherMap_.resize(size_t(w) * h * 4);
      if (SUCCEEDED(rgba->CopyPixels(nullptr, w * 4, UINT(weatherMap_.size()), weatherMap_.data()))) weatherW_ = int(w), weatherH_ = int(h);
    }
  }
  return volume(L"CLOUD_SHAPE", 128, shape_) && volume(L"CLOUD_DETAIL", 32, detail_) && image(L"WEATHER_MAP", weather_) &&
         image(L"TURBULENCE", turb_) && image(L"MOON_MAP", moon_) && slopes(L"MOON_RELIEF", 720, 360, relief_);
}

// What all drawing steps need. Set anew before every frame: if the state of the
// graphics card is cleared in between (on rebuild after a change of the
// display), all of it would be missing otherwise.
void Renderer::bindCommon() {
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->IASetInputLayout(nullptr);
  ID3D11SamplerState* samplers[3] = {clamp_.Get(), wrap_.Get(), wide_.Get()};
  ctx_->PSSetSamplers(0, 3, samplers);
  ctx_->VSSetSamplers(0, 2, samplers);
  ID3D11ShaderResourceView* moon[2] = {moon_.Get(), relief_.Get()};
  ctx_->PSSetShaderResources(13, 2, moon);
}

void Renderer::allocScatter() {
  // The scattered light is soft. At resolution 50 % it gets at most about one
  // million pixels, on small surfaces half the edge length.
  double k = par_.resolution * std::min(1.0, 2 * std::sqrt(SCATTER_PIXELS / (double(pixW_) * pixH_)));
  int w = std::max(1, int(std::lround(pixW_ * k))), h = std::max(1, int(std::lround(pixH_ * k)));
  for (auto& t : scat_) t = makeTarget(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
  for (auto& t : scatFlash_) t = makeTarget(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
  sceneDirty_ = true;
}

void Renderer::resize(int pixelW, int pixelH, float scale) {
  pixW_ = std::max(pixelW, 1);
  pixH_ = std::max(pixelH, 1);
  scale_ = scale;
  viewW_ = pixW_ / scale;
  viewH_ = pixH_ / scale;
  double k = std::min(1.0, std::sqrt(AIR_PIXELS / (double(pixW_) * pixH_)));
  // The clouds extend beyond the image on both sides (GUARD in sky.hlsl).
  int aw = std::max(2, int(std::lround((pixW_ + 2 * AIR_GUARD * scale_) * k))), ah = std::max(2, int(std::lround(pixH_ * k)));
  // The air without clouds changes only slowly across the image.
  base_ = makeTarget(std::max(2, aw / 3), std::max(2, ah / 3), DXGI_FORMAT_R16G16B16A16_FLOAT, false);
  for (int i = 0; i < 3; i++) {
    air_[i] = makeTarget(aw, ah, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    airZ_[i] = makeTarget(aw, ah, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    near_[i] = makeTarget((aw + 3) / 4, (ah + 3) / 4, DXGI_FORMAT_R16_FLOAT, false);
    airShiftOf_[i] = cloudT_;
  }
  nearBlock_ = makeTarget((aw + 3) / 4, (ah + 3) / 4, DXGI_FORMAT_R16_FLOAT, false);
  airNew_ = 0, airOld_ = 2, airWork_ = 1;
  airDone_ = 0;
  int hw = std::max(2, pixW_ / 2), hh = std::max(2, pixH_ / 2);
  sky_ = makeTarget(hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
  upper_ = makeTarget(hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
  // The strip of the mirror image above the horizon, summed column by column: from
  // the top edge of the land to the horizon. Sums over hundreds of rows need the
  // full precision.
  columnTop_ = std::max(0, int(std::floor((horizon() - focal() * STRIP_TOP) / viewH_ * hh)));
  columnH_ = std::clamp(int(std::ceil(horizon() / viewH_ * hh)) - columnTop_, 1, hh - columnTop_);
  column_ = makeTarget(hw, columnH_, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
  columnBlock_ = makeTarget(hw, columnH_, DXGI_FORMAT_R32G32B32A32_FLOAT, false);
  mapTick_ = 0;
  shade_ = makeTarget(std::max(2, pixW_ / 4), std::max(2, pixH_ / 4), DXGI_FORMAT_R16_FLOAT, false);
  // The rain is soft: a quarter of the edge length is enough.
  for (auto& t : rain_) {
    t = makeTarget(std::max(2, pixW_ / 4), std::max(2, pixH_ / 4), DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    const float none[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(t.rtv.Get(), none);
  }
  rainCur_ = 0;
  bolt_ = makeTarget(pixW_, pixH_, DXGI_FORMAT_R16G16_FLOAT, false);
  boltFar_ = makeTarget(std::max(1, pixW_ / 4), std::max(1, pixH_ / 4), DXGI_FORMAT_R16G16_FLOAT, false);
  // The glare should reach equally far on every display. The finest level that
  // enters the image is therefore always about 240 pixels wide.
  bloomBase_ = std::clamp(int(std::lround(std::log2(hw / 240.0))), 1, 4);
  bloom_.clear();
  for (int i = 0, w = hw, h = hh; i < bloomBase_ + 5 && w >= 2 && h >= 2; i++, w /= 2, h /= 2) {
    bloom_.push_back(makeTarget(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false));
  }
  allocScatter();
  sizeBodies();
  updateLight();
  updateDepths();
  passActive_ = false;
  skyDirty_ = true;
}

// Projection of the sky: pinhole camera whose horizon is at 82 % of the height.
float Renderer::focal() const { return 1.1f * std::max(viewW_, viewH_ * 1.25f); }
float Renderer::horizon() const { return 0.82f * viewH_; }

// True altitude for an image row. Above the horizon it grows faster and faster
// towards the top edge up to the zenith, below it down to deep night at the
// bottom edge. At the horizon itself image and truth agree: a sunset behind the
// mountains is exactly what it shows.
double Renderer::altitudeOfRow(float y) const {
  double e = std::atan2(double(horizon()) - y, double(focal())) * 180 / PI;
  double top = std::atan2(double(horizon()), double(focal())) * 180 / PI;
  double bottom = std::atan2(double(viewH_ - horizon()), double(focal())) * 180 / PI;
  if (e >= 0) {
    double u = e / top;
    return std::min(e + (90 - top) * u * u, 90.0);
  }
  double v = -e / bottom;
  return std::max(e - (NIGHT_DEPTH - bottom) * v * v, -NIGHT_DEPTH);
}

float Renderer::rowOfAltitude(double altitude) const {
  double top = std::atan2(double(horizon()), double(focal())) * 180 / PI;
  double bottom = std::atan2(double(viewH_ - horizon()), double(focal())) * 180 / PI;
  if (altitude >= 0) {
    double a = 90 - top, alt = std::min(altitude, 90.0);
    double u = (-top + std::sqrt(top * top + 4 * a * alt)) / (2 * a);
    return float(horizon() - focal() * std::tan(top * u * PI / 180));
  }
  double a = NIGHT_DEPTH - bottom, alt = std::min(-altitude, NIGHT_DEPTH);
  double v = (-bottom + std::sqrt(bottom * bottom + 4 * a * alt)) / (2 * a);
  return float(horizon() + focal() * std::tan(bottom * v * PI / 180));
}

void Renderer::updateLight() {
  auto direction = [&](float x, float y, double* dir) {
    double v[3] = {x - viewW_ / 2.0, double(horizon()) - y, focal()};
    double len = std::sqrt(dot3(v, v));
    for (int c = 0; c < 3; c++) dir[c] = v[c] / len;
  };
  const Heaven& h = heaven_;
  double sunAlt = altitudeOfRow(h.sunY);
  double moonAlt = altitudeOfRow(h.moonY);
  // The sun shines until it is far below the horizon; until then it still colors
  // the twilight. Only after that does the moon take over, if it is in the sky.
  // Both fade through zero so that nothing jumps at the change.
  double sunFade = smooth(-12.5, -9.5, sunAlt);
  bool moonThere = h.moonLit > 0.001 && moonAlt > -3;
  Light& l = light_;
  l.moon = sunFade <= 0 && moonThere && par_.moonlight > 0;
  l.x = l.moon ? h.moonX : h.sunX;
  l.y = l.moon ? h.moonY : h.sunY;
  double alt = l.moon ? moonAlt : sunAlt;

  double dir[3], atm[3];
  direction(l.x, l.y, dir);
  raise(dir, alt, atm);

  // Color of the light: what is left of it after the path through the air, at the
  // ground and at the height of the clouds.
  auto tint = [&](double height, double fade, double sat, float* out) {
    double T[3], peak = 1e-9;
    sunlightAt(height, atm, T);
    for (double t : T) peak = std::max(peak, t);
    // The eye partly compensates for the attenuation, the tint remains. Somewhat
    // desaturated: it sees low sunlight as golden, not orange.
    double gain = std::pow(peak, 0.4) * fade;
    for (int c = 0; c < 3; c++) out[c] = float(std::pow(T[c] / peak, sat) * gain);
  };
  double strength = 1;
  if (l.moon) {
    // Moonlight: a fraction of the sunlight, weaker with the phase. To the eye it
    // appears cool.
    double moonFade = (1 - smooth(-15.5, -12.5, sunAlt)) * smooth(-3, 2, moonAlt);
    strength = 0.075 * par_.moonlight * (0.3 + 0.7 * h.moonLit) * moonFade;
  }
  double ground = l.moon ? 1.0 : smooth(-2.5, 0.5, alt);
  tint(600, ground * (l.moon ? 1.0 : sunFade), 0.85, l.col);
  // Clouds scatter the light many times and it loses color in the process: in the
  // sunset glow they are salmon, not signal red.
  tint(CLOUD_LEVEL, l.moon ? 1.0 : sunFade, 0.62, l.cloud);
  tint(CIRRUS_LEVEL, l.moon ? 1.0 : sunFade, 0.62, l.cirrus);
  const double cool[3] = {0.72, 0.84, 1.0};
  for (int c = 0; c < 3; c++) {
    double k = strength * (l.moon ? cool[c] : 1.0);
    l.col[c] = float(l.col[c] * k);
    l.cloud[c] = float(l.cloud[c] * k);
    l.cirrus[c] = float(l.cirrus[c] * k);
  }
  l.gain = float(strength * (l.moon ? 1.0 : sunFade));

  l.night = float(1 - smooth(-11, -3, sunAlt));
  l.day = float(l.moon ? 0.0 : smooth(-6, 12, sunAlt));

  // Sky light for the shadow sides of the clouds: zenith and the side opposite the
  // light source.
  double back[3] = {-dir[0], 0.8, -dir[2]};
  double bl = std::sqrt(dot3(back, back));
  for (double& c : back) c /= bl;
  const double zenith[3] = {0, 1, 0};
  double up[3], opp[3];
  // For the sky light it counts where the light source really is.
  double backTrue[3] = {-atm[0], 0.8, -atm[2]};
  bl = std::sqrt(dot3(backTrue, backTrue));
  for (double& c : backTrue) c /= bl;
  skyRadiance(zenith, atm, atm, up);
  skyRadiance(backTrue, atm, atm, opp);
  const double nightAmb[3] = {0.004, 0.007, 0.016};
  // Multiply scattered light fills the sky far from the light source with blue: by
  // day as an addition, at twilight it is the blue of the blue hour, at night that
  // of the moonlit night.
  const double blue[3] = {0.15, 0.40, 1.0};
  const double dusk[3] = {0.12, 0.21, 0.50};
  double blueHour = l.moon ? 0.0 : smooth(-13, -5, sunAlt) * (1 - smooth(-3, 6, sunAlt));
  double filled = l.moon ? 1.4 * strength : 1.25 * smooth(-4, 16, sunAlt);
  // Afterglow above the horizon: starts when the sun reaches the mountains,
  // becomes redder with depth and goes out with the twilight.
  double glowing = l.moon ? 0.0 : (1 - smooth(1, 5, sunAlt)) * smooth(-13, -5.5, sunAlt);
  double deep = smooth(-9, -1, sunAlt);
  const double ember[3] = {1.0, 0.30 + 0.22 * deep, 0.06 + 0.10 * deep};
  for (int c = 0; c < 3; c++) {
    l.dir[c] = float(dir[c]);
    l.atm[c] = float(atm[c]);
    l.amb[c] = float(2.4 * (0.5 * up[c] + 0.5 * opp[c]) * l.gain + nightAmb[c] * l.night + 0.5 * dusk[c] * blueHour + 0.25 * blue[c] * filled);
    l.fill[c] = float(blue[c] * filled + dusk[c] * blueHour);
    l.after[c] = float(ember[c] * glowing * (1.1 + 0.9 * deep));
    // The ground: a bright surface facing the light source radiates its light back
    // divided by pi; in shadow under open sky it is left with the light of the sky,
    // averaged over the hemisphere.
    l.land[c] = float(l.col[c] * SUN_E / PI);
  }
  // The light of the sky at the ground. The singly scattered sky alone is too dark
  // and too blue for this: multiply scattered light and the reflection from the
  // landscape make it brighter and paler, otherwise the mountains in shadow would
  // be deep blue and hard in the image.
  double skyMix[3], skyLum = 0;
  const double lumOf[3] = {0.2126, 0.7152, 0.0722}, pale[3] = {0.84, 0.94, 1.08};
  for (int c = 0; c < 3; c++) {
    skyMix[c] = 1.25 * (0.45 * up[c] + 0.55 * opp[c]) * l.gain + 0.32 * blue[c] * filled;
    skyLum += lumOf[c] * skyMix[c];
  }
  // Under clouds it is gray, in a thunderstorm dark as well. Snow clouds are
  // paler than rain clouds.
  Sky wx = skyOf(par_.clouds);
  wx.dark *= 1 - 0.7f * coldOf(par_).snow;
  const double grey[3] = {0.95, 0.98, 1.04};
  for (int c = 0; c < 3; c++) {
    double clear = 2.3 * (0.45 * skyMix[c] + 0.55 * skyLum * pale[c]);
    double under = 2.3 * skyLum * grey[c] * (0.85 - 0.45 * wx.dark);
    l.skyLand[c] = float(clear + (under - clear) * 0.9 * wx.closed + 1.4 * nightAmb[c] * l.night + 0.6 * dusk[c] * blueHour);
  }
  l.alt = float(alt * PI / 180);
  // The lights of the city come on with the twilight.
  l.lights = float(1 - smooth(-6.5, 1.0, sunAlt));
  // Fog scatters the light of the sky and of the light source in all directions
  // and loses hardly any of it: it is about as bright as a white surface under
  // the open sky, a little grayer, and in the light of city lamps at night warm.
  // Around the light source it glares, unless clouds hide it.
  const double lamp[3] = {1.0, 0.78, 0.5};
  const bool city = land_[3] != nullptr;
  for (int c = 0; c < 3; c++) {
    double lum = 0.2126 * l.skyLand[0] + 0.7152 * l.skyLand[1] + 0.0722 * l.skyLand[2];
    double sun = 0.2126 * l.land[0] + 0.7152 * l.land[1] + 0.0722 * l.land[2];
    // Scattered many times, the light of the source loses most of its color.
    l.fog[c] = float(0.62 * (0.6 * l.skyLand[c] + 0.4 * lum) +
                     0.42 * (0.35 * l.land[c] + 0.65 * sun) * std::max(std::sin(alt * PI / 180), 0.0) * (1 - 0.75 * wx.closed) +
                     (city ? 0.02 * lamp[c] * l.lights : 0.0));
  }
  l.fogGlare = float(0.3 * (1 - 0.85 * wx.closed));

  // Exposure as with a camera: short while the sun is in the image, so that the
  // sky keeps its color; twilight and night need more.
  double expo = (0.18 + 0.05 * smooth(6, 40, sunAlt) + 0.30 * (1 - smooth(-6, 4, sunAlt))) * (1 - 0.4 * l.night);
  l.exposure = float(expo);
  // The disks. The moon is bright at night, in the day sky only a pale spot.
  double sunTop[3];
  sunlightAt(600, atm, sunTop);
  double peak = std::max({sunTop[0], sunTop[1], sunTop[2], 1e-9});
  // Low at the horizon the air dims the sun so much that its disk is visible with
  // edge and color; high in the sky it only glares.
  double discGain = std::pow(peak, 0.4) * smooth(-2.5, 0.5, sunAlt) * (0.14 + 0.86 * smooth(0.5, 9, sunAlt));
  double moonShow = h.moonLit > 0.001 ? smooth(-2, 1.5, moonAlt) : 0.0;
  double moonValue = (0.26 + 0.40 * l.night) / expo * moonShow;
  // The moon also takes on color low at the horizon: yellow, then orange.
  double moonDir[3], moonAtm[3], moonAir[3];
  direction(h.moonX, h.moonY, moonDir);
  raise(moonDir, moonAlt, moonAtm);
  sunlightAt(600, moonAtm, moonAir);
  double moonPeak = std::max({moonAir[0], moonAir[1], moonAir[2], 1e-9});
  for (int c = 0; c < 3; c++) {
    l.disc[c] = float(l.moon ? 0.0 : std::pow(sunTop[c] / peak, 0.85) * discGain * 110);
    l.moonDisc[c] = float(moonValue * std::pow(moonAir[c] / moonPeak, 0.8) * std::pow(moonPeak, 0.3));
  }
  // Close to the horizon the air lifts the lower edge of a disk more than the
  // upper one: it appears flattened.
  auto squash = [](double alt) { return float(1 - 0.17 * (1 - smooth(0, 6, alt))); };
  l.squash = squash(alt);
  l.moonSquash = squash(moonAlt);
}

// Size of the disks. Without a given value it follows the image size: the sun
// small and glaring, somewhat larger than in reality; the moon clearly larger so
// that its markings are visible.
void Renderer::sizeBodies() {
  sunRad_ = par_.sunRadius > 0 ? par_.sunRadius : float(std::clamp(std::lround(focal() / 165), 8L, 60L));
  moonRad_ = par_.moonRadius > 0 ? par_.moonRadius : float(std::clamp(std::lround(focal() / 88), 10L, 90L));
}

void Renderer::updateDepths() {
  // Focal length of the view: the longer, the more evenly the haze scatters
  // towards the viewer even far from the sun. And the depth of the haze up to the
  // rearmost plane; it is only the scale for distances and mist.
  float diag = std::hypot(viewW_, viewH_);
  zS_ = 1.25f * diag;
  zMax_ = 1.24f * diag;
  // Clouds and mountains belong to the wallpaper, that is, to the rearmost plane
  // directly behind the icons. This way they cast long rays across the screen.
  zFar_ = zMax_;
  count_ = std::min(int(rects_.size()), MAXO);
  int windows = 0;
  for (const auto& o : rects_) windows += o.kind == 1;
  struct Layer {
    const Occluder* o;
    float z;
    bool ghost;
  };
  std::vector<Layer> layers;
  int rank = 0;
  for (int i = 0; i < count_; i++) {
    const Occluder& o = rects_[i];
    float front = o.kind == 1 ? float(++rank) / (windows + 1) : 1.0f;
    layers.push_back({&o, zMax_ * WINDOW_BACK * (1 - par_.spread * front), false});
  }
  std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) { return a.z > b.z; });
  // Windows that have just disappeared stay briefly as ghosts: without shadow and
  // without coverage, only their area stays without light for that long.
  std::vector<Occluder> ghosts;
  for (const Track& t : tracks_) {
    if (t.gone > -1e8 && count_ + int(ghosts.size()) < MAXO) ghosts.push_back(t.last);
  }
  for (const Occluder& g : ghosts) layers.push_back({&g, 0.0f, true});
  count_ = int(layers.size());
  for (int i = 0; i < count_; i++) {
    const Occluder& o = *layers[i].o;
    float r[4] = {o.x0, o.y0, o.x1, o.y1}, p[4] = {layers[i].z, layers[i].ghost ? 0.0f : o.opacity, o.radius, float(o.kind)};
    std::memcpy(rectData_[i], r, sizeof(r));
    std::memcpy(parData_[i], p, sizeof(p));
    plateId_[i] = o.id;
    plateGhost_[i] = layers[i].ghost;
  }
}

// Records which windows have moved, are new or are missing.
void Renderer::trackScene(const std::vector<Occluder>& list) {
  for (const Occluder& o : list) {
    if (!o.id) continue;
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.id == o.id; });
    if (it == tracks_.end()) {
      Track t;
      t.id = o.id;
      t.last = o;
      t.seen = time_;
      if (sceneSeen_) t.born = time_;
      tracks_.push_back(t);
      continue;
    }
    Track& t = *it;
    if (t.gone > -1e8) {
      t.gone = -1e9;
      t.born = time_;
    }
    // Each edge by itself: when dragging to the right only the left and the right
    // one move, when resizing often only one.
    const float d[4] = {std::abs(o.x0 - t.last.x0), std::abs(o.y0 - t.last.y0), std::abs(o.x1 - t.last.x1), std::abs(o.y1 - t.last.y1)};
    if (std::max(std::max(d[0], d[1]), std::max(d[2], d[3])) > 0.01f) {
      double dt = std::max(time_ - t.seen, 1.0 / 500);
      for (int k = 0; k < 4; k++) {
        t.margin[k] = d[k] > 0.01f ? std::clamp(float(d[k] / dt * EDGE_LAG), EDGE_MIN, EDGE_MAX) : t.margin[k] * 0.5f;
      }
      t.moved = time_;
    }
    t.last = o;
    t.seen = time_;
  }
  for (Track& t : tracks_) {
    if (t.gone > -1e8) continue;
    bool here = std::any_of(list.begin(), list.end(), [&](const Occluder& o) { return o.id == t.id; });
    if (!here) t.gone = time_;
  }
  sceneSeen_ = true;
}

void Renderer::setScene(const std::vector<Occluder>& list) {
  if (list == rects_) return;
  trackScene(list);
  rects_ = list;
  updateDepths();
  sceneDirty_ = true;
}

void Renderer::setHeaven(const Heaven& h, bool gentle) {
  if (h == heaven_) return;
  float before[2] = {light_.x, light_.y};
  bool wasMoon = light_.moon;
  heaven_ = h;
  updateLight();
  float step = std::hypot(light_.x - before[0], light_.y - before[1]);
  // A jump is never gentle: the change of the light source, or time has moved on
  // while the image was idle.
  if (gentle && wasMoon == light_.moon && step < 24) {
    double dt = std::max(time_ - paceSeen_, 1e-3);
    pace_ = paceSeen_ > 0 ? 0.6f * pace_ + 0.4f * float(step / dt) : 0.0f;
    paceSeen_ = time_;
    heavenMoved_ = true;
  } else {
    pace_ = 0;
    paceSeen_ = 0;
    skyDirty_ = true;
    sceneDirty_ = true;
  }
}

void Renderer::setScene(const std::vector<Occluder>& list, float sunX, float sunY) {
  setScene(list);
  Heaven h;
  h.sunX = sunX;
  h.sunY = sunY;
  setHeaven(h);
}

void Renderer::setIconMask(const uint8_t* cover, int w, int h) {
  iconData_.assign(cover, cover + size_t(w) * h);
  iconW_ = w;
  iconH_ = h;
  iconDirty_ = true;
  sceneDirty_ = true;
}

void Renderer::setCursorShape(const uint8_t* cover, int w, int h) {
  if (cursor_.w != w || cursor_.h != h) cursor_ = makeTarget(w, h, DXGI_FORMAT_R8_UNORM, true);
  if (!cursor_.tex) return;
  ctx_->UpdateSubresource(cursor_.tex.Get(), 0, nullptr, cover, w, 0);
  ctx_->GenerateMips(cursor_.srv.Get());
  cursorMoved_ = true;
}

void Renderer::setCursor(bool visible, float x0, float y0, float x1, float y1, float calm) {
  float r[4] = {x0, y0, x1, y1};
  if (!visible) r[0] = r[1] = 0, r[2] = r[3] = -1;
  if (!std::memcmp(r, cursorRect_, sizeof(r)) && calm == cursorCalm_) return;
  std::memcpy(cursorRect_, r, sizeof(r));
  cursorCalm_ = calm;
  cursorMoved_ = true;
}

void Renderer::setOutput(bool linear, float white) {
  if (linear == linear_ && white == white_) return;
  linear_ = linear;
  white_ = white;
  sceneDirty_ = true;
}

void Renderer::setParams(const Params& p) {
  if (p == par_) return;
  bool sky = p.clouds != par_.clouds || p.sunRadius != par_.sunRadius || p.moonRadius != par_.moonRadius || p.rays != par_.rays ||
             p.stars != par_.stars || p.moonlight != par_.moonlight || p.ground != par_.ground;
  bool res = p.resolution != par_.resolution;
  // The haze also lies between eye and ground: the finished image needs it anew,
  // the clouds do not. The same goes for fog and for what the temperature decides.
  bool air = p.fog != par_.fog || p.temperature != par_.temperature || p.dayTemperature != par_.dayTemperature || p.snowDepth != par_.snowDepth;
  if (p.density != par_.density || air) faceDue_ = true;
  par_ = p;
  if (!dev_ || !sky_.tex) return;
  sizeBodies();
  if (res) allocScatter();
  if (sky) {
    updateLight();
    skyDirty_ = true;
  } else if (air) {
    updateLight();
  }
  updateDepths();
  sceneDirty_ = true;
}

void Renderer::drift(float fog, float temperature, float dayTemperature, float snowDepth) {
  if (fog == par_.fog && temperature == par_.temperature && dayTemperature == par_.dayTemperature && snowDepth == par_.snowDepth) return;
  par_.fog = fog;
  par_.temperature = temperature;
  par_.dayTemperature = dayTemperature;
  par_.snowDepth = snowDepth;
  if (!dev_ || !sky_.tex) return;
  updateLight();
  faceDue_ = true;
}

// Shared quantities of the sky shaders. shift is the wind shift of the clouds,
// jit and sub offset the sampling per pass.
// The weather changes by itself (live weather): clouds and wind glide in small
// steps. Nothing starts over, the next cloud passes pick up the new state.
void Renderer::drift(float clouds, float wind) {
  if (clouds == par_.clouds && wind == par_.wind) return;
  bool cloudsMoved = clouds != par_.clouds;
  par_.clouds = clouds;
  par_.wind = wind;
  if (!dev_ || !sky_.tex) return;
  updateLight();
  heavenMoved_ = true;
  // If the clouds stand still, no new pass would be made otherwise.
  if (cloudsMoved && !(wind > 0 && clouds > 0)) skyAccum_ = std::min(skyAccum_, SKY_ACC / 2);
}

void Renderer::fillSky(SkyC& c, int w, int h, int steps, float shift, float jit, float subX, float subY, float show) {
  c = {};
  c.res[0] = float(w), c.res[1] = float(h);
  c.view[0] = viewW_, c.view[1] = viewH_;
  const Light& l = light_;
  c.sun[0] = l.x, c.sun[1] = l.y;
  c.sub[0] = subX, c.sub[1] = subY;
  c.sunR = sunRadius();
  c.moonR = moonRad_;
  c.shape[0] = l.squash, c.shape[1] = l.moonSquash, c.shape[2] = par_.rays;
  c.focal = focal();
  c.horizon = horizon();
  c.clouds = par_.clouds;
  c.night = l.night;
  c.day = l.day;
  c.cloudT = shift;
  c.jit = jit;
  c.exposure = l.exposure;
  c.show = show;
  c.cloudSteps = steps;
  c.share = 1;
  c.back = float(cloudT_ - airShiftOf_[airNew_]);
  c.backOld = float(cloudT_ - airShiftOf_[airOld_]);
  c.fade = fade_;
  c.evolve = float(evolveT_);
  c.skyGain = l.gain;
  c.time = float(std::fmod(time_, 3600.0));
  c.stars = par_.stars;
  c.span = float(columnTop_), c.count = float(columnH_);
  Sky wx = skyOf(par_.clouds);
  const Cold cold = coldOf(par_);
  const Fog fog = fogOf(par_);
  wx.dark *= 1 - 0.7f * cold.snow;
  c.cover = wx.cover;
  c.storm[0] = wx.dark, c.storm[1] = wx.rain, c.storm[2] = rainHere_;
  c.snow[0] = cold.snow, c.snow[1] = cold.line, c.snow[2] = cold.cover;
  c.fog[0] = fog.ext, c.fog[1] = fog.top, c.fog[2] = fog.soft, c.fog[3] = fog.banks;
  for (int i = 0; i < 3; i++) c.fogCol[i] = l.fog[i];
  c.fogCol[3] = l.fogGlare;
  c.storm[3] = float(std::fmod(time_, 64.0));
  // The lightning: bluish white, far brighter than everything else below the
  // clouds.
  const float flashCol[3] = {8.9f, 10.1f, 12.0f};
  for (int i = 0; i < 3; i++) c.flash[i] = flash_.pos[i], c.flashCol[i] = flashCol[i];
  c.flash[3] = flash_.level;
  c.flashCol[3] = float(flash_.segs);
  c.flashMore[0] = flash_.trunk, c.flashMore[1] = flash_.twig, c.flashMore[2] = flash_.level;
  c.flashMore[3] = flash_.grow >= 1 ? 1e9f : flash_.top + (flash_.bottom - flash_.top) * flash_.grow;
  c.srcMoon = l.moon ? 1.0f : 0.0f;
  c.moonLit = heaven_.moonLit;
  c.moonLimb = heaven_.moonLimb;
  c.moonSpin = heaven_.moonSpin;
  c.moon[0] = heaven_.moonX, c.moon[1] = heaven_.moonY;
  for (int i = 0; i < 3; i++) {
    c.sunCol[i] = l.col[i], c.sunDir[i] = l.dir[i], c.amb[i] = l.amb[i];
    c.sunAtm[i] = l.atm[i], c.cloudCol[i] = l.cloud[i], c.cirrusCol[i] = l.cirrus[i];
    c.discCol[i] = l.disc[i], c.moonCol[i] = l.moonDisc[i], c.fill[i] = l.fill[i], c.after[i] = l.after[i];
  }
  // The angle to the light source grows by the same rule as the altitude above the
  // horizon: at the top edge of the image the angle to there has become a right
  // angle.
  double top = std::atan2(double(horizon()), double(focal()));
  c.widen = float((PI / 2 - top) / (top * top));
  const Ground& g = GROUNDS[groundOf(par_)];
  c.cloudLift = g.cloudLift;
  c.ground[0] = land_[0] ? 1.0f : 0.0f;
  c.ground[1] = g.water ? 1.0f : 0.0f;
  c.ground[2] = g.eyeWater;
  c.ground[3] = g.eyeSea;
  c.strip[0] = STRIP_TOP, c.strip[1] = STRIP_SPAN;
  c.strip[2] = l.lights;
  c.land[3] = land_[3] ? 1.0f : 0.0f;
  // The wind at the ground: how far the gust fields have moved over the water, how
  // slanted the rain falls, how often waves break.
  c.strip[3] = float(gustT_);
  c.wind[0] = float(std::min(surfSpeed() / fallSpeed(), 3.0));
  // Flakes close to the viewer fall slower still and nearly ride on the wind.
  c.snow[3] = float(std::min(surfSpeed() / SNOW_SPEED, 4.0));
  c.wind[1] = float(surfSpeed() * g.shelter);
  c.wind[2] = foamOdds_;
  c.wind[3] = foamCover_;
  for (int i = 0; i < 3; i++) c.sunLand[i] = l.land[i], c.skyLand[i] = l.skyLand[i], c.sea[i] = g.sea[i];
  c.land[0] = l.alt;
  c.land[1] = wx.closed;
  c.land[2] = 0.45f + par_.density;
  ctx_->UpdateSubresource(skyBuf_.Get(), 0, nullptr, &c, 0, 0);
  ctx_->PSSetConstantBuffers(0, 1, skyBuf_.GetAddressOf());
}

void Renderer::fillLight(LightC& c) {
  c.view[0] = viewW_, c.view[1] = viewH_;
  c.sun[0] = light_.x, c.sun[1] = light_.y;
  c.sunR = sunRadius();
  c.zS = zS_, c.zMax = zMax_, c.zFar = zFar_;
  c.moonPar[0] = light_.moon ? 1.0f : 0.0f;
  c.moonPar[1] = heaven_.moonLit, c.moonPar[2] = heaven_.moonLimb, c.moonPar[3] = heaven_.moonSpin;
  c.glow[0] = std::max(par_.glow, 1.0f);
  c.glow[1] = light_.squash;
  c.glow[3] = float(dustT_);
  // Ray length: the longer, the closer to the viewer there is still haze, and the
  // farther a shadow reaches before it fades out.
  c.nearFade = 0.6f * (1 - std::clamp(par_.length, 0.1f, 0.95f));
  c.g = par_.g;
  c.n = count_;
  c.linear = linear_ ? 1.0f : 0.0f;
  c.white = white_;
  std::memcpy(c.cursor, cursorRect_, sizeof(cursorRect_));
  c.cursorPar[0] = cursor_.w > 0 ? (cursorRect_[2] - cursorRect_[0]) / cursor_.w : 1.0f;
  c.cursorPar[1] = cursorCalm_;
  c.cursorPar[2] = cursor_.srv ? par_.pointer : 0.0f;
  c.texel[0] = viewW_ / sky_.w, c.texel[1] = viewW_ / iconW_;
  for (int i = 0; i < 3; i++) {
    c.discCol[i] = (light_.moon ? light_.moonDisc[i] : light_.disc[i]) * light_.exposure;
    c.sunCol[i] = light_.col[i];
  }
  if (flashPass_) {
    // The lightning as a light source: where it is in the image, wider than the sun,
    // bluish white, without a disk.
    const float tint[3] = {0.78f, 0.87f, 1.0f};
    c.sun[0] = flash_.x, c.sun[1] = flash_.y;
    c.sunR = 30;
    c.glow[2] = 1;
    c.moonPar[0] = 0;
    for (int i = 0; i < 3; i++) c.sunCol[i] = tint[i] * 0.45f * flash_.level, c.discCol[i] = 0;
  }
  std::memcpy(c.rect, rectData_, sizeof(rectData_));
  std::memcpy(c.par, parData_, sizeof(parData_));
  for (int i = 0; i < count_; i++) {
    if (!plateId_[i]) continue;
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.id == plateId_[i]; });
    if (it == tracks_.end()) continue;
    c.aux[i][0] = plateGhost_[i] ? float(1 - smooth(GHOST_HOLD, GHOST_LIFE, time_ - it->gone))
                                 : float(1 - smooth(APPEAR_HOLD, APPEAR_DONE, time_ - it->born));
    c.aux[i][1] = plateGhost_[i] ? 1.0f : 0.0f;
    float still = float(1 - smooth(0.05, 0.25, time_ - it->moved));
    for (int k = 0; k < 4; k++) c.edge[i][k] = plateGhost_[i] ? 0.0f : it->margin[k] * still;
  }
  ctx_->UpdateSubresource(lightBuf_.Get(), 0, nullptr, &c, 0, 0);
  ctx_->PSSetConstantBuffers(0, 1, lightBuf_.GetAddressOf());
  ctx_->VSSetConstantBuffers(0, 1, lightBuf_.GetAddressOf());
}

// Initial state of the clouds: the sun is unobscured, close to a cloud edge, and
// stays unobscured for a while afterwards. This way there are rays from the
// start.
void Renderer::pickStart() {
  const int N = 1024;
  const double SPAN = 12;
  Target t = makeTarget(N, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, false);
  target(t.rtv.Get(), N, 1);
  SkyC c;
  c = {};
  fillSky(c, N, 1, 0, 0, 0, 0, 0, 0);
  c.span = float(SPAN);
  c.count = float(N);
  ctx_->UpdateSubresource(skyBuf_.Get(), 0, nullptr, &c, 0, 0);
  ctx_->PSSetShaderResources(0, 1, noise_.GetAddressOf());
  ID3D11ShaderResourceView* clouds[4] = {shape_.Get(), detail_.Get(), weather_.Get(), turb_.Get()};
  ctx_->PSSetShaderResources(5, 4, clouds);
  quad(psPick_.Get());

  D3D11_TEXTURE2D_DESC d;
  t.tex->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> copy;
  dev_->CreateTexture2D(&d, nullptr, &copy);
  ctx_->CopyResource(copy.Get(), t.tex.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
  const float* px = static_cast<const float*>(m.pData);
  auto open = [&](int i) { return px[i * 4]; };

  int soon = int(std::lround(0.06 / SPAN * N)), hold = int(std::lround(0.45 / SPAN * N));
  int best = -1;
  double bestScore = 0.8;
  for (int i = 0; i + hold < N; i++) {
    if (px[i * 4 + 1] < 0.97f || open(i) < 0.62f || open(i) > 0.9f) continue;
    double later = 0;
    for (int k = soon; k <= hold; k++) later += open(i + k);
    later /= hold - soon + 1;
    if (later > bestScore) {
      bestScore = later;
      best = i;
    }
  }
  // No cloud edge to be found: then at least an unobscured sun.
  for (int i = 0; best < 0 && i < N; i++) {
    if (open(i) > 0.97f) best = i;
  }
  if (best >= 0) cloudT_ = (best + 0.5) / N * SPAN;
  ctx_->Unmap(copy.Get(), 0);
}

// Sky without clouds and the haze color of the mountain ranges: change only with
// the sun.
void Renderer::drawBase() {
  SkyC c;
  target(haze_.rtv.Get(), haze_.w, 1);
  fillSky(c, haze_.w, 1, 0, 0, 0, 0, 0, 0);
  quad(psHaze_.Get());
  target(base_.rtv.Get(), base_.w, base_.h);
  fillSky(c, base_.w, base_.h, 0, 0, 0, 0, 0, 0);
  quad(psBase_.Get());
  target(nullptr, 1, 1);
}

// A cloud pass: the clouds once more, with slightly differently offset sampling,
// averaged with the previous image. Averaged, the grid of the steps disappears.
void Renderer::beginPass() {
  bool first = skyAccum_ == 0;
  // If the light source moves fast (time lapse), the light of the clouds should
  // follow it instead of lagging for long: then the new counts more.
  float floor = std::clamp(pace_ / 60, 1.0f / SKY_ACC, 0.34f);
  pass_.share = first ? 1.0f : std::max(1.0f / (skyAccum_ + 1), floor);
  // The first pass after a user change with fewer steps, so that dragging the sun
  // stays smooth; the following ones refine it.
  pass_.steps = int(std::lround(CLOUD_STEPS * (first ? 0.6 : 1.0)));
  pass_.shift = float(cloudT_);
  pass_.evolve = float(evolveT_);
  // Every pass shifts the sampling pattern so that the remainder after averaging
  // does not form a fixed grid.
  pass_.jit = 5.588238f * float(skyShot_ % 64);
  pass_.subX = first ? 0 : float(std::fmod(skyShot_ * 0.7548776662, 1.0) - 0.5);
  pass_.subY = first ? 0 : float(std::fmod(skyShot_ * 0.5698402910, 1.0) - 0.5);
  pass_.band = 0;
  passActive_ = true;
}

// The next horizontal band of the running pass. It is drawn into the cloud image
// in progress and read from the newest finished one.
void Renderer::airBand() {
  int n = airWork_;
  ID3D11ShaderResourceView* none[13] = {};
  ctx_->PSSetShaderResources(0, 13, none);
  ctx_->VSSetShaderResources(0, 6, none);
  ID3D11RenderTargetView* out[2] = {air_[n].rtv.Get(), airZ_[n].rtv.Get()};
  ctx_->OMSetRenderTargets(2, out, nullptr);
  D3D11_VIEWPORT vp = {0, 0, float(air_[n].w), float(air_[n].h), 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ctx_->OMSetBlendState(blendOff_.Get(), nullptr, 0xffffffff);
  D3D11_RECT cut = {0, LONG(std::lround(air_[n].h * (1 - AIR_CUTS[pass_.band + 1]))), air_[n].w,
                    LONG(std::lround(air_[n].h * (1 - AIR_CUTS[pass_.band])))};
  ctx_->RSSetState(rasterCut_.Get());
  ctx_->RSSetScissorRects(1, &cut);
  SkyC c;
  fillSky(c, air_[n].w, air_[n].h, pass_.steps, pass_.shift, pass_.jit, pass_.subX, pass_.subY, 0);
  double back = double(pass_.shift) - airShiftOf_[airNew_];
  // After a jump of the shift there is no previous image that fits.
  c.share = std::abs(back) > 0.5 ? 1.0f : pass_.share;
  c.back = float(back);
  c.evolve = pass_.evolve;
  ctx_->UpdateSubresource(skyBuf_.Get(), 0, nullptr, &c, 0, 0);
  ID3D11ShaderResourceView* in[5] = {noise_.Get(), air_[airNew_].srv.Get(), nullptr, airZ_[airNew_].srv.Get(), base_.srv.Get()};
  ctx_->PSSetShaderResources(0, 5, in);
  ID3D11ShaderResourceView* clouds[5] = {shape_.Get(), detail_.Get(), weather_.Get(), turb_.Get(), near_[airNew_].srv.Get()};
  ctx_->PSSetShaderResources(5, 5, clouds);
  quad(psAir_.Get());
  pass_.band++;
}

// For a finished cloud image, the distance of the frontmost cloud in the
// neighborhood of every pixel: with it the edges are moved along.
void Renderer::drawNear(int image) {
  SkyC c;
  target(nearBlock_.rtv.Get(), nearBlock_.w, nearBlock_.h);
  fillSky(c, nearBlock_.w, nearBlock_.h, 0, 0, 0, 0, 0, 0);
  ctx_->PSSetShaderResources(3, 1, airZ_[image].srv.GetAddressOf());
  quad(psNearBlock_.Get());
  target(near_[image].rtv.Get(), near_[image].w, near_[image].h);
  ctx_->PSSetShaderResources(9, 1, nearBlock_.srv.GetAddressOf());
  quad(psNear_.Get());
  target(nullptr, 1, 1);
}

// The cloud images for the finished image: the newest and the one before.
void Renderer::bindClouds() {
  ID3D11ShaderResourceView* in[5] = {noise_.Get(), air_[airNew_].srv.Get(), haze_.srv.Get(), airZ_[airNew_].srv.Get(), base_.srv.Get()};
  ctx_->PSSetShaderResources(0, 5, in);
  ID3D11ShaderResourceView* more[4] = {near_[airNew_].srv.Get(), air_[airOld_].srv.Get(), near_[airOld_].srv.Get(), airZ_[airOld_].srv.Get()};
  ctx_->PSSetShaderResources(9, 4, more);
}

// Density of the cumulus above a place (meters from the viewer, x to the right,
// z in view direction), 0 to 1: the same computation as cloudWeather and
// rainRate in the shaders, from the weather map in memory.
float Renderer::cloudDensity(double x, double z) const {
  if (weatherW_ <= 0) return 0;
  double shift = cloudT_ * CLOUD_TILE;
  double u = (x + shift) / 100e3, v = (z + shift * 0.3) / 100e3;
  u = (u - std::floor(u)) * weatherW_ - 0.5;
  v = (v - std::floor(v)) * weatherH_ - 0.5;
  int x0 = int(std::floor(u)), y0 = int(std::floor(v));
  double fx = u - x0, fy = v - y0;
  auto at = [&](int xi, int yi, int ch) {
    xi = ((xi % weatherW_) + weatherW_) % weatherW_;
    yi = ((yi % weatherH_) + weatherH_) % weatherH_;
    return weatherMap_[(size_t(yi) * weatherW_ + xi) * 4 + ch] / 255.0;
  };
  float cover = skyOf(par_.clouds).cover;
  // Height 1150 m: above the middle in the lower layer, at the lower edge in the
  // upper one.
  const double open[2] = {0.528, 0.999};
  double best = 0;
  for (int ch = 0; ch < 2; ch++) {
    double lw = (at(x0, y0, ch) * (1 - fx) + at(x0 + 1, y0, ch) * fx) * (1 - fy) + (at(x0, y0 + 1, ch) * (1 - fx) + at(x0 + 1, y0 + 1, ch) * fx) * fy;
    best = std::max(best, std::clamp((lw * 0.4 + 0.6 - (1 - cover * open[ch])) / 0.6, 0.0, 1.0));
  }
  return float(best);
}

// The weather at the viewer's location: rain falls when a dense cloud passes
// above it.
// snap: the new state at once, otherwise gliding.
float Renderer::altitudeOf(const Params& p) { return GROUNDS[groundOf(p)].eyeSea; }

float Renderer::snowing() const { return coldOf(par_).snow; }

// How fast what falls from the clouds falls, meters per second: rain fast, snow
// slowly.
double Renderer::fallSpeed() const {
  double snow = coldOf(par_).snow;
  return RAIN_SPEED + (SNOW_SPEED - RAIN_SPEED) * snow;
}

void Renderer::updateWeather(double dt, bool snap) {
  Sky wx = skyOf(par_.clouds);
  // The wind carries the rain along as it falls: what arrives at the viewer's
  // location fell from a cloud that is farther upwind. Snow is carried much
  // farther; beyond three times the height of fall the slant is capped, as in the
  // image.
  double carried = std::min(surfSpeed() / fallSpeed(), 3.0) * (750.0 + GROUNDS[groundOf(par_)].cloudLift);
  float shower = float(smooth(0.62 - 0.32 * wx.rain, 0.95 - 0.2 * wx.rain, cloudDensity(-SURF_X * carried, -SURF_Z * carried)));
  float want = wx.rain * (shower + (1 - shower) * 0.3f * wx.rain);
  rainHere_ = snap ? want : rainHere_ + (want - rainHere_) * float(std::min(1.0, dt / 2.5));
}

float Renderer::chance() {
  dice_ ^= dice_ << 13, dice_ ^= dice_ >> 17, dice_ ^= dice_ << 5;
  return float(dice_ & 0xffffff) / float(0x1000000);
}

// A new lightning flash. It forms in the densest cloud among a few randomly
// chosen spots in the field of view; if none is dense enough, there is none.
// Three out of four strike through to the ground as a visible channel, a few
// kilometers in front of the viewer, so that the channel spans a good part of
// the image; the others only glow in the clouds, often far away. force: even
// without a dense cloud.
bool Renderer::startFlash(bool force) {
  const Ground& g = GROUNDS[groundOf(par_)];
  bool channel = force || chance() < 0.75f;
  float best = -1;
  double bx = 0, bz = 9000;
  // A channel reaches from the ground to the cloud base, a few hundred meters. How
  // high it is in the image depends on the distance alone: it is chosen so that
  // the channel spans a good third up to almost the whole height of the sky, over
  // water as over the mountains.
  double top = 870.0 + g.cloudLift, room = horizon() / focal();
  for (int i = 0; i < 24; i++) {
    double az = (chance() * 2 - 1) * (channel ? 0.33 : 0.42);
    double range = channel ? top / (room * (0.36 + 0.60 * double(chance()))) : 6000 + 30000 * double(chance());
    double x = range * std::sin(az), z = range * std::cos(az);
    float dens = cloudDensity(x, z);
    if (dens > best) best = dens, bx = x, bz = z;
  }
  if (best < 0.3f && !force) return false;

  Flash f;
  f.on = true;
  f.start = time_;
  float base = 750 + g.cloudLift;   // cloud base above the eye
  f.pos[0] = float(bx), f.pos[1] = base + 320, f.pos[2] = float(bz);
  // Several return strokes in quick succession: that is why a lightning flash
  // flickers.
  f.strokes = 1 + int(chance() * 4.6f);
  // Before the first return stroke the stepped leader advances from the cloud to
  // the ground, within a few hundredths of a second.
  float when = channel ? 0.030f + 0.025f * chance() : 0.0f;
  for (int i = 0; i < f.strokes; i++) {
    f.at[i] = when;
    f.gain[i] = i == 0 ? 1.0f : 0.35f + 0.65f * chance();
    when += 0.045f + 0.10f * chance();
  }
  auto screen = [&](const float* v, float* out) {
    float z = std::max(v[2], 50.0f);
    out[0] = viewW_ / 2 + v[0] / z * focal();
    out[1] = horizon() - v[1] / z * focal();
  };
  float at[2];
  screen(f.pos, at);

  BoltC bc = {};
  if (channel) {
    int n = 0;
    float range = float(std::hypot(bx, bz));
    // Near lightning flashes are wider in the image.
    float thick = std::clamp(0.85f + 1.7f * (base + 120) / range, 0.9f, 1.6f);
    auto add = [&](const float* a0, const float* a1, float gain, float width, float kind) {
      if (n >= BOLT_MAX) return;
      screen(a0, bc.seg[n]);
      screen(a1, bc.seg[n] + 2);
      bc.par[n][0] = gain, bc.par[n][1] = width * thick, bc.par[n][2] = kind;
      n++;
    };
    // A channel from a to b: bent in the middle again and again, strongly at large
    // scale, finely at small scale.
    auto jag = [&](const float* a, const float* b, int count, float rough, float (*out)[3]) {
      float len = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
      for (int c = 0; c < 3; c++) out[0][c] = a[c], out[count][c] = b[c];
      for (int step = count; step > 1; step /= 2) {
        float amp = len * rough * std::pow(float(step) / count, 0.7f);
        for (int i = step / 2; i < count; i += step) {
          for (int c = 0; c < 3; c++) {
            out[i][c] = 0.5f * (out[i - step / 2][c] + out[i + step / 2][c]) + (chance() - 0.5f) * 2 * amp * (c == 1 ? 0.3f : 1.0f);
          }
        }
      }
      return len;
    };
    const int N = 128;
    float p[N + 1][3];
    float down = g.water ? g.eyeWater : 1300.0f;
    // The channel starts in the cloud and fades out upwards there.
    float from[3] = {f.pos[0], base + 300, f.pos[2]};
    float to[3] = {f.pos[0] + (chance() - 0.5f) * 0.35f * (base + down), -down, f.pos[2] + (chance() - 0.5f) * 1200};
    float len = jag(from, to, N, 0.11f, p);
    for (int i = 0; i < N; i++) {
      // It leaves the cloud as a thin filament and becomes full only below it.
      float into = float(smooth(0.0, 240.0, base + 300 - 0.5 * (p[i][1] + p[i + 1][1])));
      add(p[i], p[i + 1], into * into, (0.8f - 0.2f * float(i) / N) * (0.45f + 0.55f * into), 0);
    }
    auto unit = [](float* v) {
      float l = std::max(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 1e-3f);
      for (int c = 0; c < 3; c++) v[c] /= l;
    };
    // A branch from the point at: it deviates from direction dir by the angle ang,
    // to the side given by side, never leads upwards and ends above the ground.
    // reach: its length, shortened if the ground is closer.
    auto limb = [&](const float* at, const float* dir, float side, float ang, float& reach, int count, float (*q)[3]) {
      float out[3] = {side * (0.8f + 0.2f * chance()), 0, (chance() - 0.5f) * 0.9f};
      unit(out);
      float tip[3];
      for (int c = 0; c < 3; c++) tip[c] = at[c] + reach * (std::cos(ang) * dir[c] + std::sin(ang) * out[c]);
      tip[1] = std::min(tip[1], at[1] - 0.3f * reach);
      float drop = at[1] - tip[1], room = at[1] + down;
      if (drop > 0.85f * room) {
        float keep = 0.85f * room / drop;
        for (int c = 0; c < 3; c++) tip[c] = at[c] + (tip[c] - at[c]) * keep;
        reach *= keep;
      }
      jag(at, tip, count, 0.14f, q);
    };
    // Branches fork off downwards below the cloud at an acute angle, become thinner
    // and fade out; the long ones tend to be at the top.
    int first = 2;
    while (first < N - 24 && p[first][1] > base + 60) first++;
    int branches = 6 + int(chance() * 5.99f);
    for (int k = 0; k < branches; k++) {
      int j = std::min(first + int(std::pow(chance(), 1.25f) * ((N - first) * 0.88f)), N - 3);
      float dir[3];
      for (int c = 0; c < 3; c++) dir[c] = p[j + 2][c] - p[j - 2][c];
      unit(dir);
      float side = chance() < 0.5f ? -1.0f : 1.0f;
      float reach = len * (0.07f + 0.27f * chance() * chance()) * (1.0f - 0.45f * float(j) / N);
      const int M = 32;
      float q[M + 1][3];
      limb(p[j], dir, side, 0.3f + 0.6f * chance(), reach, M, q);
      float bright = 0.26f + 0.30f * chance();
      for (int i = 0; i < M; i++) {
        float along = float(i) / M;
        add(q[i], q[i + 1], bright * (1.0f - 0.88f * along), 0.44f - 0.12f * along, 1);
      }
      // Some fork once or twice more.
      int forks = chance() < 0.65f ? (chance() < 0.4f ? 2 : 1) : 0;
      for (int s = 0; s < forks; s++) {
        int m = 5 + int(chance() * (M - 12));
        float dir2[3];
        for (int c = 0; c < 3; c++) dir2[c] = q[m + 1][c] - q[m - 1][c];
        unit(dir2);
        float reach2 = reach * (0.2f + 0.35f * chance());
        const int M2 = 16;
        float w[M2 + 1][3];
        limb(q[m], dir2, chance() < 0.7f ? -side : side, 0.3f + 0.6f * chance(), reach2, M2, w);
        float lit = 0.8f * bright * (1.0f - 0.88f * float(m) / M);
        for (int i = 0; i < M2; i++) add(w[i], w[i + 1], lit * (1.0f - 0.9f * float(i) / M2), 0.36f, 1);
      }
    }
    // Plus short stubs on the main channel that go out again at once.
    int stubs = 6 + int(chance() * 5.99f);
    for (int k = 0; k < stubs; k++) {
      int j = std::min(first + int(chance() * ((N - first) * 0.95f)), N - 3);
      float dir[3];
      for (int c = 0; c < 3; c++) dir[c] = p[j + 2][c] - p[j - 2][c];
      unit(dir);
      float reach = len * (0.02f + 0.04f * chance());
      const int M3 = 8;
      float w[M3 + 1][3];
      limb(p[j], dir, chance() < 0.5f ? -1.0f : 1.0f, 0.4f + 0.7f * chance(), reach, M3, w);
      float lit = 0.14f + 0.16f * chance();
      for (int i = 0; i < M3; i++) add(w[i], w[i + 1], lit * (1.0f - 0.9f * float(i) / M3), 0.34f, 1);
    }
    f.segs = n;
    f.top = bc.seg[0][1], f.bottom = bc.seg[N - 1][3];
    // For the light on the desktop the light source is in the middle of the channel,
    // as far as it lies in the image.
    at[0] = 0.5f * (bc.seg[0][0] + bc.seg[N - 1][2]);
    at[1] = std::clamp(0.5f * (bc.seg[0][1] + bc.seg[N - 1][3]), 0.0f, viewH_);
  }
  f.x = at[0], f.y = at[1];
  ctx_->UpdateSubresource(boltBuf_.Get(), 0, nullptr, &bc, 0, 0);
  flash_ = f;
  boltDue_ = f.segs > 0;
  return true;
}

// The channel of the new lightning flash into its two images: once per flash,
// afterwards only its brightness flickers. One rectangle per segment, first for
// the wide glow, then for filament and narrow glow.
void Renderer::drawBolt() {
  boltDue_ = false;
  ID3D11ShaderResourceView* none[2] = {};
  ctx_->PSSetShaderResources(21, 2, none);
  const float black[4] = {};
  struct Step {
    Target* to;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
  } steps[2] = {{&boltFar_, vsBoltFar_.Get(), psBoltFar_.Get()}, {&bolt_, vsBolt_.Get(), psBolt_.Get()}};
  for (const Step& s : steps) {
    SkyC c;
    target(s.to->rtv.Get(), s.to->w, s.to->h);
    ctx_->ClearRenderTargetView(s.to->rtv.Get(), black);
    fillSky(c, s.to->w, s.to->h, 0, 0, 0, 0, 0, 0);
    ctx_->VSSetConstantBuffers(0, 1, skyBuf_.GetAddressOf());
    ctx_->VSSetConstantBuffers(2, 1, boltBuf_.GetAddressOf());
    ctx_->OMSetBlendState(blendMax_.Get(), nullptr, 0xffffffff);
    ctx_->VSSetShader(s.vs, nullptr, 0);
    ctx_->PSSetShader(s.ps, nullptr, 0);
    ctx_->Draw(flash_.segs * 6, 0);
  }
  target(nullptr, 1, 1);
}

// The lightning of this moment. First the stepped leader advances from the cloud
// to the ground as a faint glow. When it reaches the ground, the whole channel
// lights up; every return stroke is a sharp pulse that mostly decays within a
// hundredth of a second, in between the channel keeps glowing: this way it
// flickers and still stays in the image long enough to be seen. The branches
// glow only during the first return stroke. Without a lightning flash in
// progress, a random draw decides when the next one comes, the sooner the
// stronger the thunderstorm is.
void Renderer::updateFlash() {
  Flash& f = flash_;
  if (f.hold) return;
  // No thunderstorm in snowfall: thunder in snow is so rare that it would look
  // like a mistake.
  float thunder = skyOf(par_.clouds).thunder * (1 - coldOf(par_).snow);
  if (f.on) {
    double t = time_ - f.start, last = f.at[f.strokes - 1];
    if (t < f.at[0] && thunder > 0.001f) {
      f.grow = float(t / f.at[0]);
      f.level = 0.004f + 0.012f * f.grow;
      f.trunk = f.twig = 0.012f;
      return;
    }
    f.grow = 2;
    auto pulse = [](double since) { return float(0.72 * std::exp(-since / 0.014) + 0.28 * std::exp(-since / 0.075)); };
    float level = 0;
    for (int i = 0; i < f.strokes; i++) {
      if (t >= f.at[i]) level += f.gain[i] * pulse(t - f.at[i]);
    }
    float linger = float(1 - smooth(last + 0.05, last + 0.3, t));
    f.level = std::min(level, 1.5f);
    f.trunk = std::max(f.level, 0.16f * linger);
    f.twig = pulse(t - f.at[0]) + 0.05f * linger;
    if (t > last + 0.32 || thunder <= 0.001f) f.on = false, f.level = f.trunk = f.twig = 0;
    return;
  }
  f.level = f.trunk = f.twig = 0;
  if (thunder <= 0.001f || skyAccum_ == 0) {
    nextFlash_ = 0;
    return;
  }
  if (nextFlash_ <= 0) nextFlash_ = time_ + 1.0 + 3 * chance();
  if (time_ < nextFlash_) return;
  bool struck = startFlash(false);
  double mean = 15 - 10 * thunder;   // seconds between two lightning flashes
  nextFlash_ = time_ + (struck ? std::clamp(-std::log(std::max(chance(), 1e-4f)) * mean, 0.8, 40.0) : 0.6 + chance());
}

void Renderer::flashNow(bool hold) {
  flash_.hold = false;
  startFlash(true);
  if (hold) {
    flash_.level = flash_.trunk = 1;
    flash_.twig = 1;
    flash_.grow = 2;
    flash_.hold = true;
  }
}

// Loads the selected ground: the strip with the land, if it has one, and the
// waves of its water. If the strip is missing in the program, water remains.
bool Renderer::loadGround() {
  int index = groundOf(par_);
  if (index == groundLoaded_) return true;
  groundLoaded_ = index;
  const Ground& g = GROUNDS[index];
  ID3D11ShaderResourceView* none[5] = {};
  ctx_->PSSetShaderResources(15, 5, none);
  for (auto& t : land_) t.Reset();
  bool whole = g.strip[0] != nullptr;
  for (int i = 0; i < 4 && whole; i++) {
    if (!g.strip[i]) continue;
    // Color with 8 bits, direction and horizon with 16, lights with 8.
    whole = pngTexture(dev_.Get(), g.strip[i], i == 1 || i == 2, i == 0, land_[i]);
  }
  if (!whole) {
    for (auto& t : land_) t.Reset();
  }
  if (g.water) makeWaves();
  return whole;
}

// Wind at the ground in meters per second. It grows faster than the slider: at
// half slider a moderate breeze blows, at full slider a storm.
double Renderer::surfSpeed() const {
  double s = std::clamp(double(par_.wind), 0.0, 1.0);
  return SURF_MAX * s * std::sqrt(s);
}

// The wave trains of the water. Their lengths are distributed evenly over the
// orders of magnitude, from ripples to the longest wave that the strongest wind
// raises. They run with the wind at the ground, short ones spread more widely
// around its direction. The last ones are swell: long, flat waves from far away
// that run obliquely towards the viewer, whatever the wind is. How high the wind
// waves are is set by setSea.
void Renderer::makeWaves() {
  const Ground& g = GROUNDS[groundOf(par_)];
  wave_.assign(size_t(WAVES) * 4, 0.0f);
  waveUnit_.assign(WAVES, 0.0f);
  waveLen_.assign(WAVES, 0.0f);
  uint32_t seed = 0x9e3779b9u;
  auto rnd = [&] {
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return float(seed & 0xffffff) / float(0x1000000);
  };
  auto gauss = [&] { return (rnd() + rnd() + rnd() + rnd() - 2) * 1.732f; };
  const double shortest = 0.07, longest = 1.6 * g.peak[2];
  const double wind = std::atan2(SURF_X, SURF_Z), swell = 2.75;
  const int sea = WAVES - SWELL;
  for (int i = 0; i < WAVES; i++) {
    double lambda, theta;
    if (i < sea) {
      double x = (i + rnd()) / sea;
      lambda = shortest * std::pow(longest / shortest, x);
      theta = wind + gauss() * (0.35 + 0.55 * (1 - x));
    } else {
      lambda = g.peak[1] * (0.75 + 1.25 * (i - sea + rnd()) / SWELL);
      theta = swell + gauss() * 0.12;
    }
    double k = 2 * PI / lambda;
    // The angular frequency rounded so that the wave is the same again after one
    // hour: uTime wraps every hour.
    double omega = std::round(std::sqrt(9.81 * k) * 3600 / (2 * PI)) * 2 * PI / 3600;
    waveUnit_[i] = 0.6f + 0.8f * rnd();
    waveLen_[i] = float(lambda);
    float* w = &wave_[size_t(i) * 4];
    w[0] = i < sea ? 0.0f : float(g.swell * waveUnit_[i] / k);
    w[1] = float(omega);
    w[2] = float(k * std::sin(theta));
    w[3] = float(k * std::cos(theta));
  }
  setSea();
}

// The sea state for the wind slider: how rough the water is overall and how long
// the largest waves are. Longer ones are absent, shorter ones are all equally
// steep (the spectrum of a fully developed sea after Pierson and Moskowitz). The
// wave trains themselves stay, only their height changes: this way nothing jumps
// when adjusting.
void Renderer::setSea() {
  const Ground& g = GROUNDS[groundOf(par_)];
  seaWind_ = par_.wind;
  foamOdds_ = foamCover_ = 0;
  if (!g.water || wave_.empty()) return;
  double s = std::clamp(double(par_.wind), 0.0, 1.0);
  auto along = [&](const float* v) {
    return s < 0.5 ? v[0] * std::pow(double(v[1]) / v[0], 2 * s) : v[1] * std::pow(double(v[2]) / v[1], 2 * s - 1);
  };
  double rough = along(g.rough), peak = along(g.peak);
  const int sea = WAVES - SWELL;
  std::vector<double> part(sea);
  double sum = 0;
  for (int i = 0; i < sea; i++) {
    double x = waveLen_[i] / peak;
    part[i] = waveUnit_[i] * std::exp(-0.625 * x * x);
    sum += 0.5 * part[i] * part[i];
  }
  double steep = std::sqrt(rough / std::max(sum, 1e-12));
  for (int i = 0; i < sea; i++) wave_[size_t(i) * 4] = float(steep * part[i] * waveLen_[i] / (2 * PI));
  ctx_->UpdateSubresource(waveBuf_.Get(), 0, nullptr, wave_.data(), 0, 0);
  // Whitecaps: fraction of the water surface that they cover, from measurements at
  // sea (Callaghan and others 2008). Below 3.7 m/s no wave breaks.
  double u = surfSpeed() * g.shelter;
  double cover = u <= 3.7 ? 0 : (u < 10 ? 3.18e-5 * std::pow(u - 3.7, 3) : 4.82e-6 * std::pow(u + 1.98, 3));
  // A wave breaks this often in a cell of the water so that this fraction results
  // (see foamCell in ground.hlsli); full: the fraction when every one breaks.
  double full = FOAM_FULL * std::pow(u / 17, 0.87);
  foamOdds_ = cover > 0 ? float(std::min(1.0, cover / full)) : 0.0f;
  foamCover_ = float(full);
}

// The rain of this moment, averaged with the previous image. fresh: without the
// previous one, after a user change.
void Renderer::drawRain(bool fresh) {
  if (skyOf(par_.clouds).rain <= 0.001f) return;
  ID3D11ShaderResourceView* none[1] = {};
  ctx_->PSSetShaderResources(20, 1, none);
  int next = 1 - rainCur_;
  SkyC c;
  for (int round = 0; round < (fresh ? 6 : 1); round++) {
    target(rain_[next].rtv.Get(), rain_[next].w, rain_[next].h);
    fillSky(c, rain_[next].w, rain_[next].h, 0, float(cloudT_), 5.588238f * float(rainShot_++ % 64), 0, 0, 0);
    c.share = fresh && round == 0 ? 1.0f : 0.25f;
    ctx_->UpdateSubresource(skyBuf_.Get(), 0, nullptr, &c, 0, 0);
    ctx_->PSSetShaderResources(0, 1, noise_.GetAddressOf());
    ID3D11ShaderResourceView* clouds[4] = {shape_.Get(), detail_.Get(), weather_.Get(), turb_.Get()};
    ctx_->PSSetShaderResources(5, 4, clouds);
    ctx_->PSSetShaderResources(20, 1, rain_[rainCur_].srv.GetAddressOf());
    quad(psRain_.Get());
    target(nullptr, 1, 1);
    ctx_->PSSetShaderResources(20, 1, none);
    rainCur_ = next;
    next = 1 - rainCur_;
  }
}

// The textures of the ground. mirror: plus the image that is reflected in the
// water.
void Renderer::bindGround(bool mirror) {
  ID3D11ShaderResourceView* in[5] = {land_[0].Get(), land_[1].Get(), land_[2].Get(), land_[3].Get(), mirror ? upper_.srv.Get() : nullptr};
  ctx_->PSSetShaderResources(15, 5, in);
  // Shape and weather map of the clouds: for their shadow on the land.
  ID3D11ShaderResourceView* clouds[4] = {shape_.Get(), detail_.Get(), weather_.Get(), turb_.Get()};
  ctx_->PSSetShaderResources(5, 4, clouds);
  if (seaWind_ != par_.wind) setSea();
  ctx_->PSSetConstantBuffers(1, 1, waveBuf_.GetAddressOf());
  ctx_->PSSetShaderResources(20, 1, rain_[rainCur_].srv.GetAddressOf());
  ID3D11ShaderResourceView* bolt[2] = {bolt_.srv.Get(), boltFar_.srv.Get()};
  ctx_->PSSetShaderResources(21, 2, bolt);
  ctx_->PSSetConstantBuffers(2, 1, boltBuf_.GetAddressOf());
  ID3D11ShaderResourceView* water[2] = {mirror ? shade_.srv.Get() : nullptr, mirror ? column_.srv.Get() : nullptr};
  ctx_->PSSetShaderResources(23, 2, water);
}

// What the water needs besides the mirror image: the summed strip above the
// horizon (sums) and the shadows of the clouds on it (shade).
void Renderer::drawWaterMaps(bool sums, bool shade) {
  ID3D11ShaderResourceView* none[2] = {};
  ctx_->PSSetShaderResources(23, 2, none);
  SkyC c;
  if (sums) {
    target(columnBlock_.rtv.Get(), column_.w, column_.h);
    fillSky(c, column_.w, column_.h, 0, float(cloudT_), 0, 0, 0, 0);
    ctx_->PSSetShaderResources(19, 1, upper_.srv.GetAddressOf());
    quad(psColumnBlock_.Get());
    target(column_.rtv.Get(), column_.w, column_.h);
    ctx_->PSSetShaderResources(19, 1, upper_.srv.GetAddressOf());
    ctx_->PSSetShaderResources(24, 1, columnBlock_.srv.GetAddressOf());
    quad(psColumn_.Get());
    target(nullptr, 1, 1);
    ctx_->PSSetShaderResources(19, 1, none);
    ctx_->PSSetShaderResources(24, 1, none);
    ctx_->GenerateMips(column_.srv.Get());
  }
  if (shade) {
    target(shade_.rtv.Get(), shade_.w, shade_.h);
    fillSky(c, shade_.w, shade_.h, 0, float(cloudT_), 0, 0, 0, 0);
    ctx_->PSSetShaderResources(0, 1, noise_.GetAddressOf());
    ID3D11ShaderResourceView* clouds[4] = {shape_.Get(), detail_.Get(), weather_.Get(), turb_.Get()};
    ctx_->PSSetShaderResources(5, 4, clouds);
    quad(psShade_.Get());
    target(nullptr, 1, 1);
  }
}

// A model from the resources: its mesh and its surface colors. The colors are
// one image with the layers one below the other, each as high as it is wide.
bool Renderer::loadMesh(int model) {
  Mesh& m = mesh_[model];
  const uint8_t* data;
  size_t size;
  MeshHead head;
  if (!resource(MODELS[model].mesh, data, size) || size < sizeof(head)) return false;
  memcpy(&head, data, sizeof(head));
  size_t vertexBytes = size_t(head.vertices) * MESH_VERTEX, indexBytes = size_t(head.indices) * 4, lampBytes = size_t(head.lamps) * sizeof(Lamp);
  if (memcmp(head.magic, "SBM2", 4) || !head.vertices || !head.indices || !head.layers ||
      size < sizeof(head) + vertexBytes + indexBytes + lampBytes) {
    return false;
  }
  m.lamps.resize(head.lamps);
  if (head.lamps) memcpy(m.lamps.data(), data + sizeof(head) + vertexBytes + indexBytes, lampBytes);
  D3D11_BUFFER_DESC b = {};
  b.Usage = D3D11_USAGE_IMMUTABLE;
  b.ByteWidth = UINT(vertexBytes);
  b.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA from = {data + sizeof(head), 0, 0};
  if (FAILED(dev_->CreateBuffer(&b, &from, &m.vertices))) return false;
  b.ByteWidth = UINT(indexBytes);
  b.BindFlags = D3D11_BIND_INDEX_BUFFER;
  from.pSysMem = data + sizeof(head) + vertexBytes;
  if (FAILED(dev_->CreateBuffer(&b, &from, &m.indices))) return false;

  UINT w = 0, h = 0;
  std::vector<uint8_t> px;
  if (!pngPixels(MODELS[model].colors, false, w, h, px) || h != w * head.layers) return false;
  UINT levels = 1;
  while ((w >> levels) > 0) levels++;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = d.Height = w;
  d.MipLevels = levels;
  d.ArraySize = head.layers;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ComPtr<ID3D11Texture2D> tex;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &tex))) return false;
  for (UINT i = 0; i < head.layers; i++) {
    ctx_->UpdateSubresource(tex.Get(), D3D11CalcSubresource(0, i, levels), nullptr, px.data() + size_t(i) * w * w * 4, w * 4, 0);
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
  v.Format = d.Format;
  v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  v.Texture2DArray.MipLevels = levels;
  v.Texture2DArray.ArraySize = head.layers;
  if (FAILED(dev_->CreateShaderResourceView(tex.Get(), &v, &m.colors))) return false;
  ctx_->GenerateMips(m.colors.Get());
  m.count = head.indices;
  for (int i = 0; i < 3; i++) m.lo[i] = head.lo[i], m.hi[i] = head.hi[i], m.wing[i] = head.wing[i];
  return true;
}

// The work image for the models, made when the first one appears: with as many
// samples per pixel as the graphics card offers, up to eight.
bool Renderer::allocProps() {
  if (propSamples_ != 0) return propSamples_ > 0;
  propSamples_ = -1;
  UINT count = 8;
  for (; count >= 2; count /= 2) {
    UINT color = 0, depth = 0;
    if (SUCCEEDED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, count, &color)) && color > 0 &&
        SUCCEEDED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, count, &depth)) && depth > 0) {
      break;
    }
  }
  if (count < 2) return false;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = PROP_W;
  d.Height = PROP_H;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  d.SampleDesc.Count = count;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &propColor_)) || FAILED(dev_->CreateRenderTargetView(propColor_.Get(), nullptr, &propRtv_)) ||
      FAILED(dev_->CreateShaderResourceView(propColor_.Get(), nullptr, &propSrv_))) {
    return false;
  }
  d.Format = DXGI_FORMAT_D32_FLOAT;
  d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &propDepth_)) || FAILED(dev_->CreateDepthStencilView(propDepth_.Get(), nullptr, &propDsv_))) return false;
  propSamples_ = int(count);
  return true;
}

// Ships, aircraft and birds, dt seconds later. Returns whether the image for the
// eye has to show them or, one last time, to remove them.
bool Renderer::updateLife(double dt) {
  int index = groundOf(par_);
  const Ground& g = GROUNDS[index];
  Surroundings s;
  s.ground = index;
  s.water = g.water;
  s.eye = g.eyeWater;
  s.wind = surfSpeed() * (g.water ? g.shelter : 1.0f);
  s.windX = SURF_X, s.windZ = SURF_Z;
  s.night = light_.night;
  s.rain = skyOf(par_.clouds).rain;
  s.fog = par_.fog;
  s.temperature = par_.temperature;
  s.halfWidth = 0.5 * viewW_ / focal();
  s.top = horizon() / focal();
  s.amount = par_.life;
  for (int i = 0; i < MODEL_COUNT; i++) {
    const Mesh& m = mesh_[i];
    s.has[i] = m.vertices != nullptr;
    s.length[i] = std::max({m.hi[0] - m.lo[0], m.hi[1] - m.lo[1], m.hi[2] - m.lo[2]});
  }
  bool was = !props_.empty();
  props_ = life_.update(dt, s);
  return was || !props_.empty();
}

// Whether they have moved visibly since the image for the light was last made.
bool Renderer::propsShifted() const {
  if (props_.size() != lightProps_.size()) return true;
  for (size_t i = 0; i < props_.size(); i++) {
    const Prop& a = props_[i];
    const Prop& b = lightProps_[i];
    if (a.model != b.model || std::abs(a.wing - b.wing) > 0.05f) return true;
    double dx = a.pos[0] / a.pos[2] - b.pos[0] / b.pos[2], dy = a.pos[1] / a.pos[2] - b.pos[1] / b.pos[2];
    if (std::hypot(dx, dy) * focal() > 0.6) return true;
  }
  return false;
}

// Sets what moves through the image into a finished image: the target is rtv,
// its values (SkyC) are set and the textures of sky and ground are bound. Each
// model is first computed alone in the work image, then laid over the target.
void Renderer::drawProps(Into into, ID3D11RenderTargetView* rtv, int w, int h) {
  if (props_.empty() || !rtv || !allocProps()) return;
  const Ground& g = GROUNDS[groundOf(par_)];
  // The water mirrors a point as if it were seen from an eye that is as far below
  // the water as the real one is above it. The water out there is tilted with the
  // Earth, which triples the effect of the curvature. The image that is mirrored
  // shows the models that way; for everything far away it makes no difference.
  const bool mirror = into == Into::MIRROR;
  const double eye = g.eyeWater, lift = mirror ? eye : -eye, curve = mirror ? 3 : 1;
  const double f = focal(), hz = horizon(), density = double(w) / viewW_;
  ID3D11ShaderResourceView* none[1] = {};
  if (g.water) ctx_->PSSetShaderResources(23, 1, shade_.srv.GetAddressOf());
  ctx_->VSSetConstantBuffers(0, 1, skyBuf_.GetAddressOf());
  ctx_->VSSetConstantBuffers(3, 1, propBuf_.GetAddressOf());
  ctx_->PSSetConstantBuffers(3, 1, propBuf_.GetAddressOf());
  const D3D11_VIEWPORT whole = {0, 0, float(w), float(h), 0, 1};
  for (const Prop& p : props_) {
    const Mesh& m = mesh_[p.model];
    const Model& model = MODELS[p.model];
    if (!m.vertices) continue;
    // In the image for the light everything stands for something far away that
    // casts long shafts into the haze, like the skyline. A ship at the horizon may
    // do that, a bird or an aircraft crossing the sky may not.
    if (into == Into::LIGHT && !model.afloat) continue;
    // Its rotation: first the bank around its length, then the nose up, then the
    // heading.
    double cb = std::cos(p.bank), sb = std::sin(p.bank), cp = std::cos(p.pitch), sp = std::sin(p.pitch);
    double ch = std::cos(p.heading), sh = std::sin(p.heading);
    double rot[3][3];
    for (int j = 0; j < 3; j++) {
      double x = (j == 0) * cb + (j == 1) * sb, y = -(j == 0) * sb + (j == 1) * cb, z = j == 2;
      double up = y * cp + z * sp, ahead = -y * sp + z * cp;
      rot[0][j] = x * ch + ahead * sh;
      rot[1][j] = up;
      rot[2][j] = -x * sh + ahead * ch;
    }
    // Its rectangle in the image, from the corners of its extent. Beating wings
    // reach above and below it.
    double lo[3] = {m.lo[0], m.lo[1] - m.wing[1], m.lo[2]}, hi[3] = {m.hi[0], m.hi[1] + m.wing[1], m.hi[2]};
    if (p.mirrored) lo[0] = -m.hi[0], hi[0] = -m.lo[0];
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, zNear = 1e12, zFar = 0;
    for (int k = 0; k < 8; k++) {
      double c[3] = {k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]}, at[3];
      for (int i = 0; i < 3; i++) at[i] = rot[i][0] * c[0] + rot[i][1] * c[1] + rot[i][2] * c[2] + p.pos[i];
      zNear = std::min(zNear, at[2]);
      zFar = std::max(zFar, at[2]);
      if (at[2] < 2) continue;
      at[1] += lift - curve * (at[0] * at[0] + at[2] * at[2]) / (2 * R_EFF);
      double sx = 0.5 * viewW_ + at[0] / at[2] * f, sy = hz - at[1] / at[2] * f;
      x0 = std::min(x0, sx), x1 = std::max(x1, sx);
      y0 = std::min(y0, sy), y1 = std::max(y1, sy);
    }
    if (zNear < 2) continue;
    D3D11_RECT box = {LONG(std::max(0.0, std::floor((x0 - 1.5) * density))), LONG(std::max(0.0, std::floor((y0 - 1.5) * density))),
                      LONG(std::min(double(w), std::ceil((x1 + 1.5) * density))), LONG(std::min(double(h), std::ceil((y1 + 1.5) * density)))};
    int bw = box.right - box.left, bh = box.bottom - box.top;
    if (bw <= 0 || bh <= 0) continue;
    // Pixels of the work image per pixel of the target and axis: a whole number as
    // long as it is small enough. Nothing is meant to come so close that the work
    // image would not hold it; if it does, it is computed more coarsely.
    double fine = std::min(double(PROP_W) / bw, double(PROP_H) / bh);
    fine = fine >= 1 ? std::min(std::floor(fine), double(PROP_FINE)) : fine;

    PropC c = {};
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) c.row[i][j] = float(rot[i][j]);
      c.row[i][3] = float(p.pos[i]);
    }
    c.rect[0] = float(box.left / density), c.rect[1] = float(box.top / density);
    c.rect[2] = float(box.right / density), c.rect[3] = float(box.bottom / density);
    double pad = 0.1 * (zFar - zNear) + m.wing[1] + 0.5;
    c.depth[0] = float(std::max(zNear - pad, 1.0)), c.depth[1] = float(zFar + pad);
    c.depth[2] = float(lift), c.depth[3] = float(curve);
    c.work[0] = float(box.left), c.work[1] = float(box.top), c.work[2] = float(fine), c.work[3] = float(propSamples_);
    double flat = p.pos[0] * p.pos[0] + p.pos[2] * p.pos[2];
    c.air[0] = float(std::sqrt(flat + (p.pos[1] - eye) * (p.pos[1] - eye)));
    c.air[1] = float(g.eyeSea - g.eyeWater + p.pos[1]);
    c.air[2] = model.afloat ? 1.0f : 0.0f;
    c.air[3] = model.gloss;
    // The shadow of the clouds: for what floats, as on the water at its water line
    // where the eye sees it; above the clouds none; otherwise as at the viewer.
    c.light[0] = float((0.5 * viewW_ + p.pos[0] / p.pos[2] * f) / viewW_);
    c.light[1] = float((hz + (eye + flat / (2 * R_EFF)) / p.pos[2] * f) / viewH_);
    c.light[2] = model.afloat && g.water ? 1.0f : p.pos[1] - eye > 2200 + g.cloudLift ? 2.0f : 0.0f;
    c.light[3] = g.water ? 0.12f : 0.22f;
    c.wing[0] = p.wing, c.wing[1] = p.tip, c.wing[2] = m.wing[0], c.wing[3] = m.wing[1];
    c.axis[0] = m.wing[2], c.axis[1] = p.mirrored ? 1.0f : 0.0f;
    ctx_->UpdateSubresource(propBuf_.Get(), 0, nullptr, &c, 0, 0);

    // The model alone.
    const float clear[4] = {};
    const D3D11_VIEWPORT part = {0, 0, float(bw * fine), float(bh * fine), 0, 1};
    const UINT stride = MESH_VERTEX, offset = 0;
    ctx_->PSSetShaderResources(26, 1, none);
    ctx_->OMSetRenderTargets(1, propRtv_.GetAddressOf(), propDsv_.Get());
    ctx_->ClearRenderTargetView(propRtv_.Get(), clear);
    ctx_->ClearDepthStencilView(propDsv_.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    ctx_->RSSetViewports(1, &part);
    ctx_->RSSetState(raster_.Get());
    ctx_->OMSetBlendState(blendOff_.Get(), nullptr, 0xffffffff);
    ctx_->IASetInputLayout(propLayout_.Get());
    ctx_->IASetVertexBuffers(0, 1, m.vertices.GetAddressOf(), &stride, &offset);
    ctx_->IASetIndexBuffer(m.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx_->VSSetShader(vsProp_.Get(), nullptr, 0);
    ctx_->PSSetShader(psPropIn_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(25, 1, m.colors.GetAddressOf());
    ctx_->DrawIndexed(m.count, 0, 0);

    // Laid over the target.
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &whole);
    ctx_->RSSetState(rasterCut_.Get());
    ctx_->RSSetScissorRects(1, &box);
    ctx_->OMSetBlendState(into == Into::EYE ? blendOverKeep_.Get() : blendOver_.Get(), nullptr, 0xffffffff);
    ctx_->IASetInputLayout(nullptr);
    ctx_->PSSetShaderResources(26, 1, propSrv_.GetAddressOf());
    quad(psProp_.Get());
    ctx_->PSSetShaderResources(26, 1, none);

    // Its lamps, once it is dark enough for them: each a point of light whose
    // brightness falls with the square of the distance. One that would be brighter
    // than the image can show gets a larger disc instead.
    if (light_.lights <= 0.01f || into == Into::LIGHT) continue;
    ctx_->OMSetBlendState(into == Into::EYE ? blendOverKeep_.Get() : blendAdd_.Get(), nullptr, 0xffffffff);
    for (const Lamp& lamp : m.lamps) {
      double at[3], away[3], from[3];
      for (int i = 0; i < 3; i++) at[i] = rot[i][0] * (p.mirrored ? -lamp.pos[0] : lamp.pos[0]) + rot[i][1] * lamp.pos[1] + rot[i][2] * lamp.pos[2] + p.pos[i];
      if (at[2] < 2) continue;
      // Does it shine toward the eye? The direction to the eye in the frame of the
      // model against the axis of its sector.
      away[0] = -at[0], away[1] = eye - at[1], away[2] = -at[2];
      double range = std::sqrt(away[0] * away[0] + away[1] * away[1] + away[2] * away[2]);
      for (int j = 0; j < 3; j++) from[j] = (rot[0][j] * away[0] + rot[1][j] * away[1] + rot[2][j] * away[2]) / range;
      if (p.mirrored) from[0] = -from[0];
      double seen = lamp.cosHalf < -1 ? 1 : smooth(lamp.cosHalf - 0.04, lamp.cosHalf + 0.04, from[0] * lamp.axis[0] + from[1] * lamp.axis[1] + from[2] * lamp.axis[2]);
      if (lamp.period > 0 && std::fmod(time_ / lamp.period + lamp.phase, 1.0) > lamp.lit) seen = 0;
      if (seen <= 0) continue;
      double level = at[1] + lift - curve * (at[0] * at[0] + at[2] * at[2]) / (2 * R_EFF);
      double sx = (0.5 * viewW_ + at[0] / at[2] * f) * density, sy = (hz - level / at[2] * f) * density;
      double peak = LAMP_GAIN * seen * light_.lights / (range * range * 1e-6), radius = std::max(1.2 * density, 0.9);
      if (peak > LAMP_PEAK) {
        radius *= std::min(std::sqrt(peak / LAMP_PEAK), 3.0);
        peak = LAMP_PEAK;
      }
      D3D11_RECT spot = {LONG(std::max(0.0, std::floor(sx - 3 * radius))), LONG(std::max(0.0, std::floor(sy - 3 * radius))),
                         LONG(std::min(double(w), std::ceil(sx + 3 * radius))), LONG(std::min(double(h), std::ceil(sy + 3 * radius)))};
      if (spot.right <= spot.left || spot.bottom <= spot.top) continue;
      c.rect[0] = float(sx), c.rect[1] = float(sy), c.rect[2] = float(radius);
      for (int i = 0; i < 3; i++) c.wing[i] = float(lamp.color[i] * peak);
      c.air[0] = float(range);
      c.air[1] = float(g.eyeSea - g.eyeWater + at[1]);
      ctx_->UpdateSubresource(propBuf_.Get(), 0, nullptr, &c, 0, 0);
      ctx_->RSSetScissorRects(1, &spot);
      quad(psLamp_.Get());
    }
  }
  ctx_->RSSetState(raster_.Get());
  ctx_->OMSetBlendState(blendOff_.Get(), nullptr, 0xffffffff);
}

// Sky and land without water: the image that is reflected in the water.
void Renderer::faceUpper() {
  ID3D11ShaderResourceView* none[1] = {};
  ctx_->PSSetShaderResources(19, 1, none);
  SkyC c;
  target(upper_.rtv.Get(), upper_.w, upper_.h);
  fillSky(c, upper_.w, upper_.h, 0, float(cloudT_), 0, 0, 0, 0);
  c.pass = 1;
  ctx_->UpdateSubresource(skyBuf_.Get(), 0, nullptr, &c, 0, 0);
  bindClouds();
  bindGround(false);
  quad(psFace_.Get());
  drawProps(Into::MIRROR, upper_.rtv.Get(), upper_.w, upper_.h);
  target(nullptr, 1, 1);
  ctx_->GenerateMips(upper_.srv.Get());
}

// From sky and clouds the finished image for the light. The clouds are where the
// wind has carried them since their pass.
void Renderer::faceForLight() {
  loadGround();
  bool water = GROUNDS[groundOf(par_)].water;
  if (water) {
    faceUpper();
    // Both change slowly: after a user change both, afterwards alternately.
    bool both = mapTick_ < 2;
    drawWaterMaps(both || mapTick_ % 2 == 0, both || mapTick_ % 2 == 1);
    mapTick_++;
  }
  SkyC c;
  target(sky_.rtv.Get(), sky_.w, sky_.h);
  fillSky(c, sky_.w, sky_.h, 0, float(cloudT_), 0, 0, 0, 0);
  bindClouds();
  bindGround(water);
  quad(psFace_.Get());
  drawProps(Into::LIGHT, sky_.rtv.Get(), sky_.w, sky_.h);
  lightProps_ = props_;
  lightTime_ = time_;
  target(nullptr, 1, 1);
  ctx_->GenerateMips(sky_.srv.Get());
}

// The finished sky image for the eye, at full sharpness. It is made in an image
// of its own that persists, and is copied from there to the target: this way,
// between two full frames, only the strip with the water can be made anew
// (Redo::WATER) or nothing at all (Redo::NONE), the sky above it stays as it
// was. What moves through the image is set into the copy every time, where it is
// at that moment.
void Renderer::faceForEye(ID3D11RenderTargetView* skyOut, Redo redo) {
  if (!skyOut) return;
  loadGround();
  ComPtr<ID3D11Resource> out;
  skyOut->GetResource(&out);
  ComPtr<ID3D11Texture2D> outTex;
  if (FAILED(out.As(&outTex))) return;
  D3D11_TEXTURE2D_DESC d;
  outTex->GetDesc(&d);
  if (!eye_.tex || eye_.w != int(d.Width) || eye_.h != int(d.Height) || eyeFormat_ != d.Format) {
    eye_ = makeTarget(int(d.Width), int(d.Height), d.Format, false);
    eyeFormat_ = d.Format;
    redo = Redo::ALL;
  }
  const Ground& g = GROUNDS[groundOf(par_)];
  SkyC c;
  if (redo != Redo::NONE) {
    target(eye_.rtv.Get(), pixW_, pixH_);
    if (redo == Redo::WATER) {
      // From the sea horizon downwards; one row more, its edge is soft.
      double dip = std::sqrt(2.0 * g.eyeWater / R_EFF);
      LONG top = LONG(std::clamp(std::floor((horizon() + focal() * dip - 1.5) * scale_) - 1.0, 0.0, double(pixH_ - 1)));
      D3D11_RECT cut = {0, top, pixW_, pixH_};
      ctx_->RSSetState(rasterCut_.Get());
      ctx_->RSSetScissorRects(1, &cut);
    }
    fillSky(c, pixW_, pixH_, 0, float(cloudT_), 0, 0, 0, 1);
    bindClouds();
    bindGround(g.water);
    quad(psFace_.Get());
    target(nullptr, 1, 1);
  }
  ctx_->CopyResource(out.Get(), eye_.tex.Get());
  if (props_.empty()) return;
  target(skyOut, pixW_, pixH_);
  fillSky(c, pixW_, pixH_, 0, float(cloudT_), 0, 0, 0, 1);
  bindClouds();
  bindGround(g.water);
  drawProps(Into::EYE, skyOut, pixW_, pixH_);
  target(nullptr, 1, 1);
}

// The next band of the running pass. Returns whether the pass is finished with
// it; then the new cloud image applies, and the previous one stays for
// cross-fading.
bool Renderer::stepBand() {
  airBand();
  if (pass_.band < AIR_BANDS) return false;
  drawNear(airWork_);
  airShiftOf_[airWork_] = pass_.shift;
  int oldest = airOld_;
  airOld_ = airNew_;
  airNew_ = airWork_;
  airWork_ = oldest;
  airDone_++;
  sinceSwitch_ = 0;
  passActive_ = false;
  skyAccum_ = std::min(skyAccum_ + 1, SKY_ACC);
  skyShot_++;
  return true;
}

bool Renderer::dumpDepth(std::vector<uint8_t>& bgra, int& w, int& h) {
  auto read = [&](const Target& t, int channels, std::vector<float>& out) {
    if (!t.tex) return false;
    D3D11_TEXTURE2D_DESC d;
    t.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &copy))) return false;
    ctx_->CopyResource(copy.Get(), t.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize(size_t(t.w) * t.h);
    for (int y = 0; y < t.h; y++) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
      for (int x = 0; x < t.w; x++) {
        // Half float unpacked by hand.
        uint16_t v = row[size_t(x) * channels];
        int e = (v >> 10) & 31, f = v & 1023;
        out[size_t(y) * t.w + x] = e == 0 ? std::ldexp(float(f), -24) : e == 31 ? 65504.0f : std::ldexp(float(f + 1024), e - 25);
      }
    }
    ctx_->Unmap(copy.Get(), 0);
    return true;
  };
  std::vector<float> z, zn;
  const Target& full = airZ_[airNew_];
  const Target& wide = near_[airNew_];
  if (!read(full, 4, z) || !read(wide, 1, zn)) return false;
  w = full.w;
  h = full.h;
  bgra.assign(size_t(w) * h * 4, 255);
  auto shade = [](float km) { return km > 0 ? uint8_t(80 + 175 * (1 - std::min(km / 15.0f, 1.0f))) : uint8_t(0); };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t* p = &bgra[(size_t(y) * w + x) * 4];
      p[0] = shade(z[size_t(y) * w + x]);
      p[1] = shade(zn[size_t(std::min(y / 4, wide.h - 1)) * wide.w + std::min(x / 4, wide.w - 1)]);
      p[2] = 0;
    }
  }
  return true;
}

bool Renderer::skySettled() const { return !skyDirty_ && skyAccum_ >= SKY_ACC; }

// For measurements: a whole cloud pass and afterwards the two finished images,
// time per step in milliseconds.
std::vector<double> Renderer::timeSteps(ID3D11RenderTargetView* skyOut) {
  std::vector<double> out;
  auto timed = [&](auto&& step) {
    sync();
    auto a = std::chrono::steady_clock::now();
    step();
    sync();
    out.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
  };
  updateWeather(0, true);
  beginPass();
  for (bool done = false; !done;) timed([&] { done = stepBand(); });
  drawRain(true);
  timed([&] { faceForLight(); });
  // The image for the eye several times, the median counts: a single run varies by
  // milliseconds.
  std::vector<double> eye;
  for (int i = 0; i < 15; i++) {
    timed([&] { faceForEye(skyOut); });
    eye.push_back(out.back());
    out.pop_back();
  }
  std::sort(eye.begin(), eye.end());
  out.push_back(eye[eye.size() / 2]);
  return out;
}

void Renderer::uploadIcons() {
  if (icons_.w != iconW_ || icons_.h != iconH_) icons_ = makeTarget(iconW_, iconH_, DXGI_FORMAT_R8_UNORM, true);
  ctx_->UpdateSubresource(icons_.tex.Get(), 0, nullptr, iconData_.data(), iconW_, 0);
  ctx_->GenerateMips(icons_.srv.Get());
}

void Renderer::drawBloom() {
  LightC c = {};
  target(bloom_[0].rtv.Get(), bloom_[0].w, bloom_[0].h);
  c.res[0] = float(bloom_[0].w), c.res[1] = float(bloom_[0].h);
  c.threshold = 2.2f;
  c.rim = par_.rim;
  fillLight(c);
  ID3D11ShaderResourceView* in[2] = {sky_.srv.Get(), icons_.srv.Get()};
  ctx_->PSSetShaderResources(0, 2, in);
  quad(psBloomPre_.Get());

  for (size_t i = 1; i < bloom_.size(); i++) {
    target(bloom_[i].rtv.Get(), bloom_[i].w, bloom_[i].h);
    c.res[0] = float(bloom_[i].w), c.res[1] = float(bloom_[i].h);
    c.srcTexel[0] = 1.0f / bloom_[i - 1].w, c.srcTexel[1] = 1.0f / bloom_[i - 1].h;
    fillLight(c);
    ctx_->PSSetShaderResources(2, 1, bloom_[i - 1].srv.GetAddressOf());
    quad(psBloomDown_.Get());
  }

  // Up again, every level adds the softer one below it.
  c.weight = 1;
  for (int i = int(bloom_.size()) - 2; i >= bloomBase_; i--) {
    target(bloom_[i].rtv.Get(), bloom_[i].w, bloom_[i].h);
    ctx_->OMSetBlendState(blendAdd_.Get(), nullptr, 0xffffffff);
    c.res[0] = float(bloom_[i].w), c.res[1] = float(bloom_[i].h);
    c.srcTexel[0] = 1.0f / bloom_[i + 1].w, c.srcTexel[1] = 1.0f / bloom_[i + 1].h;
    fillLight(c);
    ctx_->PSSetShaderResources(2, 1, bloom_[i + 1].srv.GetAddressOf());
    quad(psBloomUp_.Get());
  }
}

void Renderer::drawScatter() {
  Target& t = scat_[cur_];
  target(t.rtv.Get(), t.w, t.h);
  LightC c = {};
  c.res[0] = float(t.w), c.res[1] = float(t.h);
  c.sigma = par_.density / zMax_;
  c.jitter = float(std::fmod(shot_++ * 0.6180339887, 1.0));
  c.alpha = 1.0f / (std::min(accum_, ACC_FRAMES - 1) + 1);
  c.steps = par_.steps;
  c.mist = par_.mist;
  // The patches of mist are as large as a good part of the screen, stretched in
  // depth, and drift slowly sideways and through the depth.
  c.mistScale[0] = c.mistScale[1] = 1 / (1.17f * viewW_);
  c.mistScale[2] = 1 / (4.06f * viewW_);
  c.mistOff[0] = float(std::fmod(-time_ * 0.0032, 1.0));
  c.mistOff[1] = float(std::fmod(time_ * 0.0011, 1.0));
  c.mistOff[2] = float(std::fmod(time_ * 0.0045, 1.0));
  fillLight(c);
  ID3D11ShaderResourceView* in[5] = {sky_.srv.Get(), icons_.srv.Get(), scat_[1 - cur_].srv.Get(), nullptr, noise_.Get()};
  ctx_->PSSetShaderResources(0, 5, in);
  quad(psScatter_.Get());
  cur_ = 1 - cur_;
}

void Renderer::drawComposite(ID3D11RenderTargetView* lightOut) {
  if (!lightOut) return;
  Target& t = scat_[1 - cur_];
  target(lightOut, pixW_, pixH_);
  LightC c = {};
  c.res[0] = float(pixW_), c.res[1] = float(pixH_);
  c.lowRes[0] = float(t.w), c.lowRes[1] = float(t.h);
  c.exposure = par_.exposure;
  c.overWin = par_.overWindows;
  c.overSky = par_.overSky;
  c.shade = par_.shade;
  c.bloom = par_.bloom * 0.22f;
  c.rim = par_.rim;
  c.time = float(time_);
  c.dpr = scale_;
  fillLight(c);
  ID3D11ShaderResourceView* in[4] = {sky_.srv.Get(), icons_.srv.Get(), t.srv.Get(),
                                     bloom_[std::min(bloomBase_, int(bloom_.size()) - 1)].srv.Get()};
  ctx_->PSSetShaderResources(0, 4, in);
  ctx_->VSSetShaderResources(0, 2, in);
  ctx_->PSSetShaderResources(5, 1, cursor_.srv.GetAddressOf());
  quad(psComposite_.Get());

  // Equally dense dust on every display.
  int n = std::min(6000, int(std::lround(par_.dust * DUST_PER_MP * viewW_ * viewH_ / 1e6)));
  if (n > 0) {
    ctx_->OMSetBlendState(blendAdd_.Get(), nullptr, 0xffffffff);
    ctx_->VSSetShader(vsDust_.Get(), nullptr, 0);
    ctx_->PSSetShader(psDust_.Get(), nullptr, 0);
    ctx_->Draw(n * 6, 0);
  }
}

// The light of the lightning over the desktop: its scattered light with the
// shadows of windows and icons, averaged over the frames of one flash (its place
// stays the same, only the brightness flickers), then added to the light image.
void Renderer::drawFlashLight(ID3D11RenderTargetView* lightOut) {
  if (!lightOut) return;
  if (flashLit_ != flash_.start) {
    flashLit_ = flash_.start;
    flashAccum_ = 0;
  }
  flashPass_ = true;
  Target& t = scatFlash_[flashCur_];
  target(t.rtv.Get(), t.w, t.h);
  LightC c = {};
  c.res[0] = float(t.w), c.res[1] = float(t.h);
  c.sigma = par_.density / zMax_;
  c.jitter = float(std::fmod(shot_++ * 0.6180339887, 1.0));
  c.alpha = 1.0f / (std::min(flashAccum_, ACC_FRAMES - 1) + 1);
  c.steps = par_.steps;
  c.mist = par_.mist;
  c.mistScale[0] = c.mistScale[1] = 1 / (1.17f * viewW_);
  c.mistScale[2] = 1 / (4.06f * viewW_);
  c.mistOff[0] = float(std::fmod(-time_ * 0.0032, 1.0));
  c.mistOff[1] = float(std::fmod(time_ * 0.0011, 1.0));
  c.mistOff[2] = float(std::fmod(time_ * 0.0045, 1.0));
  fillLight(c);
  ID3D11ShaderResourceView* in[5] = {sky_.srv.Get(), icons_.srv.Get(), scatFlash_[1 - flashCur_].srv.Get(), nullptr, noise_.Get()};
  ctx_->PSSetShaderResources(0, 5, in);
  quad(psScatter_.Get());
  flashCur_ = 1 - flashCur_;
  flashAccum_++;

  Target& done = scatFlash_[1 - flashCur_];
  target(lightOut, pixW_, pixH_);
  ctx_->OMSetBlendState(blendAdd_.Get(), nullptr, 0xffffffff);
  LightC k = {};
  k.res[0] = float(pixW_), k.res[1] = float(pixH_);
  k.lowRes[0] = float(done.w), k.lowRes[1] = float(done.h);
  k.exposure = par_.exposure;
  k.overWin = par_.overWindows;
  k.overSky = par_.overSky;
  k.rim = par_.rim;
  k.time = float(time_);
  k.dpr = scale_;
  fillLight(k);
  ID3D11ShaderResourceView* use[3] = {sky_.srv.Get(), icons_.srv.Get(), done.srv.Get()};
  ctx_->PSSetShaderResources(0, 3, use);
  ctx_->PSSetShaderResources(5, 1, cursor_.srv.GetAddressOf());
  quad(psFlash_.Get());
  flashPass_ = false;
}

Renderer::Drawn Renderer::render(double dt, ID3D11RenderTargetView* skyOut, ID3D11RenderTargetView* lightOut) {
  Drawn out;
  bindCommon();
  time_ += dt;
  // The gust fields move over the water with the wind at the ground. Their path
  // wraps without anything jumping: 5000 m are three tiles of the noise.
  gustT_ = std::fmod(gustT_ + dt * surfSpeed() * 0.8, 5000.0);

  // Water moves fast enough that every display refresh is visible. Everything else
  // in the image does not need that: if the image is calm, only the strip with the
  // water is made anew between two full frames. Clouds, light and weather catch up
  // on the elapsed time in the next full frame.
  owed_ += dt;
  // Ships, aircraft and birds move on with every frame.
  bool alive = updateLife(dt);
  bool quiet = calm_ && !skyDirty_ && !sceneDirty_ && !iconDirty_ && !cursorMoved_ && !heavenMoved_ && !faceDue_ && !boltDue_ &&
               flash_.level <= 0.001f && eye_.tex;
  if (quiet && skyOut && time_ - worldTime_ < WORLD_EVERY) {
    faceForEye(skyOut, GROUNDS[groundOf(par_)].water ? Redo::WATER : Redo::NONE);
    out.sky = out.lively = true;
    return out;
  }
  dt = owed_;
  owed_ = 0;
  worldTime_ = time_;

  bool drifting = par_.wind > 0 && par_.clouds > 0;
  if (drifting) {
    cloudT_ += dt * par_.wind * WIND_MAX / CLOUD_TILE;
    // After 625 tiles weather map and noise are exactly the same again, for the wind
    // below as for the one above: there the shift wraps, and everything that has
    // stored a state wraps with it.
    if (cloudT_ >= CLOUD_WRAP) {
      cloudT_ -= CLOUD_WRAP;
      for (double& at : airShiftOf_) at -= CLOUD_WRAP;
      pass_.shift -= float(CLOUD_WRAP);
    }
    evolveT_ += dt * par_.wind * EVOLVE_MAX;
  }
  // The dust also moves with the wind at the ground; after 80 image widths it is
  // the same again.
  double blow = std::clamp(double(par_.wind), 0.0, 1.0);
  dustT_ = std::fmod(dustT_ + dt * (0.8 + 6 * blow + 14 * blow * blow), 80.0 * (viewW_ + 80.0));

  if (!started_ && viewW_ >= 480 && viewH_ >= 320) {
    pickStart();
    started_ = true;
    skyDirty_ = true;
  }

  // Ghosts of disappeared windows expire.
  size_t tracked = tracks_.size();
  std::erase_if(tracks_, [&](const Track& t) { return t.gone > -1e8 && time_ - t.gone > GHOST_LIFE; });
  if (tracks_.size() != tracked) updateDepths();
  // As long as something is settling, it needs every frame.
  bool settling = std::any_of(tracks_.begin(), tracks_.end(), [&](const Track& t) {
    return time_ - t.moved < 0.3 || time_ - t.born < APPEAR_DONE + 0.05 || t.gone > -1e8;
  });

  // Clouds. After a user change at once and in one go, so that the sky follows the
  // user's hand; otherwise band by band spread over several frames, so that no
  // single frame takes long.
  bool urgent = skyDirty_;
  skyDirty_ = false;
  updateWeather(dt, urgent);
  updateFlash();
  if (boltDue_) drawBolt();
  // While there is lightning, every frame needs the new state, and one frame after
  // it the darkness.
  bool flick = flash_.level > 0.001f || flashDrawn_ > 0.001f;
  flashDrawn_ = flash_.level;
  // If sun or moon have moved on by themselves, the air follows at once, the
  // clouds with the next pass; nothing starts over.
  bool drifted = heavenMoved_ && !urgent;
  heavenMoved_ = false;
  if (urgent) {
    skyAccum_ = 0;
    airDone_ = 0;
    passActive_ = false;
    mapTick_ = 0;
  }
  if (urgent || drifted) drawBase();
  if (!passActive_ && (urgent || skyAccum_ < SKY_ACC || (drifting && time_ - skyTime_ >= SKY_EVERY))) {
    beginPass();
    skyTime_ = time_;
  }
  bool passDone = false;
  for (int n = urgent ? AIR_BANDS : 1; passActive_ && n > 0; n--) passDone |= stepBand();

  // The finished images. If the clouds drift, every frame moves them a bit
  // further; the one for the light needs that only occasionally, it is soft.
  bool moving = drifting && skyAccum_ > 0;
  bool skyMoved = false;
  // A new pass fades in over the duration of the next one. After a user change it
  // applies at once, and the image before it does not fit then either.
  fade_ = moving && !urgent && airDone_ >= 2 ? std::min(1.0f, float(sinceSwitch_ + 1) / AIR_BANDS) : 1.0f;
  sinceSwitch_++;
  // The rain: after a user change at once, otherwise every fourth frame a bit
  // further.
  if (urgent) drawRain(true);
  else if (++rainTick_ % 4 == 0) drawRain(false);
  bool tick = moving && ++faceTick_ % 2 == 0;
  bool due = faceDue_ && skyAccum_ > 0;
  if (due) faceDue_ = false;
  // What moves through the image is also in the image for the light and in the
  // one the water mirrors: both follow once it has moved visibly, at most thirty
  // times per second.
  bool shifted = alive && GROUNDS[groundOf(par_)].water && skyAccum_ > 0 && time_ - lightTime_ >= 1.0 / 31 && propsShifted();
  if (passDone || drifted || tick || due || flick || shifted) {
    faceForLight();
    if (urgent) sceneDirty_ = true;
    else if (passDone || drifted || flick || faceTick_ % 4 == 0) skyMoved = true;
  }
  // At night the stars twinkle: even without drifting clouds a frame at intervals.
  bool twinkling = light_.night > 0.3f && par_.stars > 0 && skyAccum_ > 0 && time_ - faceTime_ > 1.0 / 20;
  // Water is always in motion.
  bool rippling = GROUNDS[groundOf(par_)].water && skyAccum_ > 0 && time_ - faceTime_ > 1.0 / 62;
  // Rain keeps falling in every frame.
  rippling = rippling || (rainHere_ > 0.003f && skyAccum_ > 0);
  if (passDone || moving || drifted || twinkling || rippling || due || flick) {
    faceTime_ = time_;
    faceForEye(skyOut);
    out.sky = skyOut != nullptr;
  } else if (alive && skyAccum_ > 0) {
    // Nothing but a ship, an aircraft or a bird has moved: the sky stays as it is.
    faceForEye(skyOut, Redo::NONE);
    out.sky = skyOut != nullptr;
  }
  // The image is calm when nothing except the water and what moves through the
  // image needs every refresh.
  calm_ = (GROUNDS[groundOf(par_)].water || !props_.empty()) && skyAccum_ >= SKY_ACC && !urgent && !settling && !flick && rainHere_ <= 0.003f;

  if (iconDirty_) {
    uploadIcons();
    iconDirty_ = false;
  }
  if (sceneDirty_) {
    drawBloom();
    accum_ = 0;
    sceneDirty_ = false;
  } else if (skyMoved) {
    drawBloom();
  }
  // What moves by itself flows in gradually; after a user change the averaging
  // starts anew so that the light follows at once.
  bool flowing = drifting || par_.mist > 0 || drifted;
  bool fresh = false;
  if (accum_ < ACC_FRAMES || flowing) {
    drawScatter();
    accum_ = std::min(accum_ + 1, ACC_FRAMES);
    fresh = true;
  }
  if ((fresh || par_.dust > 0 || settling || cursorMoved_ || flick) && lightOut) {
    cursorMoved_ = false;
    drawComposite(lightOut);
    // If there is lightning, its light is added: past windows and icons like that of
    // the sun.
    if (flash_.level > 0.001f) drawFlashLight(lightOut);
    out.light = true;
  }
  // While the light is still averaging in, every full frame counts.
  calm_ = calm_ && accum_ >= ACC_FRAMES;
  out.lively = calm_;
  target(nullptr, 1, 1);
  return out;
}

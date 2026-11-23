// Draws the sky and the light with Direct3D 11.
//
// Sequence per frame:
//   1. Sky (on change, and at intervals while the clouds drift):
//      air and clouds at limited resolution, averaged over several passes
//      and spread over several frames. From this the finished image with
//      mountains and sun is made, once at full sharpness for the eye and once
//      at half size as a texture for shadows and glare.
//   2. Glare: visible sky, blurred in levels.
//   3. Scattered light: view ray through the haze, averaged over several
//      frames.
//   4. Composition: scattered light, glare, rim light, the dust on top.

#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

// What is in the way of the light, in logical pixels. kind: 1 window, 2 taskbar.
struct Occluder {
  float x0, y0, x1, y1;
  float radius;
  float opacity;
  int kind;
  uint64_t id = 0;   // id of the window, to recognize it from frame to frame; 0 = none
  bool operator==(const Occluder&) const = default;
};

// What is in the sky, in logical pixels. The position may be outside the image
// (below the horizon, beyond the edge).
struct Heaven {
  float sunX = 0, sunY = 0;
  float moonX = 0, moonY = 0;
  float moonLit = 0;    // illuminated fraction of the moon disk; 0: no moon
  float moonLimb = 0;   // direction of the bright side, radians, counterclockwise from the top
  float moonSpin = 0;   // direction of the moon's north pole, likewise
  bool operator==(const Heaven&) const = default;
};

struct Params {
  float density = 0.55f;      // Haze
  float mist = 0.5f;          // Mist
  float length = 0.62f;       // Ray length
  float spread = 0.25f;       // Depth spread
  float g = 0.88f;            // Forward scatter
  float dust = 0.5f;          // Dust
  float sunRadius = 0.0f;     // Sun size in pixels; 0 follows the image size
  float moonRadius = 0.0f;    // Moon size, likewise
  float rays = 0.3f;          // Rays: fine ray needles around the sun
  float clouds = 0.5f;        // Clouds: up to 1 more clouds, above that overcast, rain, thunderstorm (up to 2)
  float wind = 0.5f;          // Wind
  float exposure = 1.0f;      // Brightness
  float bloom = 0.55f;        // Glare
  float rim = 0.8f;           // Rim light
  float overWindows = 0.6f;   // Over windows: fraction of the scattered light in front of windows
  float overSky = 0.4f;       // Over sky: fraction of the scattered light in front of open sky
  float pointer = 1.0f;       // Pointer: strength of its shadow and of its fringe
  float shade = 0.6f;         // Shadows: how much windows and icons darken the sky
  float stars = 1.0f;         // Stars
  float moonlight = 1.0f;     // Moonlight: strength of the light at night
  float glow = 2.0f;          // HDR glow: brightness of the sun as a multiple of white, 1 = off
  float ground = 0.0f;        // Ground: 0 Mountains, 1 Skyline, 2 Sea
  float resolution = 0.5f;    // resolution of the scattered light
  int steps = 40;             // steps of the scattered light
  bool operator==(const Params&) const = default;
};

class Renderer {
 public:
  struct Drawn {
    bool sky = false;    // a new sky image is in skyOut
    bool light = false;  // a new light image is in lightOut
    bool lively = false; // water moves in the image: the next frame is worthwhile with the next display refresh
  };

  bool init(ID3D11Device* device, ID3D11DeviceContext* context);
  // Size of the surface in device pixels and scale (device pixels per logical
  // pixel).
  void resize(int pixelW, int pixelH, float scale);
  void setScene(const std::vector<Occluder>& list);
  // Sun and moon. gentle: they have moved on a bit by themselves, the image
  // follows gradually; otherwise it counts as a user change and is recomputed at
  // once.
  void setHeaven(const Heaven& h, bool gentle = false);
  // Windows and sun in one, without moon.
  void setScene(const std::vector<Occluder>& list, float sunX, float sunY);
  // The image shows the sky compressed: a sun at the top edge is in reality at the
  // zenith, one at the bottom edge far below the horizon. The true altitude in
  // degrees for an image row and vice versa (logical pixels).
  double altitudeOfRow(float y) const;
  float rowOfAltitude(double altitude) const;
  float horizonRow() const { return horizon(); }
  // What is shining right now: the sun or, at night, the moon.
  bool moonShines() const { return light_.moon; }
  float sourceX() const { return light_.x; }
  float sourceY() const { return light_.y; }
  // Coverage of the icons, one byte per device pixel, row by row from the top.
  void setIconMask(const uint8_t* cover, int w, int h);
  void setParams(const Params& p);
  // The mouse pointer. cover: its coverage including the free margin all around,
  // one byte per device pixel, row by row from the top.
  void setCursorShape(const uint8_t* cover, int w, int h);
  // Position of this area in logical pixels; visible false: no pointer.
  // calm: 1 at rest, 0 during fast movement, then the fringe is suspended.
  void setCursor(bool visible, float x0, float y0, float x1, float y1, float calm);
  // How Windows blends the light into the image. linear: HDR mode, the light is
  // output linearly; white: brightness of white there, in multiples of 80 cd/m².
  void setOutput(bool linear, float white);
  const Params& params() const { return par_; }
  // Clouds and wind glide to these values without the sky starting over (live
  // weather).
  void drift(float clouds, float wind);
  // One frame, dt seconds after the previous one. Both targets have the size of
  // the surface.
  Drawn render(double dt, ID3D11RenderTargetView* skyOut, ID3D11RenderTargetView* lightOut);
  // The sky is fully averaged since the last user change.
  bool skySettled() const;
  // Waits until the graphics card has processed everything so far (for
  // measurements).
  void sync();
  // For measurements: a whole sky pass, time per step in milliseconds.
  std::vector<double> timeSteps(ID3D11RenderTargetView* skyOut);

  float viewW() const { return viewW_; }
  float viewH() const { return viewH_; }
  float sunDiscRadius() const { return sunRad_; }
  float moonDiscRadius() const { return moonRad_; }
  // For still images: a lightning flash at full brightness that persists.
  // hold false: an ordinary flash that runs its course.
  void flashNow(bool hold = true);
  // There is lightning right now.
  bool flashing() const { return flash_.level > 0.001f; }
  float flashLevel() const { return flash_.level; }
  // Rain at the viewer's location, 0 to 1.
  float rainHere() const { return rainHere_; }
  // Radius of the disk that is shining right now.
  float sunRadius() const { return light_.moon ? moonRad_ : sunRad_; }
  void seek(double cloudShift) { cloudT_ = cloudShift; started_ = true; skyDirty_ = true; }
  // For debugging: where the newest cloud image knows a distance (blue) and where
  // the one extended into the neighborhood does (green), nearer clouds brighter.
  // As an image at the resolution of the clouds, BGRA from the top.
  bool dumpDepth(std::vector<uint8_t>& bgra, int& w, int& h);
  // State of the clouds: wind shift and time of their evolution. For reproducing a
  // moment.
  double cloudShift() const { return cloudT_; }
  double cloudAge() const { return evolveT_; }
  void seek(double cloudShift, double age) { cloudT_ = cloudShift; evolveT_ = age; started_ = true; skyDirty_ = true; }

 private:
  struct Target {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
  };
  struct Pass {
    float share = 1;
    int steps = 0;
    float shift = 0, jit = 0, subX = 0, subY = 0, evolve = 0;
    int band = 0;
  };
  // A window over time: whether it is moving right now, has just appeared or has
  // just disappeared. On this depends where the light is briefly suspended.
  struct Track {
    uint64_t id = 0;
    Occluder last{};
    double seen = 0;       // time of the last position
    double moved = -1e9;   // last moved
    float margin[4] = {};  // margin without light around the edges left, top, right, bottom; logical pixels
    double born = -1e9;    // appeared
    double gone = -1e9;    // disappeared; stays briefly as a ghost
  };
  // A lightning flash: several return strokes in quick succession at the same
  // place.
  struct Flash {
    bool on = false, hold = false;
    double start = 0;
    float pos[3] = {};                 // place of the light source as seen from the eye, meters: right, up, ahead
    float at[6] = {}, gain[6] = {};    // time and strength of each return stroke
    int strokes = 0;
    float level = 0;                   // brightness now: what it illuminates
    float trunk = 0, twig = 0;         // brightness of the main channel and of the branches
    float grow = 2;                    // how far the stepped leader has grown downwards, 0 to 1; above that the channel is complete
    float top = 0, bottom = 0;         // image row in which the channel starts and ends, logical pixels
    int segs = 0;                      // segments of the channel, 0: sheet lightning without a channel
    float x = 0, y = 0;                // place of the light source in the image, logical pixels
  };
  // The light source and what follows from its position.
  struct Light {
    bool moon = false;                 // the moon is shining, the sun has set
    float x = 0, y = 0;                // light source position in the image
    float dir[3] = {0, 0.2f, 0.98f};   // direction to it as it is in the image
    float atm[3] = {0, 0.2f, 0.98f};   // direction that the air computes with: its true altitude
    float col[3] = {1, 1, 1};          // its light at the ground
    float cloud[3] = {1, 1, 1};        // at the height of the cumulus
    float cirrus[3] = {1, 1, 1};       // at the height of the cirrus
    float disc[3] = {0, 0, 0};         // luminance of the sun disk
    float moonDisc[3] = {0, 0, 0};     // luminance of the moon disk
    float squash = 1, moonSquash = 1;  // how much the air flattens the disks close to the horizon
    float amb[3] = {0, 0, 0};
    float fill[3] = {0, 0, 0};
    float after[3] = {0, 0, 0};        // afterglow above the horizon
    float gain = 1;                    // strength for the scattered light of the air
    float night = 0, day = 1, exposure = 1;
    float land[3] = {0, 0, 0};         // its light on a bright surface at the ground that faces it
    float skyLand[3] = {0, 0, 0};      // sky light on a bright surface at the ground
    float alt = 0.3f;                  // its true altitude in radians
    float lights = 0;                  // how bright the lights of the city are
  };

  Target makeTarget(int w, int h, DXGI_FORMAT format, bool mips);
  void makeNoise();
  bool loadClouds();
  void bindCommon();
  void sizeBodies();
  void allocScatter();
  float focal() const;
  float horizon() const;
  void updateLight();
  void updateDepths();
  void trackScene(const std::vector<Occluder>& list);
  void fillSky(struct SkyC& c, int w, int h, int steps, float shift, float jit, float subX, float subY, float show);
  void fillLight(struct LightC& c);
  void target(ID3D11RenderTargetView* rtv, int w, int h);
  void quad(ID3D11PixelShader* ps);
  void pickStart();
  void beginPass();
  void airBand();
  void drawNear(int image);
  void bindClouds();
  float cloudDensity(double x, double z) const;
  void updateWeather(double dt, bool snap);
  void drawRain(bool fresh);
  bool startFlash(bool force);
  void drawBolt();
  void updateFlash();
  float chance();
  bool loadGround();
  void makeWaves();
  void setSea();
  double surfSpeed() const;
  void bindGround(bool mirror);
  void drawWaterMaps(bool sums, bool shade);
  void faceUpper();
  void faceForLight();
  void faceForEye(ID3D11RenderTargetView* skyOut, bool waterOnly = false);
  void drawBase();
  bool stepBand();
  void uploadIcons();
  void drawBloom();
  void drawScatter();
  void drawComposite(ID3D11RenderTargetView* lightOut);
  void drawFlashLight(ID3D11RenderTargetView* lightOut);

  Microsoft::WRL::ComPtr<ID3D11Device> dev_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vsFull_, vsDust_, vsBolt_, vsBoltFar_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> psNoise_, psAir_, psNear_, psNearBlock_, psBase_, psHaze_, psFace_, psPick_, psRain_, psShade_, psColumn_, psColumnBlock_, psBolt_, psBoltFar_, psScatter_, psBloomPre_,
      psBloomDown_, psBloomUp_, psComposite_, psFlash_, psDust_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> clamp_, wrap_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blendOff_, blendShare_, blendAdd_, blendMax_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_, rasterCut_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> skyBuf_, lightBuf_, noiseBuf_;
  Microsoft::WRL::ComPtr<ID3D11Query> event_;
  Microsoft::WRL::ComPtr<ID3D11Texture3D> noiseTex_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> noise_;
  // Textures of the clouds: base shape, fine cells, weather map, turbulence.
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shape_, detail_, weather_, turb_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> moon_, relief_;   // map of the moon surface and its slope
  // The ground: strip with the land (color, direction and distance, horizon,
  // lights) and the waves of the water.
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> land_[4];
  Microsoft::WRL::ComPtr<ID3D11SamplerState> wide_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> waveBuf_;
  std::vector<float> wave_;       // the wave trains: height, angular frequency, wave vector
  std::vector<float> waveUnit_;   // per wave train: its steepness relative to the others
  std::vector<float> waveLen_;    // and its length in meters
  float seaWind_ = -1;            // position of the wind slider for which the wave heights apply
  float foamOdds_ = 0;            // how often a wave breaks in a cell of the water, 0 to 1
  float foamCover_ = 0;           // fraction of the water under foam
  double gustT_ = 0;              // the gust fields have moved this far over the water, meters
  double dustT_ = 0;              // the wind has carried the dust this far, logical pixels
  int groundLoaded_ = -1;   // which ground is loaded
  std::vector<uint8_t> weatherMap_;   // the weather map of the clouds, RGBA
  int weatherW_ = 0, weatherH_ = 0;
  float rainHere_ = 0;      // rain at the viewer's location, 0 to 1
  Target rain_[2];          // rain in the image: coverage, distance, scattering; written alternately
  int rainCur_ = 0, rainShot_ = 0, rainTick_ = 0;
  Flash flash_;
  Target bolt_;             // the channel of the lightning flash in progress, drawn once per flash: filament and narrow glow
  Target boltFar_;          // its wide glow, at a quarter of the edge length
  bool boltDue_ = false;
  double nextFlash_ = 0;    // earliest time of the next lightning flash; 0: not chosen yet
  float flashDrawn_ = 0;    // brightness of the lightning in the last drawn frame
  uint32_t dice_ = 0x2545f491u;
  Microsoft::WRL::ComPtr<ID3D11Buffer> boltBuf_;
  Target upper_;            // sky and land without water at half size: what is reflected in the water
  Target column_;           // its strip above the horizon, summed column by column from top to bottom
  Target columnBlock_;      // intermediate stage for that: sums within blocks of 16 rows
  int mapTick_ = 0;         // counts how often sums and shadows have been made since the last user change
  int columnTop_ = 0, columnH_ = 1;   // top row of this strip in upper_ and its height
  Target shade_;            // shadows of the clouds on the water, a quarter of the edge length

  Target base_;     // sky without clouds, at the resolution of the clouds
  // Clouds, averaged: rgb = their light, a = transmittance. Three images in a
  // ring: the newest finished one, the one before (for cross-fading) and the one
  // being computed.
  Target air_[3];
  Target airZ_[3];   // mean distance of the clouds per pixel in km, 0 = none
  Target near_[3];   // distance of the frontmost cloud in the neighborhood, at a quarter of the resolution
  Target nearBlock_; // intermediate stage for that
  int airNew_ = 0, airOld_ = 2, airWork_ = 1;
  double airShiftOf_[3] = {};   // wind shift for which each of the images applies
  int airDone_ = 0;             // finished passes since the last user change
  int sinceSwitch_ = 0;         // frames since the last finished pass
  float fade_ = 1;              // fraction of the newest cloud image when cross-fading
  Target haze_;     // haze color of the mountain ranges per image column
  Target sky_;      // finished sky at half size with mip levels, for shadows and glare
  Target icons_;    // coverage of the icons
  Target cursor_;   // coverage of the mouse pointer
  Target scat_[2];  // scattered light, written alternately
  Target scatFlash_[2];   // scattered light of a lightning flash, likewise
  int flashCur_ = 0, flashAccum_ = 0;
  double flashLit_ = -1;  // the lightning flash the scattered light is for
  bool flashPass_ = false;   // the computation currently uses the lightning as light source
  int cur_ = 0;
  std::vector<Target> bloom_;
  int bloomBase_ = 2;  // finest level that enters the image; above that the light source would stay recognizable

  Params par_;
  int pixW_ = 1, pixH_ = 1;
  float viewW_ = 1, viewH_ = 1, scale_ = 1, sunRad_ = 17, moonRad_ = 30;
  std::vector<Occluder> rects_;
  int count_ = 0;
  Heaven heaven_;
  bool heavenMoved_ = false;   // sun or moon have moved on gently
  float pace_ = 0;             // how fast the light source moves, logical pixels per second
  double paceSeen_ = 0;
  float rectData_[32][4] = {};
  float parData_[32][4] = {};
  uint64_t plateId_[32] = {};
  bool plateGhost_[32] = {};
  std::vector<Track> tracks_;
  bool sceneSeen_ = false;
  bool linear_ = false;
  float cursorRect_[4] = {0, 0, -1, -1};
  float cursorCalm_ = 1;
  bool cursorMoved_ = false;
  float white_ = 1;
  float zS_ = 1, zMax_ = 1, zFar_ = 1;
  Light light_;

  std::vector<uint8_t> iconData_;
  int iconW_ = 1, iconH_ = 1;
  bool iconDirty_ = false;

  bool skyDirty_ = true, sceneDirty_ = true, started_ = false, passActive_ = false;
  int accum_ = 0, shot_ = 0, skyAccum_ = 0, skyShot_ = 0;
  Pass pass_;
  double time_ = 0, cloudT_ = 0, skyTime_ = 0;
  double evolveT_ = 0;    // time with which the shapes evolve
  int faceTick_ = 0;
  bool faceDue_ = false;   // the finished image is to be made anew without the clouds starting over
  double faceTime_ = 0;   // when the image for the eye was last drawn
  Target eye_;            // the image for the eye, persists: between two full frames only the water is made anew in it
  DXGI_FORMAT eyeFormat_ = DXGI_FORMAT_UNKNOWN;
  bool calm_ = false;     // apart from the water, nothing needs every display refresh right now
  double worldTime_ = -1; // when everything was last recomputed
  double owed_ = 0;       // time elapsed since then that clouds, light and weather still have to catch up on
};

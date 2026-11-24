// Sky. The air without clouds is in its own image (Base); it changes only with
// the sun. The volumetric clouds are computed at limited resolution (Air), as
// their light and their transmittance, plus their distance per pixel. Because
// the distance is known, the wind gives the position where a cloud was a moment
// ago: this way the passes average without moving clouds smearing, and between
// two passes the final image (Face) keeps moving the clouds on steadily. Face
// puts stars, sun disc and mountain ranges in front at full resolution. The
// alpha channel states how much sunlight the far plane (clouds, mountains) lets
// through.

#include "common.hlsli"

cbuffer SkyC : register(b0) {
  float2 uRes;
  float2 uView;
  float2 uSun;       // position of the light source in the image: the sun, at night the moon
  float2 uSub;
  float uSunR;
  float uFocal;
  float uHorizon;
  float uClouds;
  float uNight;
  float uDay;
  float uCloudT;
  float uJit;
  float3 uSunCol;    // light of the light source at the ground
  float uExposure;
  float3 uSunDir;    // direction to the light source as it appears in the image
  float uShow;
  float3 uAmb;
  int uCloudSteps;
  float uSpan;       // in the final image: top row of the summed strip in the mirror image
  float uCount;      // and its height in rows
  float uShare;      // share of the new pass in the averaged cloud image
  float uBack;       // wind shift since the image that is read from, in tiles
  float uEvolve;     // time by which the shapes of the clouds change
  float uSkyGain;    // strength of the light source for the scattered light of the air: sun 1, moon a fraction
  float uTime;
  float uTown;       // how brightly the lights of a city below shine into the night sky, 0 to 1
  float3 uSunAtm;    // direction to the light source that the air computes with: its true elevation
  float uSrcMoon;    // 1: the light source is the moon
  float3 uCloudCol;  // light of the light source at the height of the cumulus clouds
  float uMoonLit;    // illuminated fraction of the moon disc, 0: no moon
  float3 uCirrusCol; // light of the light source at the height of the cirrus
  float uMoonLimb;
  float3 uDiscCol;   // luminance of the sun disc
  float uMoonSpin;
  float3 uMoonCol;   // luminance of the moon disc
  float uWiden;      // widens the angle to the light source, per radian
  float3 uFill;      // multiply scattered light that fills the sky far from the light source
  float uCover;      // cloud cover: how much of the weather map becomes clouds
  float2 uMoon;      // position of the moon in the image
  float uBackOld;    // the same as uBack, for the cloud image before that
  float uFade;       // share of the newest cloud image when cross-fading
  float3 uAfter;     // afterglow above the horizon once the sun has set
  float uMoonR;      // radius of the moon disc; uSunR is the radius of the light source
  float4 uShape;     // x: squash of the light source at the horizon, y: of the moon, z: ray spikes around the sun, w: the slider Wind, 0 to 1 (it shapes the clouds)
  float4 uGround;    // x: 1 = land from the strip, y: 1 = water, z: eye above the water in m, w: eye above sea level in m
  float4 uStrip;     // x: tangent of the upper edge of the strip, y: of its full height, z: how brightly the city lights burn, w: how far the gust patches have moved across the water, meters
  float3 uSunLand;   // light of the light source on a bright surface facing it
  float uPass;       // 1: only sky and land, without water (the image that is mirrored in the water)
  float3 uSkyLand;   // sky light on a bright surface under open sky
  float uCloudLift;  // this many meters higher than usual the clouds lie above the viewer (over mountains, so that the summits stay clear)
  float4 uLand;      // x: true elevation of the light source in radians, y: how overcast the sky is, z: haze, w: 1 = the land has lights and glass
  float4 uSea;       // color of the light that returns from the water; w: how much more the cumulus lean than in the cloud image that is read (while clouds are computed: the newest; in the final image: the one before it)
  float4 uStorm;     // x: storm darkness of the clouds, y: rain overall, z: rain at the viewer's location, w: time for the rain streaks (wraps after 64 s)
  float4 uFlash;     // lightning: position relative to the eye in meters, w: brightness now
  float4 uFlashCol;  // its color, w: number of segments of its channel (0 = sheet lightning without a channel)
  float4 uFlashMore; // x: brightness of the main channel now, y: of the branches, z: of the sheet lightning, w: image row down to which the stepped leader has grown (very large: the channel is complete)
  float4 uWind;      // wind at the ground. x: slant of the rain (tangent of the angle to the vertical), y: wind over the water in m/s, z: how often waves break there (0 to 1), w: fraction of the water under foam if every wave breaks
  float4 uSnow;      // x: fraction of what falls that is snow, y: height above the sea from which snow lies, z: snow on the ground at the viewer's level (0 to 1), w: slant of the snowfall
  float4 uFog;       // fog: x extinction per meter in the layer, y height of its top above the sea, z depth of the transition at the top, w how much it lies in banks (0 to 1)
  float4 uFogCol;    // its own light before exposure; w: strength of the glare around the light source
};

// The lightning channel as a sequence of short segments in the image.
static const int BOLT_MAX = 768;
cbuffer BoltC : register(b2) {
  float4 uBolt[BOLT_MAX];      // start and end of each segment, logical pixels
  float4 uBoltPar[BOLT_MAX];   // strength, width in logical pixels, 1 = branch
};

Texture3D tNoise : register(t0);
Texture2D tCloud : register(t1);    // clouds: rgb = their light, a = transmittance
Texture2D tHaze : register(t2);
Texture2D tCloudZ : register(t3);   // mean distance of the clouds in km, 0 = none
Texture2D tNear : register(t9);     // distance of the frontmost cloud in the neighborhood, coarse
Texture2D tCloudOld : register(t10);   // the cloud image before that, with its distances
Texture2D tNearOld : register(t11);
Texture2D tCloudZOld : register(t12);
Texture2D tBase : register(t4);     // sky without clouds
Texture2D tRain : register(t20);    // rain: r = coverage, g = mean distance in km, b = its spread
Texture2D tBolt : register(t21);    // lightning channel, filament and narrow glow: r = main channel, g = branches
Texture2D tBoltFar : register(t22); // its wide glow, at a quarter of the edge length
Texture2D tShade : register(t23);   // shadow of the clouds on the water: what they transmit there of the light source's light
Texture2D tColumn : register(t24);  // the strip above the horizon, summed from top to bottom (see Column)
Texture2D tNight : register(t27);   // the night sky: Milky Way, stars, planets, shooting stars; their light as it enters the image

static float cloudShift;   // shift of the clouds by the wind, in tiles
static float cloudAge;     // time by which their shapes change
static float lightJit;     // offsets the path of the light to the sun per pixel and pass

static const float PI = 3.14159265;
static const float R_PLANET = 6371e3;
static const float R_ATMOS = 6471e3;
static const float3 K_RLH = float3(5.5e-6, 13.0e-6, 22.4e-6);
static const float K_MIE = 10e-6;
static const float SH_RLH = 8e3;
static const float SH_MIE = 1.2e3;
static const float G_MIE = 0.5;       // wide: the narrow aureole around the sun is drawn by aureole()
static const float SUN_E = 22.0;
static const float3 NIGHT = float3(0.0035, 0.006, 0.014);

static const float CL_TILE = 6400.0;    // meters of wind shift per unit of cloudShift
static const float3 WIND = float3(1.0, 0.0, 0.3);   // direction of the wind; this far per tile of shift
// The cirrus at seven kilometers altitude is driven by a different wind: faster
// and rotated by 30 degrees.
static const float3 WIND_HIGH = float3(1.75, 0.0, -0.45);

// The wind at height h above the ground. Each layer moves as a whole.
float3 windAt(float h) {
  return h > 4500.0 ? WIND_HIGH : WIND;
}
// The wind at the ground: rotated by 25 degrees against the drift of the clouds,
// toward the viewer (right, ahead). As in render.cpp.
static const float2 SURF = float2(-0.747, -0.665);

// The clouds are computed slightly wider than the image on both sides: what the
// wind carries in from the side has then already been averaged over many passes
// when it enters the image. Without the margin there was a dot pattern there.
static const float GUARD = 96.0;   // logical pixels per side

// Position in the cloud image for a position in the image, and vice versa.
float2 airUv(float2 p) {
  return float2((p.x + GUARD) / (uView.x + 2.0 * GUARD), p.y / uView.y);
}

float2 airPoint(float2 uv) {
  return float2(uv.x * (uView.x + 2.0 * GUARD) - GUARD, uv.y * uView.y);
}

float3 dirOf(float2 p) {
  return normalize(float3(p.x - 0.5 * uView.x, uHorizon - p.y, uFocal));
}

// Inverse: the position in the image at which a point in direction v appears.
float2 pointOf(float3 v) {
  return float2(0.5 * uView.x + v.x / v.z * uFocal, uHorizon - v.y / v.z * uFocal);
}

float2 raySphere(float3 o, float3 d, float r) {
  float b = dot(o, d);
  float h = b * b - dot(o, o) + r * r;
  if (h < 0.0) return float2(1e9, -1e9);
  h = sqrt(h);
  return float2(-b - h, -b + h);
}

// The image shows the sky compressed: what lies ten degrees beside the sun in it
// is in reality much farther away from it. Close to the sun the angle is
// correct, with distance it grows ever faster. This keeps the bright aureole
// tight around the sun, and the sky beside it becomes blue as in a wide-angle
// photo, instead of fading everywhere in the backlight.
float widen(float theta) {
  return min(theta + uWiden * theta * theta, 2.7);
}

// Direction of the light for a cloud in view direction d, by the same rule: in
// front of the sun it is backlit; the farther it is from the sun in the image,
// the more the light comes from the side and from the true elevation of the sun.
float3 lightDir(float3 d) {
  float3 s = uSunDir;
  float theta = acos(clamp(dot(d, s), -1.0, 1.0));
  float azD = atan2(d.x, d.z), dAz = atan2(s.x, s.z) - azD;
  float az = azD + sign(dAz) * widen(abs(dAz));
  float el = lerp(asin(s.y), asin(uSunAtm.y), smoothstep(0.0, 0.25, theta));
  return float3(sin(az) * cos(el), sin(el), cos(az) * cos(el));
}

// Luminance of the air in view direction r. s is the direction to the light
// source as it appears in the image: the bright aureole lies around it. sa is
// the direction in which it actually is: it determines how long the path of the
// light through the air is, that is how red it arrives and how blue the sky
// stays. The image shows the sky compressed, a high sun is lower in it than in
// reality; at the horizon both directions coincide.
float3 atmosphere(float3 r, float3 s, float3 sa) {
  const int I = 12;
  const int J = 6;
  float3 o = float3(0.0, R_PLANET + 600.0, 0.0);
  float tMax = raySphere(o, r, R_ATMOS).y;
  float2 ground = raySphere(o, r, R_PLANET);
  if (ground.x > 0.0) tMax = min(tMax, ground.x);
  float ds = tMax / float(I);

  float mu = cos(widen(acos(clamp(dot(r, s), -1.0, 1.0))));
  float gg = G_MIE * G_MIE;
  float pR = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
  float pM = 3.0 / (8.0 * PI) * ((1.0 - gg) * (1.0 + mu * mu))
           / (pow(max(1.0 + gg - 2.0 * mu * G_MIE, 1e-6), 1.5) * (2.0 + gg));

  float odR = 0.0, odM = 0.0;
  float3 sumR = 0.0, sumM = 0.0;
  for (int i = 0; i < I; i++) {
    float3 pos = o + r * (ds * (float(i) + 0.5));
    float h = length(pos) - R_PLANET;
    float dR = exp(-h / SH_RLH) * ds;
    float dM = exp(-h / SH_MIE) * ds;
    odR += dR;
    odM += dM;

    float dj = raySphere(pos, sa, R_ATMOS).y / float(J);
    float jR = 0.0, jM = 0.0;
    for (int j = 0; j < J; j++) {
      float hj = length(pos + sa * (dj * (float(j) + 0.5))) - R_PLANET;
      jR += exp(min(-hj / SH_RLH, 20.0)) * dj;
      jM += exp(min(-hj / SH_MIE, 20.0)) * dj;
    }
    float3 att = exp(-(K_MIE * (odM + jM) + K_RLH * (odR + jR)));
    sumR += dR * att;
    sumM += dM * att;
  }
  float3 col = SUN_E * (pR * K_RLH * sumR + pM * K_MIE * sumM);
  // Light that was scattered multiple times comes from all directions and fills
  // the sky far from the sun. By day it is hardly noticeable; at twilight it is
  // almost all that still keeps the sky above the viewer blue.
  float3 through = 1.0 - exp(-K_RLH * odR * 1.6);
  return col * uSkyGain + uFill * through;
}

#include "clouds.hlsli"

float3 skyColor(float3 d) {
  float3 col = atmosphere(d, uSunDir, uSunAtm);
  // Moonlight is sunlight, but to the eye a moonlit night appears blue.
  col *= lerp(float3(1.0, 1.0, 1.0), float3(0.55, 0.74, 1.0), uSrcMoon);
  // Afterglow: once the sun has set, a warm band remains above the horizon,
  // brightest in its direction, with a pale pink above it.
  float az = atan2(d.x, d.z) - atan2(uSunDir.x, uSunDir.z);
  float side = 0.3 + 0.7 * exp(-az * az / 0.10);
  float up = max(d.y, 0.0);
  col += uAfter * side * (exp(-up / 0.05) + float3(0.45, 0.2, 0.3) * exp(-up / 0.17));
  // Over a city the night sky is never dark: its lights brighten the air above
  // it, warm and brightest over the roofs, paler higher up.
  col += uTown * (float3(0.15, 0.10, 0.055) * exp(-up / 0.07) + float3(0.024, 0.021, 0.02));
  return col + uNight * NIGHT;
}

float4 Base(float4 pos : SV_Position) : SV_Target {
  float2 p = pos.xy / uRes * uView;
  return float4(min(skyColor(dirOf(p)) * uExposure, 400.0), 1.0);
}

// Fine ray spikes around the sun, as the eye sees them around any glaring light:
// many thin rays of unequal length. Two layers, a few long ones and many short
// ones.
float needles(float2 d, float r) {
  float dist = length(d);
  float turn = atan2(d.y, d.x) / 6.2831853 + 0.5;
  float sum = 0.0;
  for (int k = 0; k < 2; k++) {
    float n = k == 0 ? 26.0 : 84.0;
    float t = turn * n + 0.37 * float(k);
    float i = fmod(floor(t), n), f = frac(t);
    float h = hash11(i * 1.31 + 17.0 * float(k) + 2.0), h2 = hash11(i * 2.17 + 5.3 + float(k));
    float width = k == 0 ? 0.07 + 0.10 * h2 : 0.16;
    float streak = exp(-(f - 0.5) * (f - 0.5) / (width * width));
    float reach = r * (k == 0 ? 6.0 + 12.0 * h : 3.0 + 4.0 * h);
    sum += streak * (0.35 + 0.65 * h2) * smoothstep(reach, r, dist) * (k == 0 ? 1.0 : 0.45);
  }
  return sum;
}

// Aureole and glare around the light source. Aerosols scatter forward in a
// narrow cone: the wide aureole. Plus the glare in the eye: around the sun disc
// the brightness falls off steeply but continuously instead of ending at its
// edge; so it appears glaring and not like a white disc.
float3 aureole(float2 p) {
  float2 d = p - uSun;
  float dist = length(d);
  float halo = dist / (2.2 * uSunR);
  float3 wide = uSunCol * ((16.0 - 8.0 * uSrcMoon) * uExposure * pow(1.0 + halo * halo, -1.5));
  float past = max(dist - uSunR, 0.0) / (0.5 * uSunR);
  float3 glare = uDiscCol * (uExposure * (0.34 * pow(1.0 + past * past, -1.25) + 0.05 * uShape.z * needles(d, uSunR) / (1.0 + 0.03 * past * past)));
  return wide + glare;
}

// Distance of the clouds at a position in a stored image, averaged over the
// neighbors that show a cloud; 0 where there is none. With the nearest pixel
// alone the value would jump from point to point at the outline of a cloud, and
// the outline would get teeth.
float depthAt(Texture2D tex, float2 u) {
  uint w, h;
  tex.GetDimensions(w, h);
  float2 t = u * float2(w, h) - 0.5;
  float2 b = floor(t), f = t - b;
  int2 hi = int2(w, h) - 1;
  float sum = 0.0, ws = 0.0;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      float z = tex.Load(int3(clamp(int2(b) + int2(x, y), int2(0, 0), hi), 0)).r;
      float k = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) * step(1e-4, z);
      sum += z * k;
      ws += k;
    }
  }
  return ws > 1e-4 ? sum / ws : 0.0;
}

// Mean distance of the clouds and how far they spread around it, both averaged
// over the passes (channels b and g); 0 where there is no cloud. With these they
// are interleaved with the land: the values of a single pass fluctuate, and the
// boundary to the land would flicker.
float2 depthSoft(Texture2D tex, float2 u) {
  uint w, h;
  tex.GetDimensions(w, h);
  float2 t = u * float2(w, h) - 0.5;
  float2 b = floor(t), f = t - b;
  int2 hi = int2(w, h) - 1;
  float2 sum = 0.0;
  float ws = 0.0;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      float2 z = tex.Load(int3(clamp(int2(b) + int2(x, y), int2(0, 0), hi), 0)).bg;
      float k = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) * step(1e-4, z.x);
      sum += z * k;
      ws += k;
    }
  }
  return ws > 1e-4 ? sum / ws : 0.0;
}

// Where in a stored cloud image was what is now visible in view direction d?
// Since that image the clouds have moved with the wind by back tiles, the
// farther in the image the nearer they are. z: their distance in km; 0 if no
// cloud is known at this position. Then the distance of the frontmost cloud in
// the neighborhood applies: its edge and the sky right beside it move with it.
// With a merely estimated distance they ran ahead or lagged behind, and the
// cloud dragged a fringe behind it. How far a cloud has moved depends on its
// height (driftOf): the cirrus at the very top is driven by a different wind.
// zTex: the distances of that image, nearTex: the same, coarse and extended into
// the neighborhood. zOld: the distance that the content had there.
// lean: how much more the cumulus lean now than in that image (tangent). If the
// wind has changed since, what is high up in a cloud stood closer to its base
// then, or farther from it.
float3 driftOf(float3 d, float z, float back, float lean) {
  float3 at = float3(0.0, R_PLANET + EYE_ALT - uCloudLift, 0.0) + d * (z * 1000.0);
  float h = length(at) - R_PLANET - EYE_ALT;
  float3 was = back * CL_TILE * windAt(h);
  if (h < 4500.0) was.xz -= DOWNWIND * (lean * max(h - CUMULUS_LO, 0.0));
  return was;
}

float2 cloudFrom(Texture2D zTex, Texture2D nearTex, float2 uv, float3 d, float z, float back, float lean, out float zOld) {
  zOld = z;
  if ((abs(back) < 1e-7 && abs(lean) < 1e-6) || d.y <= 0.004) return uv;
  if (z <= 0.0) {
    z = depthAt(nearTex, uv);
    if (z <= 0.0) {
      // No cloud known far and wide. Whatever may be here is hardly visible; it still
      // moves along, estimated with the mean height of the layer. If it stood still
      // while everything beside it moves, the boundary in between would appear as a
      // stepped edge in the image.
      float3 o = float3(0.0, R_PLANET + EYE_ALT - uCloudLift, 0.0);
      z = raySphere(o, d, R_PLANET + EYE_ALT + 1300.0).y * 0.001;
    } else {
      float3 N = d * (z * 1000.0) + driftOf(d, z, back, lean);
      if (N.z <= 1.0) return float2(-1.0, -1.0);
      float zn = length(N) * 0.001, zs = depthAt(zTex, airUv(pointOf(N)));
      if (!(zs > 0.0 && abs(zs - zn) < 0.3 * zn)) {
        // The front cloud was not there at all. Then this position shows what was
        // already here before: a more distant cloud that moves more slowly.
        float zf = depthAt(zTex, uv);
        if (zf > 0.0) z = zf;
      }
    }
  }
  float3 P = d * (z * 1000.0) + driftOf(d, z, back, lean);
  if (P.z <= 1.0) return float2(-1.0, -1.0);
  zOld = length(P) * 0.001;
  return airUv(pointOf(P));
}

// Distance of the frontmost cloud in the neighborhood of each pixel, at a
// quarter of the resolution: for tracking the edges (see cloudFrom). In two
// stages, so that no pixel is left out; thin haze often has only isolated pixels
// with a distance. First stage: the frontmost cloud per block of four by four
// pixels.
float4 NearBlock(float4 pos : SV_Position) : SV_Target {
  uint w, h;
  tCloudZ.GetDimensions(w, h);
  int2 c = int2(pos.xy) * 4, hi = int2(w, h) - 1;
  float z = 1e9;
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      float v = tCloudZ.Load(int3(min(c + int2(x, y), hi), 0)).r;
      if (v > 0.0) z = min(z, v);
    }
  }
  return float4(z < 1e8 ? z : 0.0, 0.0, 0.0, 1.0);
}

// Second stage: extended into the neighborhood. The result of the first stage is
// read (bound as tNear).
float4 Near(float4 pos : SV_Position) : SV_Target {
  uint w, h;
  tNear.GetDimensions(w, h);
  int2 c = int2(pos.xy), hi = int2(w, h) - 1;
  // The cloud right next to it takes precedence over one that is nearer to the
  // viewer but farther away in the image: search the small radius first.
  float close = 1e9, wide = 1e9;
  for (int y = -4; y <= 4; y++) {
    for (int x = -4; x <= 4; x++) {
      float v = tNear.Load(int3(clamp(c + int2(x, y), int2(0, 0), hi), 0)).r;
      if (v <= 0.0) continue;
      wide = min(wide, v);
      if (abs(x) <= 1 && abs(y) <= 1) close = min(close, v);
    }
  }
  float z = close < 1e8 ? close : wide;
  return float4(z < 1e8 ? z : 0.0, 0.0, 0.0, 1.0);
}

// Samples the averaged cloud image with a sharp filter (Catmull-Rom over nine
// taps). The history is read at a shifted position on every pass; with linear
// filtering it would get slightly softer each time, until nothing remains of
// fine structures.
float4 cloudSharp(float2 uv) {
  float2 pos = uv * uRes;
  float2 tc = floor(pos - 0.5) + 0.5;
  float2 f = pos - tc;
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  float2 w3 = f * f * (-0.5 + 0.5 * f);
  float2 w12 = w1 + w2;
  float2 a = (tc - 1.0) / uRes, b = (tc + w2 / w12) / uRes, c = (tc + 2.0) / uRes;
  float4 r = tCloud.SampleLevel(sClamp, float2(a.x, a.y), 0.0) * (w0.x * w0.y)
           + tCloud.SampleLevel(sClamp, float2(b.x, a.y), 0.0) * (w12.x * w0.y)
           + tCloud.SampleLevel(sClamp, float2(c.x, a.y), 0.0) * (w3.x * w0.y)
           + tCloud.SampleLevel(sClamp, float2(a.x, b.y), 0.0) * (w0.x * w12.y)
           + tCloud.SampleLevel(sClamp, float2(b.x, b.y), 0.0) * (w12.x * w12.y)
           + tCloud.SampleLevel(sClamp, float2(c.x, b.y), 0.0) * (w3.x * w12.y)
           + tCloud.SampleLevel(sClamp, float2(a.x, c.y), 0.0) * (w0.x * w3.y)
           + tCloud.SampleLevel(sClamp, float2(b.x, c.y), 0.0) * (w12.x * w3.y)
           + tCloud.SampleLevel(sClamp, float2(c.x, c.y), 0.0) * (w3.x * w3.y);
  return float4(max(r.rgb, 0.0), saturate(r.a));
}

struct AirOut {
  float4 col : SV_Target0;   // light of the clouds, transmittance
  float4 z : SV_Target1;     // r: their mean distance in km; g, b: spread and distance, averaged over the passes
};

// Clouds. uSub offsets the sampling by fractions of a pixel per pass; averaged
// over the passes this smooths the edges. Averaging uses the previous image, at
// the position where this cloud was in it.
AirOut Air(float4 pos : SV_Position) {
  cloudShift = uCloudT;
  cloudAge = uEvolve;
  lightJit = ign(pos.yx + 1.7 * uJit + 11.0);
  float2 uv = pos.xy / uRes;
  float2 p = airPoint((pos.xy + uSub) / uRes);
  float3 d = dirOf(p);
  float3 s = lightDir(d);

  float4 now = float4(0.0, 0.0, 0.0, 1.0);
  float zNow = 0.0, deepNow = 0.0;
  if (uClouds > 0.001 && d.y > 0.004) {
    float3 sky = tBase.SampleLevel(sClamp, p / uView, 0.0).rgb / uExposure;
    float distLow, distHigh;
    float4 low = cloudLayer(d, s, sky, ign(pos.xy + uJit), distLow);
    float lowDeep = cloudDeep;
    float4 high = highLayer(d, s, ign(pos.yx + 2.3 * uJit + 5.0), distHigh);
    // Distant clouds fade into the haze of the air: they take on the color of the
    // sky in front of which they stand.
    low.rgb = lerp(sky * (1.0 - low.a), low.rgb, exp(-distLow / 45e3));
    high.rgb = lerp(sky * (1.0 - high.a), high.rgb, exp(-distHigh / 140e3));
    now = float4(min((low.rgb + low.a * high.rgb) * uExposure, 400.0), low.a * high.a);
    // Every cloud carries its distance, even the thinnest cirrus. What has none is
    // tracked with the distance of the nearest cloud; cirrus would then move wrongly
    // in a blocky area around each near cloud.
    zNow = (distLow > 0.0 ? distLow : distHigh) * 0.001;
    deepNow = distLow > 0.0 ? lowDeep * 0.001 : 0.0;
  }

  float share = uShare;
  float4 was = now;
  float zWas = zNow;
  float2 soft = float2(zNow, deepNow);
  if (share < 0.999) {
    float zOld;
    // The history is looked up for the center of the pixel, not for the offset
    // sample of this pass: otherwise it would be read shifted by a fraction on every
    // pass and would blur over time.
    float2 uvOld = cloudFrom(tCloudZ, tNear, uv, dirOf(airPoint(uv)), zNow, uBack, uSea.w, zOld);
    if (uvOld.x > 0.0 && uvOld.y > 0.0 && uvOld.x < 1.0 && uvOld.y < 1.0) {
      was = cloudSharp(uvOld);
      zWas = depthAt(tCloudZ, uvOld);
      // If a different cloud was there than the one that is here now (a nearer one had
      // covered it), the new one counts more. At the edge of a cloud one pass hits it
      // and the next does not; that is meant to average out and gives the smooth
      // outline.
      if (zNow > 0.0 && zWas > 0.0) share = max(share, saturate(abs(zWas - zOld) / (0.3 * zOld) - 0.3));
      float2 softWas = depthSoft(tCloudZ, uvOld);
      if (softWas.x > 0.0) soft = zNow > 0.0 ? lerp(softWas, soft, max(share, 0.06)) : softWas;
    } else {
      share = 1.0;
    }
  }
  AirOut o;
  o.col = lerp(was, now, share);
  float z = zNow > 0.0 ? zNow : (o.col.a < 0.998 ? zWas : 0.0);
  o.z = z > 0.0 ? float4(z, soft.y, soft.x > 0.0 ? soft.x : z, 1.0) : float4(0.0, 0.0, 0.0, 1.0);
  return o;
}

// Rain at coarse resolution: how much of the image behind it it covers, at what
// distance it falls on average and how far it spreads around that. Averaged with
// the previous image (bound as tRain), which smooths the samples.
float4 Rain(float4 pos : SV_Position) : SV_Target {
  cloudShift = uCloudT;
  cloudAge = uEvolve;
  float2 uv = pos.xy / uRes;
  float dist, deep;
  float through = rainVeil(dirOf(uv * uView), ign(pos.xy + uJit), dist, deep);
  float4 now = float4(1.0 - through, dist * 0.001, deep * 0.001, 1.0);
  float4 was = tRain.SampleLevel(sClamp, uv, 0.0);
  // Where there was no rain a moment ago, the new distance applies immediately.
  if (was.r < 1e-4) was.gb = now.gb;
  if (now.r < 1e-4) now.gb = was.gb;
  return lerp(was, now, uShare);
}

// Haze color of the mountain ranges per image column: the sky just above the
// horizon in this direction, limited in brightness without shifting the hue.
float4 Haze(float4 pos : SV_Position) : SV_Target {
  float x = pos.x / uRes.x * uView.x;
  float3 d = normalize(float3(x - 0.5 * uView.x, 0.0, uFocal));
  float3 haze = skyColor(normalize(float3(d.x, 0.075, d.z)));
  haze *= min(1.0, 7.0 / max(haze.r, max(haze.g, haze.b)));
  return float4(haze * uExposure, 1.0);
}

#include "ground.hlsli"

// The lightning channel, drawn once per lightning strike: a glaring filament a
// few pixels wide, a narrow glow close around it and a faint one far around it.
// Each segment is a rectangle as large as its glow reaches; where several
// overlap, the brightest applies. Main channel (r) and branches (g) are
// separate, because the branches glow only during the first return stroke.
// Filament and narrow glow are in one image at full resolution, the wide glow in
// a second one at a quarter of the edge length.
static const float BOLT_NEAR = 30.0;    // the narrow glow reaches this far, logical pixels
static const float BOLT_WIDE = 170.0;   // and the wide one this far

struct BoltOut {
  float4 pos : SV_Position;
  nointerpolation float4 seg : TEXCOORD0;
  nointerpolation float4 par : TEXCOORD1;
};

static const float2 BOLT_CORNER[6] = {
  float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
  float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0),
};

// The rectangle around a segment, larger by reach on all sides.
BoltOut boltQuad(uint vid, float reach) {
  uint i = vid / 6u;
  float2 corner = BOLT_CORNER[vid % 6u];
  float4 seg = uBolt[i];
  float2 along = seg.zw - seg.xy;
  along /= max(length(along), 1e-3);
  float2 across = float2(-along.y, along.x);
  float2 p = (corner.x < 0.0 ? seg.xy : seg.zw) + (along * corner.x + across * corner.y) * reach;
  BoltOut o;
  o.pos = float4(p.x / uView.x * 2.0 - 1.0, 1.0 - p.y / uView.y * 2.0, 0.0, 1.0);
  o.seg = seg;
  o.par = uBoltPar[i];
  return o;
}

BoltOut BoltVS(uint vid : SV_VertexID) { return boltQuad(vid, BOLT_NEAR); }
BoltOut BoltFarVS(uint vid : SV_VertexID) { return boltQuad(vid, BOLT_WIDE); }

// Distance of a pixel from its segment, logical pixels.
float boltGap(BoltOut v) {
  float2 p = v.pos.xy / uRes * uView;
  float2 a = v.seg.xy, ab = v.seg.zw - a;
  float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-4));
  return length(p - a - ab * t);
}

float4 Bolt(BoltOut v) : SV_Target {
  float d = boltGap(v);
  // No filament is drawn narrower than a pixel, it is drawn fainter instead:
  // otherwise it would break up into steps.
  float w = max(v.par.y, 0.62 * uView.x / uRes.x);
  float near1 = d / (2.8 * v.par.y + 0.5);
  float light = v.par.x * (110.0 * v.par.y / w * exp(-d * d / (w * w)) + 4.2 / pow(1.0 + near1 * near1, 1.5)) * smoothstep(BOLT_NEAR, 0.6 * BOLT_NEAR, d);
  return v.par.z < 0.5 ? float4(light, 0.0, 0.0, 0.0) : float4(0.0, light, 0.0, 0.0);
}

float4 BoltFar(BoltOut v) : SV_Target {
  float d = boltGap(v);
  float far1 = d / (22.0 * v.par.y + 16.0);
  float light = v.par.x * 0.6 / (1.0 + far1 * far1) * smoothstep(BOLT_WIDE, 0.5 * BOLT_WIDE, d);
  return v.par.z < 0.5 ? float4(light, 0.0, 0.0, 0.0) : float4(0.0, light, 0.0, 0.0);
}

// The light of the lightning channel at a position in the image: white in the
// core, bluish in the glow.
float3 boltLight(float2 uv) {
  if (uFlashCol.w < 0.5 || uFlashMore.x + uFlashMore.y <= 0.001) return 0.0;
  float2 b = tBolt.SampleLevel(sClamp, uv, 0.0).rg + tBoltFar.SampleLevel(sClamp, uv, 0.0).rg;
  float trunk = uFlashMore.x, twig = uFlashMore.y;
  if (uFlashMore.w < 1e7) {
    // The stepped leader grows down from the cloud, the branches with it: what lies
    // above its tip glows faintly, brightest close to the tip.
    float behind = uFlashMore.w - uv.y * uView.y;
    float lit = smoothstep(-4.0, 4.0, behind) * (1.0 + 9.0 * exp(-max(behind, 0.0) / 30.0));
    trunk *= lit;
    twig *= lit;
  }
  float light = b.r * trunk + b.g * twig;
  return lerp(float3(0.70, 0.82, 1.0), float3(1.0, 0.96, 1.0), smoothstep(0.5, 6.0, light)) * light;
}

// Lightning lights up the rain around it: bright close to the channel, hardly at
// all a few hundred meters farther. v: view direction, rain: coverage of the
// rain, its mean distance and its spread in km.
float rainFlash(float3 v, float3 rain) {
  if (uFlash.w <= 0.001) return 0.0;
  float2 h = v.xz;
  float flat = max(dot(h, h), 1e-4);
  // The point of the view ray in the rain that comes closest to the channel.
  float mean = rain.g * 1000.0, deep = max(rain.b, 0.08 * rain.g) * 1000.0;
  float s = clamp(dot(uFlash.xz, h) / flat, max(mean - 1.6 * deep, 0.0), mean + 1.6 * deep);
  float away = length(h * s - uFlash.xz);
  return uFlash.w * (1.0 / (1.0 + pow(away / 380.0, 2.0)) + 0.03);
}

// Rain close in front of the viewer: rain streaks in five layers, a few long and
// fast ones in front, many short ones behind. Each layer is a grid with one drop
// falling as a streak in each of its cells. The wind at the ground carries the
// drops along: the more strongly it blows across the view, the more slanted they
// fall. Gusts make the slant vary, slightly differently in each layer.
float rainFall(float2 p) {
  float sum = 0.0;
  float lean = -SURF.x * uWind.x;
  float turn = uStorm.w * (6.2831853 / 64.0);
  for (int k = 0; k < 5; k++) {
    float far = float(k) * 0.25;
    float2 cell = lerp(float2(120.0, 560.0), float2(28.0, 160.0), far);
    // Cells per second, rounded to sixty-fourths: the time wraps after 64 s.
    float rate = round(lerp(6.2, 7.4, far) * 64.0) / 64.0;
    float slant = lean * (1.0 + 0.16 * sin(3.0 * turn + 1.7 * float(k)) + 0.09 * sin(7.0 * turn + 2.9 * float(k)))
                + 0.02 * sin(5.0 * turn + 2.3 * float(k));
    float2 q = float2((p.x + (p.y - 0.5 * uView.y) * slant) / cell.x, p.y / cell.y - uStorm.w * rate);
    float2 id = floor(q), f = q - id;
    float h = hash12(id + 17.0 * float(k)), h2 = hash12(id * 1.7 + 3.1 + float(k));
    if (h > lerp(0.5, 0.85, far) * uStorm.z + 0.08) continue;
    // A slanted drop falls faster and draws a longer streak; at the full computed
    // length it would look like a scratch.
    float len = (0.30 + 0.35 * h2) / sqrt(sqrt(1.0 + slant * slant));
    float y0 = (1.0 - len) * frac(h * 7.3);
    // Measured crosswise the streak keeps the same width, even when it is slanted.
    float width = lerp(1.5, 0.8, far) / cell.x * sqrt(1.0 + slant * slant);
    float across = (f.x - (0.15 + 0.7 * frac(h2 * 5.7))) / width;
    float along = (f.y - y0) / len;
    float streak = exp(-across * across) * smoothstep(0.0, 0.25, along) * smoothstep(1.0, 0.6, along);
    sum += streak * lerp(1.0, 0.4, far) * (0.5 + 0.5 * frac(h * 13.1));
  }
  return sum;
}

// Snow close in front of the viewer: flakes in six layers, a few large ones in
// front, many small ones behind. Each layer is a grid with one flake per cell;
// the grid falls slowly and slides sideways with the wind at the ground, much
// more than rain, and each flake sways on its way.
float snowFall(float2 p) {
  float sum = 0.0;
  float lean = -SURF.x * uSnow.w;
  float turn = uStorm.w * (6.2831853 / 64.0);
  for (int k = 0; k < 6; k++) {
    float far = float(k) * 0.2;
    float cell = lerp(150.0, 34.0, far);
    // Cells per second down and sideways, rounded to sixty-fourths: the time wraps
    // after 64 s.
    float fall = round(lerp(1.1, 1.9, far) * 64.0) / 64.0;
    float slide = round(lean * fall * 64.0) / 64.0;
    float2 q = float2(p.x / cell + uStorm.w * slide, p.y / cell - uStorm.w * fall) + 13.7 * float(k);
    float2 id = floor(q), f = q - id;
    float h = hash12(id + 17.0 * float(k)), h2 = hash12(id * 1.7 + 3.1 + float(k));
    // The heavier the snowfall, the more cells hold a flake.
    if (h > 0.2 + 0.8 * uStorm.z) continue;
    float2 at = float2(0.3 + 0.4 * h2 + 0.09 * sin(16.0 * turn + 6.2831853 * h) + 0.05 * sin(11.0 * turn + 9.1 * h2),
                       0.3 + 0.4 * frac(h * 7.3) + 0.03 * sin(13.0 * turn + 4.7 * h2));
    float size = lerp(2.9, 1.0, far) / cell * (0.7 + 0.6 * frac(h2 * 5.7));
    float2 d = (f - at) / size;
    sum += exp(-dot(d, d)) * lerp(1.0, 0.6, far);
  }
  return sum;
}

// The clouds at a position in the image, in view direction toward: their light
// and transmittance, and their mean distance and its spread in km (0 = none).
// They are where the wind has carried them since their pass. When a new pass
// arrives, it fades in over the duration of the next one: this way the image
// changes steadily instead of jumping with every pass.
void cloudsAt(float2 p, float3 toward, out float4 cloud, out float2 cloudZ) {
  float zThere;
  float2 au = airUv(p);
  float2 from = cloudFrom(tCloudZ, tNear, au, toward, 0.0, uBack, 0.0, zThere);
  cloud = tCloud.SampleLevel(sClamp, from, 0.0);
  // Distance of the clouds at this position in km, 0 = none: by this they lie in
  // front of or behind the land.
  cloudZ = depthSoft(tCloudZ, from);
  // Where none is known at this position itself, that of the clouds around it.
  if (cloudZ.x <= 0.0) cloudZ = float2(tNear.SampleLevel(sClamp, from, 0.0).r, 0.0);
  if (uFade < 0.999) {
    // The image before may have been made in a different wind: its clouds are set
    // to the lean of the newest one, so that the two fade into each other in place.
    from = cloudFrom(tCloudZOld, tNearOld, au, toward, 0.0, uBackOld, uSea.w, zThere);
    float4 older = tCloudOld.SampleLevel(sClamp, from, 0.0);
    float2 zOlder = depthSoft(tCloudZOld, from);
    if (zOlder.x <= 0.0) zOlder = float2(tNearOld.SampleLevel(sClamp, from, 0.0).r, 0.0);
    cloud = lerp(older, cloud, uFade);
    cloudZ = cloudZ.x > 0.0 && zOlder.x > 0.0 ? lerp(zOlder, cloudZ, uFade) : (cloudZ.x > 0.0 ? cloudZ : zOlder);
  }
}

// The final sky image. uShow = 1 draws it including the sun disc for the screen;
// 0 yields luminance without the disc, and transmittance, for the light layer
// (there skyAt adds the disc with a sharp edge).
float4 Face(float4 pos : SV_Position) : SV_Target {
  float2 uv = pos.xy / uRes;
  float2 p = uv * uView;
  float3 toward = dirOf(p);
  float4 cloud;
  float2 cloudZ;
  cloudsAt(p, toward, cloud, cloudZ);
  // Sheet lightning: lightning lights the clouds around it from inside. What lies
  // close to it glows; clouds in front, farther away from it, stay dark and appear
  // as an outline against the glow.
  if (uFlashMore.z > 0.001 && cloudZ.x > 0.0) {
    float3 at = toward * (cloudZ.x * 1000.0);
    float away = length(at - uFlash.xyz);
    float billow = tNoise.SampleLevel(sWrap, at * 0.00031 + 0.37, 0.0).r;
    // Close to the lightning the cloud glows, far around it a faint glow lies over
    // the whole cloud deck.
    float glow = uFlashMore.z * (0.45 + 1.1 * billow) * (1.0 / (1.0 + pow(max(away, 0.0) / 1300.0, 2.4)) + 0.02 / (1.0 + pow(max(away, 0.0) / 8000.0, 2.0)));
    cloud.rgb += uFlashCol.rgb * (0.16 * glow * (1.0 - cloud.a) * uExposure);
  }
  float3 sky = tBase.SampleLevel(sClamp, uv, 0.0).rgb + aureole(p);
  float alpha = cloud.a;

  // Behind the clouds: the night sky (see night.hlsl), moon and sun disc.
  float3 back = 0.0;
  float3 night = tNight.SampleLevel(sClamp, uv, 0.0).rgb;
  if (uSrcMoon < 0.5) {
    float cover;
    back += uDiscCol * sunFace(p - uSun, uSunR, uShape.x, cover) * uShow;
  }
  if (uMoonLit > 0.0) {
    // The moon covers the stars behind it, also with its dark side. If it is the
    // light source, the light layer itself draws it sharply into the image.
    float cover;
    float3 face = moonFace(p - uMoon, uMoonR, uMoonLit, uMoonLimb, uMoonSpin, uShape.y, cover);
    back *= 1.0 - cover;
    night *= 1.0 - cover;
    if (uShow > 0.5 || uSrcMoon < 0.5) back += uMoonCol * face;
  }
  sky += back * uExposure + night;
  // Above a thunderstorm the high sky is overcast too: in the gaps there is pale
  // gray, no blue.
  sky = lerp(sky, luma(sky) * float3(0.92, 0.95, 1.0) * 0.8, 0.9 * uStorm.x);
  float3 col = sky * cloud.a + cloud.rgb;

  // Rain: how much it covers and where it falls.
  float3 rain = uStorm.y > 0.001 ? tRain.SampleLevel(sClamp, uv, 0.0).rgb : 0.0;

  // The ground: water up to the sea horizon, the land in front of it; clouds that
  // are nearer than the land in front of that.
  // near: the fraction of the rain that lies in front of what is visible at this
  // position.
  float near = 1.0;
  // The lightning channel is below the clouds: clouds and land in front of it
  // cover it.
  float3 bolt = boltLight(uv);
  float boltFar = length(uFlash.xyz);
  if (bolt.r + bolt.g + bolt.b > 0.0) {
    float tF, tB;
    float3 lF, lB;
    cloudSplit(cloud, cloudZ, boltFar, tF, lF, tB, lB);
    bolt *= tF;
  }
  float fogTau = uFog.x > 0.0 ? fogSky(toward) : 0.0;
  ground(p, toward, uv, sky, cloud, cloudZ, rain, boltFar, col, alpha, near, bolt, fogTau);

  // Fog lies in front of everything it reaches: ground, water, and the sky low
  // above the horizon.
  if (uFog.x > 0.0) {
    float seen = exp(-fogTau);
    col = col * seen + fogLight(toward, sunOpen) * (1.0 - seen);
    alpha *= seen;
    bolt *= seen;
    landLights *= seen;
  }

  // The rain lies in front as a gray veil.
  if (rain.r > 1e-4) {
    float veil = pow(max(1.0 - rain.r, 1e-4), near);
    col = col * veil + (rainLight() + uFlashCol.rgb * (0.016 * rainFlash(toward, rain))) * ((1.0 - veil) * uExposure);
    alpha *= veil;
    // Only the rain that falls in front of the lightning channel covers it.
    bolt *= pow(max(1.0 - rain.r, 1e-4), rainBefore(rain, boltFar));
  }
  col += bolt * uExposure;

  // Rain or snow close in front of the viewer. Flakes are white: they return far
  // more of the light than drops.
  if (uShow > 0.5 && uStorm.z > 0.002) {
    float3 lit = uSkyLand * 0.3 + 0.01 + uFlashCol.rgb * (uFlash.w * 0.03 / (1.0 + dot(uFlash.xz, uFlash.xz) / 1.6e7));
    if (uSnow.x < 0.999) col += lit * (rainFall(p) * uStorm.z * (1.0 - uSnow.x) * uExposure);
    if (uSnow.x > 0.001) col += (lit * 2.4 + uSunLand * (0.06 * sunOpen)) * (snowFall(p) * uSnow.x * uExposure);
  }

  // Fine noise against color banding in the gradients. From two random values, not
  // from a fixed grid: that appeared as hatching in the dark night sky.
  if (uShow > 0.5) return float4(develop(col) + (hash12(pos.xy + 0.37) + hash12(pos.yx * 1.31 + 7.7) - 1.0) / 255.0, 1.0);
  // Instead of the transmittance, the mirror image stores how much of the light at
  // each position comes from lamps.
  return float4(col, uPass > 0.5 ? landLights : alpha);
}

// What moves through the image: ships, an aircraft, birds. Each is a mesh with
// its surface colors in the layers of one texture. It is computed in two steps.
// First the mesh alone, lit, into a small work image with several samples per
// pixel and, if it is small in the image, several of its pixels per pixel of the
// image: masts and wings are much thinner than a pixel and would flicker
// otherwise. Then that image is set into the sky: behind haze, clouds and rain
// according to its distance.
cbuffer PropC : register(b3) {
  float4 uPropRow[3];   // rows of the rotation of the model; w: where its origin is, meters: right of the eye, above the water, ahead
  float4 uPropRect;     // its rectangle in the image, logical pixels: left, top, right, bottom
  float4 uPropDepth;    // x: nearest, y: farthest distance in meters; z: height of the water above the eye (in the mirror image: below it), w: factor on the curvature of the Earth
  float4 uPropWork;     // x, y: left top corner of the rectangle in pixels of the target, z: pixels of the work image per pixel and axis (a whole number, or less than 1), w: samples per pixel there
  float4 uPropAir;      // x: distance of the object in meters, y: its height above sea level, z: 1 = cut off below the water line, w: gloss
  float4 uPropLight;    // x, y: position in the image at which the shadow of the clouds on the water is read; z: 1 = read it there, 0 = as at the viewer, 2 = above the clouds; w: how much light comes back from below
  float4 uPropWing;     // birds: x: angle of the inner wing, y: additional angle of the outer wing, z: half width of the body, w: length of a wing (0 = rigid)
  float4 uPropAxis;     // x: height of the wing root in the model, y: 1 = mirrored left to right
};

Texture2DArray tPropTex : register(t25);          // surface colors of the model, one layer per material
Texture2DMS<float4> tPropWork : register(t26);    // the work image: light of the covered samples, a = covered

struct PropVert {
  float3 pos : POSITION;      // meters: right, up, forward
  float4 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float4 tint : COLOR0;       // color factor of the material, a = its layer
};

struct PropOut {
  float4 pos : SV_Position;
  float3 normal : NORMAL;
  float3 uvl : TEXCOORD0;
  float3 tint : COLOR0;
  float3 at : TEXCOORD1;      // position relative to the eye
  float height : TEXCOORD2;   // above the water
  float thin : TEXCOORD3;     // 1: sailcloth or feathers, light shines through
};

PropOut PropVS(PropVert v) {
  float3 m = v.pos, n = v.normal.xyz;
  if (uPropWing.w > 0.0) {
    // The wing beat: the inner wing turns at the shoulder, the outer part bends
    // further at the wrist. Each point turns around the shoulder by the angle that
    // applies at its distance from the body.
    float side = m.x < 0.0 ? -1.0 : 1.0;
    float2 q = float2(abs(m.x) - uPropWing.z, m.y - uPropAxis.x);
    float t = saturate(q.x / uPropWing.w);
    float sn, cs;
    sincos(uPropWing.x * smoothstep(0.0, 0.18, t) + uPropWing.y * smoothstep(0.38, 0.72, t), sn, cs);
    q = float2(q.x * cs - q.y * sn, q.x * sn + q.y * cs);
    m.x = side * (q.x + uPropWing.z);
    m.y = q.y + uPropAxis.x;
    float2 nq = float2(n.x * side, n.y);
    nq = float2(nq.x * cs - nq.y * sn, nq.x * sn + nq.y * cs);
    n.x = nq.x * side;
    n.y = nq.y;
  }
  if (uPropAxis.y > 0.5) {
    m.x = -m.x;
    n.x = -n.x;
  }
  PropOut o;
  float3 at = float3(dot(uPropRow[0].xyz, m), dot(uPropRow[1].xyz, m), dot(uPropRow[2].xyz, m)) + float3(uPropRow[0].w, uPropRow[1].w, uPropRow[2].w);
  o.normal = float3(dot(uPropRow[0].xyz, n), dot(uPropRow[1].xyz, n), dot(uPropRow[2].xyz, n));
  o.height = at.y;
  // As seen from the eye: the water lies below it, and with distance the Earth
  // curves away downward.
  at.y += uPropDepth.z - uPropDepth.w * dot(at.xz, at.xz) / (2.0 * R_EFF);
  o.at = at;
  float2 s = pointOf(at);
  float2 ndc = (s - uPropRect.xy) / (uPropRect.zw - uPropRect.xy) * float2(2.0, -2.0) + float2(-1.0, 1.0);
  o.pos = float4(ndc * at.z, (at.z - uPropDepth.x) * uPropDepth.y / (uPropDepth.y - uPropDepth.x), at.z);
  o.uvl = float3(v.uv, round(v.tint.a * 255.0));
  o.tint = pow(max(v.tint.rgb, 0.0), 2.2);
  o.thin = v.normal.w > 0.5 ? 1.0 : 0.0;
  return o;
}

struct PropLit {
  float4 col : SV_Target;
  uint cover : SV_Coverage;
};

// The lit surface of the model.
PropLit PropIn(PropOut v) {
  PropLit o;
  float4 tex = tPropTex.Sample(sWrap, v.uvl);
  // Parts of a surface that are see-through (railings, rigging painted into the
  // texture) cover that fraction of the samples.
  int samples = int(uPropWork.w);
  int kept = tex.a > 0.995 ? samples : int(tex.a * float(samples) + ign(v.pos.xy));
  if (kept <= 0 || (uPropAir.z > 0.5 && v.height < 0.0)) discard;
  o.cover = kept >= samples ? 0xffffffffu : (1u << uint(kept)) - 1u;

  float3 view = normalize(-v.at);
  float3 n = normalize(v.normal);
  // Thin parts such as sails are seen from both sides.
  if (dot(n, view) < 0.0) n = -n;
  // What floats is lit like the land beside it. What flies is lit like the clouds
  // around it: backlit in front of the light source, from the side far from it.
  float3 l = groundLight();
  float open = sunThrough();
  if (uPropLight.z > 0.5 && uPropLight.z < 1.5) {
    open = lerp(open, tShade.SampleLevel(sClamp, uPropLight.xy, 0.0).r, smoothstep(0.05, 0.16, l.y));
  } else {
    l = lightDir(normalize(float3(uPropRow[0].w, uPropRow[1].w + uPropDepth.z, uPropRow[2].w)));
    if (uPropLight.z > 1.5) open = 1.0;
  }
  // Fog above it takes the direct light.
  open *= fogSun(uPropAir.y);
  float facing = saturate(dot(n, l));
  // Through sailcloth and feathers part of the light shines when it comes from
  // behind.
  if (v.thin > 0.5) facing += 0.25 * saturate(-dot(n, l));
  // The sky lights from above; a vertical surface sees half of it, and on the side
  // away from the light source the darker half. uSkyLand is the light on open
  // ground including what the landscape around it returns; a surface in the air
  // gets less than half of that. From below comes what water or land return.
  float up = 0.5 + 0.5 * n.y;
  // In the air the sky is all around, and the direct light is less yellow than at
  // the ground: white wings stay white.
  float aloft = uPropLight.z > 0.5 && uPropLight.z < 1.5 ? 0.0 : 1.0;
  float3 direct = lerp(uSunLand, luma(uSunLand) * float3(1.03, 1.0, 0.95), 0.55 * aloft);
  float3 light = direct * (facing * open) + uSkyLand * lerp(lerp(0.10, 0.16, aloft), lerp(0.42, 0.48, aloft), up) +
                 (uSunLand * (saturate(l.y) * open) + uSkyLand * 0.5) * ((1.0 - up) * uPropLight.w);
  float3 lit = tex.rgb * v.tint * light;
  // Paint and glass show the light source as a highlight.
  if (uPropAir.w > 0.0 && facing > 0.0) lit += uSunLand * (open * uPropAir.w * pow(saturate(dot(n, normalize(l + view))), 64.0));
  o.col = float4(lit, 1.0);
  return o;
}

// How much of something at distance dist is left uncovered at a position in the
// image: land that is nearer covers it, and beyond the sea horizon the water
// covers what is below it.
float propOpen(float2 p, float dist) {
  float open = 1.0;
  if (uGround.x > 0.5) {
    float tx = (p.x - 0.5 * uView.x) / uFocal, ty = (uHorizon - p.y) / uFocal;
    float2 lu = float2(0.5 + tx * 1.1, (uStrip.x - ty) / uStrip.y);
    if (lu.y > 0.0) {
      float lod = log2(max(STRIP_W / uRes.x, 1.0));
      float there = tLandA.SampleLevel(sClamp, lu, lod).a;
      if (there > 0.002 && DIST_MIN * pow(DIST_MAX / DIST_MIN, tLandB.SampleLevel(sClamp, lu, lod).z) < dist) open *= 1.0 - there;
    }
  }
  if (uGround.y > 0.5 && uPass < 0.5 && dist * dist > 2.0 * R_EFF * uGround.z) {
    float rowEdge = uHorizon + uFocal * sqrt(2.0 * uGround.z / R_EFF);
    open *= 1.0 - saturate((p.y - rowEdge) * (uRes.y / uView.y) + 0.5);
  }
  return open;
}

// What lies between the eye and something at distance dist: the air (through:
// what it transmits), the part of the clouds in front (front: what they
// transmit, glow: their own light), the rain in front (clear: what it
// transmits) and the fog (seen: what it transmits).
void propBetween(float2 p, float2 uv, float dist, out float3 through, out float front, out float3 glow, out float clear, out float seen) {
  through = airThrough(dist, uGround.w, uPropAir.y);
  seen = uFog.x > 0.0 ? exp(-fogAlong(dirOf(p), dist, uPropAir.y)) : 1.0;
  float4 cloud;
  float2 cloudZ;
  cloudsAt(p, dirOf(p), cloud, cloudZ);
  float tBack;
  float3 lightBack;
  cloudSplit(cloud, cloudZ, dist, front, glow, tBack, lightBack);
  float3 rain = uStorm.y > 0.001 ? tRain.SampleLevel(sClamp, uv, 0.0).rgb : 0.0;
  clear = rain.r > 1e-4 ? pow(max(1.0 - rain.r, 1e-4), rainBefore(rain, dist)) : 1.0;
}

// Sets the work image into the sky.
float4 Prop(float4 pos : SV_Position) : SV_Target {
  // Several pixels of the work image per pixel, or, for something very near, less
  // than one.
  int per = max(int(uPropWork.z), 1), samples = int(uPropWork.w);
  float2 cell = floor(pos.xy - uPropWork.xy);
  int2 base = uPropWork.z >= 1.0 ? int2(cell) * per : int2((cell + 0.5) * uPropWork.z);
  float4 sum = 0.0;
  for (int j = 0; j < per; j++) {
    for (int i = 0; i < per; i++) {
      for (int k = 0; k < samples; k++) sum += tPropWork.Load(base + int2(i, j), k);
    }
  }
  sum /= float(per * per * samples);
  if (sum.a < 0.002) discard;
  float2 uv = pos.xy / uRes;
  float2 p = uv * uView;
  float dist = uPropAir.x;
  float cover = sum.a * propOpen(p, dist);
  if (cover < 0.002) discard;

  // The air in between as in front of the land, then the clouds, then the rain.
  float3 haze = tHaze.SampleLevel(sClamp, float2(uv.x, 0.5), 0.0).rgb;
  haze = lerp(haze, luma(haze) * float3(0.86, 0.89, 0.95) * (1.0 - 0.4 * uStorm.x), 0.9 * uLand.y);
  float3 veil = lerp(haze, luma(haze) * float3(0.94, 0.98, 1.05), 0.35);
  float3 through, glow;
  float front, clear, seen;
  propBetween(p, uv, dist, through, front, glow, clear, seen);
  float3 col = sum.rgb / sum.a * uExposure * through + veil * (1.0 - through);
  col = col * front + glow;
  col = col * seen + fogLight(dirOf(p), sunThrough()) * (1.0 - seen);
  col = col * clear + rainLight() * ((1.0 - clear) * uExposure);
  if (uShow > 0.5) return float4(develop(col) + (hash12(pos.xy + 0.37) + hash12(pos.yx * 1.31 + 7.7) - 1.0) / 255.0, cover);
  return float4(col, cover);
}

// A lamp on a ship or an aircraft: a point of light, drawn as a small soft disc.
// uPropRect: its position in pixels of the target and the radius of the disc;
// uPropWing: its light. In the image for the eye it is laid over what is there
// by its brightness, because that image holds finished colors; into the others
// its light is added, and a counts it as lamp light for the water.
float4 Lamp(float4 pos : SV_Position) : SV_Target {
  float2 d = (pos.xy - uPropRect.xy) / uPropRect.z;
  float2 uv = pos.xy / uRes;
  float2 p = uv * uView;
  float dist = uPropAir.x;
  float3 through, glow;
  float front, clear, seen;
  propBetween(p, uv, dist, through, front, glow, clear, seen);
  float3 col = uPropWing.rgb * through * (exp(-dot(d, d)) * propOpen(p, dist) * front * clear * seen * uExposure);
  float peak = max(col.r, max(col.g, col.b));
  if (peak < 1e-4) discard;
  if (uShow > 0.5) return float4(pow(col / peak, 1.0 / 2.2), 1.0 - exp(-1.6 * peak));
  return float4(col, luma(col));
}

// Search for a good start: a different wind shift per pixel, the result is the
// transmittance in the immediate surroundings of the sun (r) and exactly toward
// its center (g).
float4 Pick(float4 pos : SV_Position) : SV_Target {
  cloudShift = pos.x / uCount * uSpan;
  cloudAge = 0.0;
  lightJit = 0.5;
  float3 s = uSunDir;
  float3 a = normalize(cross(s, float3(0.0, 1.0, 0.0)));
  float3 b = cross(a, s);
  float mid = through(s);
  float near = mid;
  [loop] for (uint i = 0u; i < 8u; i++) {
    float ang = float(i) * 0.7853982;
    near += through(normalize(s + ((i & 1u) == 0u ? 0.012 : 0.026) * (cos(ang) * a + sin(ang) * b)));
  }
  return float4(near / 9.0, mid, 0.0, 1.0);
}

// Shadow of the clouds on the water, at a quarter of the edge length: per pixel,
// what the clouds transmit of the light source's light at the position of the
// water there. Toward the sea horizon the rows are kilometers apart; therefore
// four samples per pixel spread over its height, otherwise it would flicker
// there.
float4 Shade(float4 pos : SV_Position) : SV_Target {
  cloudShift = uCloudT;
  cloudAge = uEvolve;
  float3 l = groundLight();
  if (pos.y / uRes.y * uView.y < uHorizon - 4.0 || l.y <= 0.04) return 1.0;
  float eye = uGround.z;
  float dip = sqrt(2.0 * eye / R_EFF);
  float sum = 0.0;
  for (int i = 0; i < 4; i++) {
    float2 p = (pos.xy + float2(0.0, (float(i) - 1.5) * 0.25)) / uRes * uView;
    float3 v = dirOf(p);
    float3 vd = normalize(float3(v.x, min(v.y, -dip * 1.0005 * length(v.xz)), v.z));
    float down = -vd.y * (R_EFF + eye), q0 = 2.0 * R_EFF * eye + eye * eye;
    float dist = q0 / (down + sqrt(max(down * down - q0, 0.0)));
    sum += cloudShade(vd * dist, l);
  }
  return 0.25 * sum;
}

// The strip of the mirror image above the horizon, summed column by column from
// top to bottom. From two taps follows the light of a section of a column of any
// height, without it spreading sideways: this way the lights of a city stay as
// long columns in rough water.
// uSpan: top row of the strip, uCount: its height in rows.
// In two stages, so that not every pixel taps all rows above it.
// First stage: the sum within a block of 16 rows, including the pixel's own row.
float4 ColumnBlock(float4 pos : SV_Position) : SV_Target {
  int x = int(pos.x), top = int(uSpan), row = int(pos.y);
  float4 sum = 0.0;
  [loop] for (int j = row & ~15; j <= row; j++) sum += tUpper.Load(int3(x, top + j, 0));
  return sum;
}

// Second stage: plus the blocks above (from the first stage, bound as tColumn).
// The pixel's own row counts half: the value applies to its center.
float4 Column(float4 pos : SV_Position) : SV_Target {
  int x = int(pos.x), top = int(uSpan), row = int(pos.y);
  float4 sum = tColumn.Load(int3(x, row, 0)) - 0.5 * tUpper.Load(int3(x, top + row, 0));
  [loop] for (int b = (row >> 4) - 1; b >= 0; b--) sum += tColumn.Load(int3(x, b * 16 + 15, 0));
  return sum;
}

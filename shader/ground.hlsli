// The ground: land from real terrain data, and water.
//
// The land is available as a strip around the horizon, precomputed from an
// elevation model and aerial imagery (tools\ground): per point the color of the
// surface, its orientation, the distance, and how high the terrain blocks the
// horizon in the direction of the light source. From this it is lit here for any
// time of day; the air in between increases with distance, denser deep in a
// valley than at altitude.
//
// The water is a surface up to the sea horizon. Its waves are a sum of many
// individual wave trains. What a pixel resolves of them tilts the surface; what
// is finer enters the reflection as roughness (after Bruneton, Neyret,
// Holzschuch 2010: Real-time Realistic Ocean Lighting using Seamless Transitions
// from Geometry to BRDF). So it glitters in the foreground and becomes calm
// toward the sea horizon, without flickering.
//
// The wind determines the sea state: in a calm the water lies smooth, only the
// swell raises and lowers it; with the wind first come ripples in patches, then
// ever longer waves, finally whitecaps.

Texture2D tLandA : register(t15);   // color of the surface, coverage
Texture2D tLandB : register(t16);   // orientation of the surface (2), distance, open sky
Texture2D tLandC : register(t17);   // elevation of the horizon in four directions toward the light source
Texture2D tLandD : register(t18);   // lights of the night, gloss
Texture2D tUpper : register(t19);   // sky and land without water: what is mirrored
SamplerState sWide : register(s2);  // reads stretched surfaces correctly

static const int WAVES = 160;
static const int SWELL = 8;   // the last of these are swell: gusts do not change them
cbuffer WaveC : register(b1) {
  float4 uWave[WAVES];   // height, angular frequency, wave vector (x, z)
};

static float sunOpen = 1.0;                   // how much of the light source's light the clouds let through
static float landLights = 0.0;                // brightness of the land's lights at this position in the image
static float landHeight = 0.0;                // height above the sea of the land at this position in the image

static const float R_EFF = 7433e3;            // Earth radius, enlarged to account for refraction
static const float STRIP_W = 3840.0;          // width of the strip in points
static const float DIST_MIN = 50.0, DIST_MAX = 400000.0;
static const float HOR_MIN = -30.0, HOR_SPAN = 90.0;
static const float LIGHT_MAX = 24.0;          // this much luminance the image of the lights holds (tools\ground\panorama.py)
static const float GLITTER_KNEE = 0.45;       // above this brightness the glitter path grows only with a square root
// The lights of a city are in reality many times brighter than the screen can
// show them. In smooth water they are mirrored as they appear in the image; when
// rough water spreads their light over long columns, their true brightness
// counts, otherwise the columns would be far too dark.
static const float LIGHT_TRUE = 8.0;

// Mean density of the air on the path from the eye (height h0) to a point
// (height h1), as a fraction of the density at sea level.
float meanDensity(float h0, float h1, float scale) {
  float x = (h1 - h0) / scale;
  float k = abs(x) < 1e-3 ? 1.0 - 0.5 * x : (1.0 - exp(-x)) / x;
  return exp(-h0 / scale) * k;
}

// What the air transmits of the light over this distance.
float3 airThrough(float dist, float h0, float h1) {
  return exp(-(K_RLH * meanDensity(h0, h1, SH_RLH) + K_MIE * 1.1 * uLand.z * meanDensity(h0, h1, SH_MIE)) * dist);
}

// Fog: a layer over the ground. Well below its top (uFog.y) it has its full
// density, above it the density falls to nothing over a transition of depth
// uFog.z. This is that density integrated from height h upward, in meters of
// full density.
float fogAbove(float h) {
  float x = (h - uFog.y) / uFog.z;
  return uFog.z * (x < -30.0 ? -x : log(1.0 + exp(min(-x, 30.0))));
}

// Optical depth of the fog along a straight path of length dist between the
// heights h0 and h1 above the sea.
float fogDepth(float dist, float h0, float h1) {
  float dh = h1 - h0;
  float mean = abs(dh) < 0.5 ? 1.0 / (1.0 + exp(clamp((0.5 * (h0 + h1) - uFog.y) / uFog.z, -30.0, 30.0))) : (fogAbove(h0) - fogAbove(h1)) / dh;
  return uFog.x * max(mean, 0.0) * dist;
}

// Thin fog lies in banks that drift with the wind at the ground: its density
// around a position (meters from the eye: right, ahead), relative to the mean.
float fogBank(float2 at) {
  float2 q = (at - uGust.xy) * 0.0006;
  return 0.15 + 1.7 * smoothstep(0.25, 0.75, tNoise.SampleLevel(sWrap, float3(q.x, 0.37, q.y), 0.0).r);
}

// Optical depth of the fog along the view ray v from the eye over dist to the
// height h1, banks included. The banks are read at four places along the part of
// the path that the eye sees into, each counted by the density of the layer at
// its height.
float fogAlong(float3 v, float dist, float h1) {
  float h0 = uGround.w;
  float tau = fogDepth(dist, h0, h1);
  if (uFog.w <= 0.001 || tau <= 1e-4) return tau;
  float reach = min(1.0, 2.5 / tau);
  float weight = 1e-5, sum = 1e-5;
  for (int i = 0; i < 4; i++) {
    float f = (float(i) + 0.5) * 0.25 * reach;
    float w = 1.0 / (1.0 + exp(clamp((lerp(h0, h1, f) - uFog.y) / uFog.z, -30.0, 30.0)));
    weight += w;
    sum += w * fogBank(v.xz * (f * dist));
  }
  return tau * lerp(1.0, sum / weight, uFog.w);
}

// The same along a view ray into the sky: up to where the ray leaves the layer.
// The Earth curves away below the ray, so even a level ray leaves it eventually.
float fogSky(float3 v) {
  float clear = uFog.y + 6.0 * uFog.z, h0 = uGround.w;
  if (h0 >= clear) return 0.0;
  float t = min(R_EFF * (-v.y + sqrt(v.y * v.y + 2.0 * (clear - h0) / R_EFF)), 4e5);
  return fogAlong(v, t, clear);
}

// What the fog above a point at height h lets through of the direct light.
float fogSun(float h) {
  if (uFog.x <= 0.0) return 1.0;
  return exp(-uFog.x * fogAbove(h) / max(sin(uLand.x), 0.06));
}

float landOpen(float2 p);

// The light that fog scatters toward the viewer in view direction v: its own
// brightness, and around the light source a glare, because fog scatters most of
// the light forward. open: how much of the light source the clouds let through.
// Behind land the source glares far less; a remainder comes over the ridge.
float3 fogLight(float3 v, float open) {
  float c = dot(v, uSunDir);
  float glare = uFogCol.w * open * (0.25 + 0.75 * landOpen(uSun));
  // Fog is never even: wisps drift through it with the wind, and upward, where
  // the layer thins and its light comes from, it is brighter.
  float2 q = (v.xz * (260.0 / (abs(v.y) + 0.2)) - uGust.xy) * 0.0012;
  float wisp = tNoise.SampleLevel(sWrap, float3(q.x, 0.61, q.y), 0.0).r;
  float3 own = uFogCol.rgb * ((0.80 + 0.40 * wisp) * lerp(0.80, 1.12, saturate(0.5 + 2.5 * v.y)));
  // A city behind the fog lights it from within: the light of its lamps is not
  // lost, it arrives as a warm band low over the horizon.
  if (uLand.w > 0.5) own += float3(1.0, 0.68, 0.38) * (uStrip.z * 0.55 * exp(-pow(abs(v.y - 0.035) / 0.06, 1.5)));
  return (own + uSunLand * (glare * (hg(c, 0.82) + 0.3 * hg(c, 0.4)))) * uExposure;
}

float3 octDecode(float2 f) {
  f = f * 2.0 - 1.0;
  float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
  float t = saturate(-n.z);
  n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
  return normalize(n);
}

// Shadow of the cumulus clouds on a point of the ground (at: its position
// relative to the eye, meters): what they transmit of the light on the path to
// the light source. A few samples across the layer, each averaged over an area
// as large as its step; so the shadow has soft edges and moves with the wind.
float cloudShade(float3 at, float3 s) {
  if (uClouds <= 0.001) return 1.0;
  float3 from = float3(0.0, R_PLANET + EYE_ALT - uCloudLift, 0.0) + at;
  float t0 = max(raySphere(from, s, R_PLANET + EYE_ALT + CUMULUS_LO).y, 0.0);
  float t1 = raySphere(from, s, R_PLANET + EYE_ALT + CUMULUS_HI).y;
  if (t1 <= t0) return 1.0;
  const int N = 7;
  float len = (t1 - t0) / float(N), od = 0.0;
  float mip = clamp(log2(len / 195.0), 0.0, 5.5), lod = clamp(log2(len / 52.0), 0.0, 5.0);
  for (int i = 0; i < N; i++) {
    Weather w = cloudWeather(from + s * (t0 + len * (float(i) + 0.5)), mip, 0.0);
    if (any(w.dens.xy > 1e-5)) {
      float grad;
      od += cloudMedia(w, false, lod, grad) * len;
    }
  }
  return exp(-od);
}

// Direction to the light source for the ground: sideways where it appears in the
// image, at the elevation where it actually is.
float3 groundLight() {
  float az = atan2(uSunDir.x, uSunDir.z);
  float c = cos(uLand.x);
  return float3(sin(az) * c, sin(uLand.x), cos(az) * c);
}

// How clear of land a position in the image is: 1 = nothing in front.
float landOpen(float2 p) {
  if (uGround.x < 0.5) return 1.0;
  float tx = (p.x - 0.5 * uView.x) / uFocal, ty = (uHorizon - p.y) / uFocal;
  float2 uv = float2(0.5 + tx * 1.1, (uStrip.x - ty) / uStrip.y);
  if (uv.y <= 0.0) return 1.0;
  return 1.0 - tLandA.SampleLevel(sClamp, uv, 3.0).a;
}

// The land at a position in the image: lit color and coverage, plus its distance
// in meters.
float4 land(float2 p, float3 v, float3 haze, out float dist) {
  dist = 0.0;
  float tx = (p.x - 0.5 * uView.x) / uFocal, ty = (uHorizon - p.y) / uFocal;
  float2 uv = float2(0.5 + tx * 1.1, (uStrip.x - ty) / uStrip.y);
  if (uv.y <= 0.0) return 0.0;
  float lod = log2(max(STRIP_W / uRes.x, 1.0));
  float4 a = tLandA.SampleLevel(sClamp, uv, lod);
  if (a.a < 0.002) return 0.0;
  float4 b = tLandB.SampleLevel(sClamp, uv, lod);
  float4 c = tLandC.SampleLevel(sClamp, uv, lod);
  // Wet land is darker; snowfall does not wet it.
  a.rgb *= 1.0 - 0.22 * uStorm.z * (1.0 - uSnow.x);
  float3 m = octDecode(b.xy);
  float3 n = float3(m.x, m.z, m.y);   // stored with the vertical as the axis
  dist = DIST_MIN * pow(DIST_MAX / DIST_MIN, b.z);
  float flat0 = dist * length(v.xz);
  float h1 = uGround.w + v.y * dist + flat0 * flat0 / (2.0 * R_EFF);
  landHeight = h1;

  // Snow. In the mountains it lies above the snow line on everything that is not
  // too steep; the line is ragged, and rock faces stay bare. In a city it lies on
  // roofs and open ground when it lies at the viewer's level at all.
  float ragged = tNoise.SampleLevel(sWrap, float3(v.x * dist * 0.0009, 0.21, v.z * dist * 0.0009), 0.0).r - 0.5;
  float lies = uLand.w > 0.5 ? uSnow.z * smoothstep(0.55, 0.85, n.y)
                             : smoothstep(-140.0, 160.0, h1 - uSnow.y + 420.0 * ragged) * smoothstep(0.30, 0.62, n.y + 0.25 * ragged);
  a.rgb = lerp(a.rgb, float3(0.86, 0.89, 0.94), lies);

  // Shadow: if the light source is lower than the horizon that the terrain leaves
  // in its direction from this point, the point is in shadow.
  float off = degrees(atan2(uSunDir.x, uSunDir.z));
  float f = clamp((off + 27.0) / 18.0, 0.0, 2.999);
  float hor = f < 1.0 ? lerp(c.r, c.g, f) : f < 2.0 ? lerp(c.g, c.b, f - 1.0) : lerp(c.b, c.a, f - 2.0);
  hor = HOR_MIN + HOR_SPAN * hor;
  float shadow = smoothstep(hor - 0.7, hor + 0.7, degrees(uLand.x));
  float3 l = groundLight();
  // Clouds cast their shadow on the land. If the light source is low, the path of
  // the light through the cloud layer is too long to trace point by point: then
  // what counts is whether it is behind clouds as seen from the viewer.
  float open = sunOpen;
  float steep = smoothstep(0.05, 0.16, l.y);
  if (steep > 0.0 && shadow > 0.0) open = lerp(open, cloudShade(v * dist, l), steep);
  // Fog above the point takes the direct light and returns part of it from all
  // sides.
  float clear = fogSun(h1);
  // In addition to the direct light and that of the sky there is the light
  // reflected by the sunlit landscape all around: it also brightens what lies in
  // shadow.
  float3 lit = a.rgb * (uSunLand * (saturate(dot(n, l)) * shadow * open * clear + 0.055 * saturate(sin(uLand.x)) * (0.5 * open + 0.5 * sunOpen)
                                    + 0.22 * saturate(sin(uLand.x)) * sunOpen * (1.0 - clear)) + uSkyLand * b.a);
  // Lightning lights up the land around it.
  if (uFlash.w > 0.001) {
    float3 toFlash = uFlash.xyz - v * dist;
    float reach = length(toFlash);
    lit += a.rgb * uFlashCol.rgb * (uFlash.w * 0.05 * saturate(0.3 + 0.7 * dot(n, toFlash / reach)) / (1.0 + pow(reach / 4500.0, 2.0)));
  }
  float3 lamps = 0.0;
  if (uLand.w > 0.5) {
    // A city: glass reflects the sky behind the viewer, more strongly at grazing
    // angles. At night the windows are lit.
    float4 d = tLandD.SampleLevel(sClamp, uv, lod);
    float fres = 0.09 + 0.6 * pow(1.0 - saturate(dot(n, -v)), 4.0);
    lit += uSkyLand * (1.15 * d.a * fres);
    lamps = d.rgb * d.rgb * (LIGHT_MAX * uStrip.z);
    lit += lamps;
  }

  float3 through = airThrough(dist, uGround.w, h1);
  // The haze in front of distant land is paler than the sky above it: light from
  // all directions mixes in it.
  float3 veil = lerp(haze, luma(haze) * float3(0.94, 0.98, 1.05), 0.35);
  landLights = luma(lamps * through) * uExposure * a.a;
  return float4(lit * uExposure * through + veil * (1.0 - through), a.a);
}

float erfApprox(float x) {
  float s = sign(x), a = abs(x);
  float t = 1.0 / (1.0 + 0.3275911 * a);
  float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-a * a);
  return s * y;
}

// Mean reflectance of a rough water surface (Bruneton et al.).
float meanFresnel(float cosV, float sigma) {
  return pow(1.0 - cosV, 5.0 * exp(-2.69 * sigma)) / (1.0 + 22.7 * pow(sigma, 1.5));
}

float lambdaOf(float cosT, float sigmaSq) {
  float v = cosT / sqrt(max((1.0 - cosT * cosT) * (2.0 * sigmaSq), 1e-12));
  float erfc = 2.0 * exp(-v * v) / (2.319 * v + sqrt(4.0 + 1.52 * v * v));
  return max(0.0, (exp(-v * v) - v * 1.7724539 * erfc) / (2.0 * v * 1.7724539));
}

// Fraction of the light from direction l that the rough surface reflects toward
// the eye. n: mean normal, tx and ty: axes in the surface, sigmaSq: variance of
// the slope along both.
float glitter(float3 l, float3 v, float3 n, float3 tx, float3 ty, float2 sigmaSq) {
  float3 h = normalize(l + v);
  float zh = dot(h, n);
  float zetaX = dot(h, tx) / zh, zetaY = dot(h, ty) / zh;
  float zl = max(dot(l, n), 0.01), zv = max(dot(v, n), 0.01);
  float pdf = exp(-0.5 * (zetaX * zetaX / sigmaSq.x + zetaY * zetaY / sigmaSq.y)) / (6.2831853 * sqrt(sigmaSq.x * sigmaSq.y));
  float2 vt = float2(dot(v, tx), dot(v, ty)), lt = float2(dot(l, tx), dot(l, ty));
  float cv2 = vt.x * vt.x / max(dot(vt, vt), 1e-9), cl2 = lt.x * lt.x / max(dot(lt, lt), 1e-9);
  float sv = sigmaSq.x * cv2 + sigmaSq.y * (1.0 - cv2), sl = sigmaSq.x * cl2 + sigmaSq.y * (1.0 - cl2);
  float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(v, h)), 5.0);
  float zh2 = zh * zh;
  return fresnel * pdf / ((1.0 + lambdaOf(zl, sl) + lambdaOf(zv, sv)) * zv * zh2 * zh2 * 4.0);
}

// Individual glints: at small scale water flashes instead of shining evenly. A
// grid on the water, each cell flashes at its own rhythm; on average the
// brightness stays the same.
float glints(float2 x, float cell, float t) {
  float2 id = floor(x / cell);
  float h = hash12(id), h2 = hash12(id + 37.7);
  float s = 0.5 + 0.5 * sin(t * (2.0 + 5.0 * h2) + h * 6.2831853);
  s *= s;
  s *= s;
  return s * s * 5.09;
}

// The cells are about as large as the water that a pixel covers crosswise.
float sparkle(float2 x, float across, float t) {
  float l = log2(max(across * 1.4, 1e-4)), l0 = floor(l);
  return lerp(glints(x, exp2(l0), t), glints(x, exp2(l0 + 1.0), t + 3.0), l - l0);
}

// Whitecaps. In a fresh wind individual waves break: a bright front runs with
// the wind for a few seconds and drags a trail of foam behind it, then both
// fade. For this the water is divided into cells (three grids on top of each
// other); in each one a wave breaks again and again at a random position, at its
// own rhythm, more often and wider with more wind. Front and trail are ellipses.
// What is computed is the fraction they take of the distance that a pixel covers
// in the view direction: this way nothing flickers in the distance, where
// patches become fine bright lines.
static const float2 FOAM_CELL[3] = {float2(30.0, 17.0), float2(33.0, 19.5), float2(31.5, 18.2)};   // cells of the grids, meters along the wind and across
static const float FOAM_TURN[5] = {6.0, 7.5, 8.0, 9.0, 10.0};   // seconds per cycle, divisors of the hour: uTime wraps every hour

float4 hash42(float2 p) {
  float4 p4 = frac(float4(p.xyxy) * float4(0.1031, 0.1030, 0.0973, 0.1099));
  p4 += dot(p4, p4.wzxy + 33.33);
  return frac((p4.xxyz + p4.yzzw) * p4.zywx);
}

// Fraction that an ellipse (center c, semi-axes r) takes of the segment of
// length len around q in direction dir; toward its edge it fades out softly.
float ellipseShare(float2 q, float2 dir, float len, float2 c, float2 r) {
  float2 o = (q - c) / r, d = dir / r;
  float dd = dot(d, d);
  float t0 = -dot(o, d) / dd;
  float2 nearest = o + d * t0;
  float gap = dot(nearest, nearest);
  if (gap >= 1.0) return 0.0;
  float reach = sqrt((1.0 - gap) / dd);
  float part = min(t0 + reach, 0.5 * len) - max(t0 - reach, -0.5 * len);
  return saturate(part / len) * saturate((1.0 - gap) * 3.0);
}

// The foam of the wave currently breaking in a cell. q: position in the axes of
// the wind (along it, across), dir: view direction in those axes, len: distance
// the pixel covers there, odds: how often a wave breaks, lace: how dense the
// foam is at this position, in the front and in the trail.
// Most cells are empty at the moment, and most pixels lie far from the wave:
// both are detected quickly.
float foamCell(float2 q, float2 dir, float len, float2 id, float2 cell, float salt, float odds, float2 lace) {
  float foam = 0.0;
  float4 h = hash42(id + salt);
  float period = FOAM_TURN[int(h.x * 4.999)];
  float turn = uTime / period + h.y;
  float n = floor(turn);
  float age = (turn - n) * period;
  n = fmod(n, 3600.0 / period);
  float4 e = hash42(id * 1.37 + float2(7.31 * n + salt, 3.17 * n + 11.0));
  float live = 1.6 + 2.0 * e.y;
  float fade = 1.0 + 0.8 * frac(e.y * 5.13 + h.z);
  [branch] if (e.x <= odds && age < live + fade) {
    float size = 0.6 + 0.05 * uWind.y;
    float speed = (2.0 + 0.25 * uWind.y) * (0.8 + 0.4 * h.w);
    float run = speed * min(age, live);
    // The crest breaks at one point first, then along its whole width.
    float width = (2.5 + 8.0 * e.z * e.z) * size * (0.3 + 0.7 * smoothstep(0.0, 0.9, age));
    float2 off = q - (id + float2(frac(e.w * 7.31 + h.z), e.w)) * cell;
    float2 slack = 0.5 * len * abs(dir) + 0.1;
    [branch] if (abs(off.y) < 0.5 * width + slack.y && off.x > -1.5 - slack.x && off.x < run + 1.5 + slack.x) {
      float tail = min(run, speed * (0.8 + 0.9 * frac(e.z * 3.71 + h.x)));
      float gone = 1.0 - smoothstep(live - 0.2, live + fade, age);
      float a = smoothstep(0.0, 0.3, age) * lace.x * ellipseShare(off, dir, len, float2(run, 0.0), float2(0.4 * size + 0.15, 0.5 * width));
      float b = 0.5 * lace.y * ellipseShare(off, dir, len, float2(run - 0.5 * tail, 0.0), float2(0.5 * tail + 0.2, 0.42 * width));
      foam = gone * max(a, b);
    }
  }
  return foam;
}

// A wave of the wake: its direction, wave number, phase and steepness (amplitude
// times wave number). Like the waves of the wind: it tilts the surface where the
// pixel resolves it, otherwise it only roughens it.
void wakeWave(float2 dir, float k, float phase, float steep, float2 side, float2 fwd, float across, float along, inout float2 slope, inout float2 rough) {
  if (steep <= 1e-4) return;
  float2 part = float2(dot(dir, side), dot(dir, fwd));
  float foot = length(float3(part * float2(across, along), part.x * along * 0.3));
  float seen = smoothstep(3.0, 7.0, 6.2831853 / (k * foot));
  slope += dir * (steep * seen * cos(phase));
  rough += (0.5 * steep * steep * (1.0 - seen * seen)) * part * part;
}

// The wake of ship or boat i (uWake). In the frame of the vessel: s ahead of its
// middle, c to its right. Three parts, as photographs show them:
// - Kelvin's wedge: the waves it raises keep pace with it and fill a wedge of
//   19.5 degrees to each side behind the bow. Across the track run the
//   transverse waves, as long as a wave must be to travel at its speed
//   (2 pi U^2 / g); along the edges of the wedge the diverging waves crowd, at
//   35 degrees to the track and two thirds that length.
// - The bow wave: white water pushed aside along the forward part of the hull.
// - The turbulent wake behind the stern: white close behind it, then a long band
//   in which the churning has smoothed out the ripples (calm): it mirrors the sky
//   like still water.
// A band narrower than the water a pixel covers is spread over that water with
// the same total: far out it remains a faint line instead of flickering.
void wakeOf(int i, float2 x, float2 side, float2 fwd, float across, float along, float lod, inout float2 slope, inout float2 rough, inout float calm,
            inout float foam) {
  float4 a = uWake[2 * i], b = uWake[2 * i + 1];
  float2 f = a.zw, r = float2(f.y, -f.x);
  float2 d = x - a.xy;
  float s = dot(d, f), c = dot(d, r);
  float speed = b.x, L = b.y, B = b.z;
  float aft = L - s, behind = -s - L;
  if (aft < -B - 2.0 || behind > 25.0 * L + 1500.0 || abs(c) > 0.4 * max(aft, 0.0) + 3.0 * B + 20.0) return;
  // How much water the pixel covers across the track and along it.
  float overC = abs(dot(fwd, r)) * along + abs(dot(side, r)) * across;
  // A ship churns the water white, a small boat hardly.
  float churn = pow(saturate((speed - 1.0) / 9.0), 1.5);

  // Behind the stern: the band of churned water, widening.
  float back = max(behind, 0.0);
  float wide = B * (1.0 + 1.5 * (1.0 - exp(-back / (4.0 * L)))) + 0.01 * back;
  float wEff = sqrt(wide * wide + 0.25 * overC * overC);
  float band = exp(-c * c / (wEff * wEff)) * (wide / wEff) * smoothstep(-0.5 * L, 0.0, behind);
  float still = band * exp(-back / (20.0 * L + 600.0));
  calm = max(calm, still);
  slope *= 1.0 - 0.35 * still;
  rough *= 1.0 - 0.7 * still;
  float grain = tNoise.SampleLevel(sWrap, float3(x / 7.0, 0.53), lod).a;
  float white = band * churn * exp(-back / (2.0 * L + 3.0 * speed)) * (0.55 + 0.9 * grain);

  // Along the bow.
  if (aft > -0.05 * L && aft < 0.8 * L) {
    float ahead = max(aft, 0.0);
    float hull = B * sqrt(saturate(ahead / (0.5 * L)));
    float w = 0.2 * B + 0.4 + 0.04 * ahead;
    float off = abs(c) - hull - 0.1 * ahead;
    float wBow = sqrt(w * w + 0.25 * overC * overC);
    white += 0.9 * churn * exp(-off * off / (wBow * wBow)) * (w / wBow) * exp(-ahead / (0.3 * L)) * (0.7 + 0.6 * grain);
  }
  foam = 1.0 - (1.0 - foam) * (1.0 - saturate(white));

  // Kelvin's wedge.
  if (aft > 0.0) {
    float k = 9.81 / max(speed * speed, 0.05);
    float lambda = 6.2831853 / k;
    float edge = 0.354 * aft;
    float grow = smoothstep(0.0, 0.6 * L + 0.5 * lambda, aft);
    float inside = smoothstep(edge, 0.7 * edge, abs(c)) * grow * rsqrt(1.0 + aft / (2.0 * L));
    wakeWave(f, k, k * aft, 0.05 * inside, side, fwd, across, along, slope, rough);
    float2 dir = f * 0.8165 + r * (c < 0.0 ? -0.5774 : 0.5774);
    float kd = 1.5 * k;
    float rim = (abs(c) - edge) / (0.05 * aft + 0.6 * lambda);
    float cusp = exp(-rim * rim) * grow * pow(1.0 + aft / (2.0 * L), -0.33);
    wakeWave(dir, kd, kd * dot(d - f * L, dir), 0.08 * cusp, side, fwd, across, along, slope, rough);
  }
}

// The water at a position in the image. cover: coverage (0 above the sea
// horizon), far: distance in meters.
float3 water(float2 p, float3 v, float3 haze, out float cover, out float far) {
  far = 0.0;
  float eye = uGround.z;
  // The sea horizon lies below the horizon by the dip of the horizon; its edge is
  // soft over one pixel.
  float dip = sqrt(2.0 * eye / R_EFF);
  float rowEdge = uHorizon + uFocal * dip;
  cover = saturate((p.y - rowEdge) * (uRes.y / uView.y) + 0.5);
  if (cover <= 0.0) return 0.0;

  float3 vd = normalize(float3(v.x, min(v.y, -dip * 1.0005 * length(v.xz)), v.z));
  // Intersection of the view ray with the sphere of the Earth. Computed in this
  // form because the usual one subtracts two nearly equal numbers: with an Earth
  // radius of millions of meters the distance was then accurate only to meters,
  // and the waves appeared as a jagged pattern in the image.
  float down = -vd.y * (R_EFF + eye), q0 = 2.0 * R_EFF * eye + eye * eye;
  float dist = q0 / (down + sqrt(max(down * down - q0, 0.0)));
  far = dist;
  float2 x = vd.xz * dist;
  float3 up = normalize(float3(x.x, R_EFF, x.y));
  float2 fwd = normalize(vd.xz), side = float2(fwd.y, -fwd.x);
  float cosV = max(dot(-vd, up), 1e-4);
  // This much water a pixel covers: crosswise and in the view direction.
  float pix = uView.x / (uRes.x * uFocal);
  float across = dist * pix, along = across / cosV;

  // The waves of the wind and the swell kept separate: gusts change only the
  // former.
  float2 slope = 0.0, swell = 0.0;
  float2 rough = 0.0, calm = float2(1e-6, 1e-6);   // unresolved slope: crosswise, in the view direction
  // For the foam: how high the position lies on the waves of three meters length
  // and more (each counted at its crest), and how much of that the pixel resolves.
  float crest = 0.0, crestSeen = 0.0, crestAll = 0.0;
  for (int i = 0; i < WAVES; i++) {
    float4 w = uWave[i];
    float k = length(w.zw);
    float2 dir = w.zw / k;
    float2 part = float2(dot(dir, side), dot(dir, fwd));
    // Distance that a pixel covers in the direction of travel of the wave. Plus its
    // depth along the crest: crests are hardly longer than the wave itself, averaged
    // over a larger depth the wave vanishes. Otherwise waves running crosswise would
    // remain and make every reflection jagged.
    float foot = length(float3(part * float2(across, along), part.x * along * 0.3));
    // A wave tilts the surface only when several pixels fall on it; barely resolved
    // it would appear as a stripe pattern in the image.
    float seen = smoothstep(3.0, 7.0, 6.2831853 / (k * foot));
    float steep = w.x * k;
    float sn = 0.0, cs = 0.0;
    if (seen > 0.0) sincos(dot(w.zw, x) - w.y * uTime, sn, cs);
    float2 tilt = (w.x * seen * cs) * w.zw;
    float2 fine = (0.5 * steep * steep * (1.0 - seen * seen)) * part * part;
    if (i < WAVES - SWELL) {
      slope += tilt;
      rough += fine;
      float big = steep * saturate(2.1 / k - 1.0);
      crest += big * seen * sn;
      crestSeen += 0.5 * big * big * seen * seen;
      crestAll += 0.5 * big * big;
    } else {
      swell += tilt;
      calm += fine;
    }
  }
  // Gusts: patches with more and with less wind move with it across the water. In
  // a light wind the water ripples only in individual patches and lies smooth in
  // between; in a fresh wind it is rough everywhere, somewhat more in the gusts.
  // Axes along the wind and across it; the foam below is fixed on the water in
  // them, the gust fields move.
  float2 sea = uSurf.zw, perp = float2(sea.y, -sea.x);
  float2 lee = float2(dot(x, sea), dot(x, perp));
  float gust = tNoise.SampleLevel(sWrap, float3((lee.x - uGust.z) * 0.0006, lee.y * 0.0017, uTime * (14.0 / 3600.0)), 0.0).r;
  float wind = lerp(2.4 * smoothstep(0.62, 0.9, gust), 0.45 + 1.1 * gust, saturate(uWind.y / 5.0));
  // Rain roughens the water at small scale: drops make rings.
  rough = rough * wind + calm + 0.010 * uStorm.z * (1.0 - uSnow.x);
  slope = slope * sqrt(wind) + swell;

  // Ships and boats under way leave their wake.
  float still = 0.0, wakeFoam = 0.0;
  [branch] if (uWake[1].w > 0.0) {
    float wakeLod = log2(max(sqrt(along * across), 0.02) * 14.0);
    wakeOf(0, x, side, fwd, across, along, wakeLod, slope, rough, still, wakeFoam);
    if (uWake[3].w > 0.0) wakeOf(1, x, side, fwd, across, along, wakeLod, slope, rough, still, wakeFoam);
  }

  // Whitecaps, more in the gusts. Far out a pixel covers many cells: there its
  // distance is divided into pieces; even farther out only the mean brightening
  // remains.
  float foam = 0.0;
  [branch] if (uWind.z > 0.0) {
    float odds = saturate(uWind.z * wind * sqrt(wind));
    // If only a few waves break, one of the three grids suffices, in which one
    // breaks more often instead.
    int grids = uWind.z < 0.17 ? 1 : (uWind.z < 0.34 ? 2 : 3);
    float each = odds * 3.0 / float(grids);
    float away = smoothstep(80.0, 180.0, along);
    [branch] if (away < 1.0) {
      // Noise bends the outlines and makes the foam patchy, the trail more than the
      // front. Read at full resolution: the pattern is fixed on the water, it does not
      // flicker.
      float lod = log2(max(sqrt(along * across), 0.02) * 14.0);
      float2 bend = float2(tNoise.SampleLevel(sWrap, float3(x / 9.0, 0.11), lod).a, tNoise.SampleLevel(sWrap, float3(x / 9.0 + 0.5, 0.71), lod).a) - 0.5;
      float4 grain = tNoise.SampleLevel(sWrap, float3(lee.x / 2.6, lee.y / 6.5, 0.37), lod - 1.0);
      float2 lace = float2(0.55 + 0.45 * smoothstep(0.62, 0.86, grain.g), smoothstep(0.40, 0.60, grain.a));
      float2 q = lee + bend * 6.0;
      float2 dirW = float2(dot(fwd, sea), dot(fwd, perp));
      int steps = clamp(int(ceil(along / 12.0)), 1, 8);
      float piece = along / float(steps);
      [loop] for (int st = 0; st < steps; st++) {
        float2 qs = q + dirW * ((float(st) + 0.5) * piece - 0.5 * along);
        float keep = 1.0;
        [loop] for (int grid = 0; grid < grids; grid++) {
          float2 cell = FOAM_CELL[grid];
          // A cell extends along the wind into the next one, and crosswise halfway into
          // the neighbor on both sides.
          float2 base = floor(qs / cell - float2(1.0, 0.5));
          for (int j = 0; j < 4; j++) {
            keep *= 1.0 - foamCell(qs, dirW, piece, base + float2(j & 1, j >> 1), cell, 17.0 * float(grid) + 3.0, each, lace);
          }
        }
        foam += 1.0 - keep;
      }
      foam /= float(steps);
      // Waves break at their crest: there the foam is dense, in the trough only
      // remnants stay. What the pixel does not resolve of the waves counts as an
      // average.
      if (crestAll > 1e-12) {
        float high = smoothstep(-0.1, 0.9, crest / sqrt(max(crestSeen, 1e-12)));
        foam = saturate(foam * lerp(1.0, 0.25 + 2.0 * high, saturate(1.5 * crestSeen / crestAll)));
      }
    }
    foam = lerp(foam, odds * uWind.w, away);
  }
  // In the smoothed band behind a ship no wave breaks.
  foam = 1.0 - (1.0 - foam * (1.0 - still)) * (1.0 - wakeFoam);

  float3 ex = normalize(cross(up, float3(0.0, 0.0, 1.0)));   // east of the surface
  float3 ez = cross(ex, up);
  float3 n = normalize(up - slope.x * ex - slope.y * ez);
  float3 tAlong = normalize(float3(fwd.x, 0.0, fwd.y) - up * dot(float3(fwd.x, 0.0, fwd.y), up));
  float3 tSide = cross(up, tAlong);
  float3 toEye = -vd;

  // Seen at a grazing angle, mainly the facets tilted toward the eye are visible:
  // on average the water therefore reflects sky far above the horizon, not the
  // horizon itself. That is why the sea is darker than the sky above the sea
  // horizon.
  float sigma = sqrt(rough.y);
  float xr = cosV / (sqrt(1.0 - cosV * cosV) * sigma);
  float big = 0.5 * (1.0 + erfApprox(xr * 0.70710678)), small0 = exp(-0.5 * xr * xr) * 0.39894228;
  float lean = sigma * big / max(xr * big + small0, 1e-4);
  float3 nMean = normalize(n - lean * tAlong);
  float3 r = reflect(vd, nMean);
  r.y = abs(r.y) + 0.004;
  r.z = max(r.z, 0.05);
  r = normalize(r);
  float2 q = pointOf(r) / uView;
  float spreadY = 2.0 * sigma * lerp(0.66, 1.0, saturate(xr * 0.5)) * uFocal / uView.y;
  float spreadX = 2.0 * sqrt(rough.x) * max(r.y, 0.05) * uFocal / uView.x;
  float4 mirror = tUpper.SampleGrad(sWide, saturate(q), float2(max(spreadX, 1.0 / uView.x), 0.0), float2(0.0, max(spreadY, 1.0 / uView.y)));
  // Rough water does not reflect each position once, but spread over a tall range
  // of the sky: depending on how the facets happen to be oriented. Close above the
  // horizon, where the land and its lights are, this range is summed exactly,
  // section by section (tColumn), each weighted by how many facets are oriented
  // that way and how much of them is visible. Above that the sky is smooth, one
  // tap suffices there. This way bright windows extend through the water as long
  // columns instead of vanishing.
  uint upW, upH;
  tUpper.GetDimensions(upW, upH);
  float rowsPer = float(upH) / uView.y;
  float lobe = smoothstep(1.5, 5.0, 2.0 * sigma * uFocal * rowsPer);
  float c0 = dot(toEye, n);
  float zLo = max(-2.5 * sigma, 1e-4 - c0), zHi = 3.0 * sigma;
  [branch] if (lobe > 0.0 && zHi > zLo + 1e-5) {
    float yTop = uHorizon - uFocal * uStrip.x, yHor = uHorizon;
    float lodX = log2(max(spreadX * float(upW), 1.0));
    float rows[7];
    float4 sums[7];   // light per color, plus the lamps' fraction of it
    for (int e = 0; e < 7; e++) {
      float3 ne = normalize(n - (zLo + (zHi - zLo) * float(e) / 6.0) * tAlong);
      float3 re = reflect(vd, ne);
      rows[e] = uHorizon - re.y / max(re.z, 0.05) * uFocal;
      float vy = (clamp(rows[e], yTop, yHor) * rowsPer - uSpan) / uCount;
      sums[e] = tColumn.SampleLevel(sClamp, float2(saturate(q.x), vy), lodX);
    }
    float4 light = 0.0;
    float total = 0.0, skyW = 0.0, skyY = 0.0, lowW = 0.0;
    for (int m = 0; m < 6; m++) {
      float zeta = zLo + (zHi - zLo) * (float(m) + 0.5) / 6.0;
      float wgt = exp(-0.5 * zeta * zeta / (sigma * sigma)) * max(c0 + zeta, 0.0);
      float len = max(rows[m] - rows[m + 1], 1e-3);
      light += (sums[m] - sums[m + 1]) * (wgt / (len * rowsPer));
      float above = saturate((yTop - rows[m + 1]) / len);
      skyW += wgt * above;
      skyY += wgt * above * 0.5 * (min(rows[m], yTop) + rows[m + 1]);
      lowW += wgt * saturate((rows[m] - yHor) / len);
      total += wgt;
    }
    if (skyW > 1e-6 * total) {
      float mid = skyY / skyW;
      light += skyW * tUpper.SampleGrad(sWide, float2(saturate(q.x), saturate(mid / uView.y)), float2(max(spreadX, 1.0 / uView.x), 0.0),
                                        float2(0.0, max(0.7 * (yTop - mid) / uView.y, 1.0 / uView.y)));
    }
    // What would be reflected downward hits water again: it shows what is just above
    // the sea horizon.
    if (lowW > 1e-6 * total) light += lowW * tUpper.SampleLevel(sClamp, float2(saturate(q.x), (yHor - 3.0) / uView.y), 2.0);
    light /= max(total, 1e-9);
    // The farther the water spreads the light of the lamps, the more their true
    // brightness counts.
    float spread = smoothstep(4.0, 30.0, 2.0 * sigma * uFocal * rowsPer);
    light.rgb *= 1.0 + (LIGHT_TRUE - 1.0) * spread * saturate(light.a / max(luma(light.rgb), 1e-5));
    mirror.rgb = lerp(mirror.rgb, light.rgb, lobe);
  }

  float fres = 0.02 + 0.98 * meanFresnel(saturate(dot(toEye, n)), sqrt(rough.x + rough.y));
  float3 col = fres * mirror.rgb;
  // Clouds cast their shadow on the water: where it lies, the light of the light
  // source is missing. If the source is low, the path of the light through the
  // clouds is too long to trace position by position: then, as with the land, what
  // counts is whether it is behind clouds as seen from the viewer.
  float direct = lerp(sunOpen, tShade.SampleLevel(sClamp, p / uView, 0.0).r, smoothstep(0.05, 0.16, groundLight().y)) * fogSun(uGround.w - eye);
  // The light that returns from the depth.
  col += (1.0 - fres) * uSea.rgb * (uSkyLand + uSunLand * (0.35 * saturate(sin(uLand.x)) * direct)) * uExposure;
  // The glitter path of the light source: only where its light reaches the water.
  // If it is behind the land, there is no path.
  if (uSunDir.y > 0.0) {
    float open = direct * landOpen(uSun);
    float g = glitter(uSunDir, toEye, n, tSide, tAlong, rough);
    float3 shine = uSunLand * (3.14159265 * g * open * smoothstep(0.0, 0.012, uSunDir.y) * uExposure);
    // The path is a thousand times brighter than the water beside it. Unchanged it
    // would appear in the image as a white area with a hard edge; the eye instead
    // sees a gradient with a bright core. Therefore, above a threshold the
    // brightness grows only with a square root. The glitter path of the moon stays
    // silver.
    float knee = GLITTER_KNEE * (1.0 - 0.6 * uSrcMoon);
    float peak = max(shine.r, max(shine.g, shine.b));
    if (peak > knee) shine *= knee * pow(max(peak / knee, 1.0), 0.38) / peak;
    // In the foreground the glints are visible individually, toward the sea horizon
    // they merge.
    col += shine * lerp(1.0, sparkle(x, across, uTime), saturate(3.0 * sqrt(cosV)));
  }
  // Foam scatters the light like snow, slightly darker.
  if (foam > 0.0) {
    float3 white = (uSkyLand * 0.85 + uSunLand * ((0.25 + 0.75 * saturate(sin(uLand.x))) * direct)) * (0.62 * uExposure);
    col = lerp(col, white, foam);
  }
  float3 through = airThrough(dist, uGround.w, uGround.w - eye);
  return col * through + haze * (1.0 - through);
}

// Clouds and rain enclose the ground in space. Along each view ray it is known
// at what distance they lie on average and how far they spread around it. The
// part of them in front of a surface at distance dist is laid in front of it
// (transmittance tFront, light lightFront), the rest stays behind: a summit thus
// enters a cloud gradually, and distant ground disappears in the rain, instead
// of everything lying either in front or behind.
void cloudSplit(float4 cloud, float2 cloudZ, float dist, out float tFront, out float3 lightFront, out float tBack, out float3 lightBack) {
  float part = 0.0;
  if (cloudZ.x > 0.0 && cloud.a < 0.999) {
    float deep = max(cloudZ.y, 0.05 * cloudZ.x) * 1000.0;
    part = smoothstep(-1.7, 1.7, (dist - cloudZ.x * 1000.0) / deep);
  }
  float whole = max(1.0 - cloud.a, 1e-4);
  tFront = pow(max(cloud.a, 1e-4), part);
  tBack = pow(max(cloud.a, 1e-4), 1.0 - part);
  lightFront = cloud.rgb * ((1.0 - tFront) / whole);
  lightBack = cloud.rgb * ((1.0 - tBack) / whole);
}

// The fraction of the rain that falls in front of a surface at distance dist
// (meters).
float rainBefore(float3 rain, float dist) {
  if (rain.r <= 1e-4) return 0.0;
  float deep = max(rain.b, 0.08 * rain.g) * 1000.0;
  return smoothstep(-1.7, 1.7, (dist - rain.g * 1000.0) / max(deep, 1.0));
}

// How much of the light source's light the clouds let through to the viewer: if
// it is behind a cloud, the ground lacks its direct light.
float sunThrough() {
  float2 su = airUv(uSun), sd = 1.6 * uSunR / float2(uView.x + 2.0 * GUARD, uView.y);
  return 0.2 * (tCloud.SampleLevel(sClamp, su, 0.0).a + tCloud.SampleLevel(sClamp, su + float2(sd.x, 0.0), 0.0).a +
                tCloud.SampleLevel(sClamp, su - float2(sd.x, 0.0), 0.0).a + tCloud.SampleLevel(sClamp, su + float2(0.0, sd.y), 0.0).a +
                tCloud.SampleLevel(sClamp, su - float2(0.0, sd.y), 0.0).a);
}

// Places the ground in the image. First the water up to the sea horizon, the
// land in front of it. sky: the sky without clouds, cloud: their light and their
// transmittance, cloudZ: their mean distance and its spread in km (0 = none),
// rain: coverage of the rain, its mean distance and its spread in km, col: the
// sky with clouds. near: the fraction of the rain that lies in front of the
// ground. bolt: the light of a lightning channel at distance boltFar; ground in
// front of it occludes it. fogTau arrives as the optical depth of the fog along
// the ray into the sky; where there is ground it becomes that up to the ground.
void ground(float2 p, float3 v, float2 uv, float3 sky, float4 cloud, float2 cloudZ, float3 rain, float boltFar, inout float3 col, inout float alpha,
            inout float near, inout float3 bolt, inout float fogTau) {
  landLights = 0.0;
  if (uGround.x + uGround.y < 0.5) return;
  // State of the clouds now: for their shadow on the land.
  cloudDrift = uDrift;
  cloudAge = uEvolve;
  float3 haze = tHaze.SampleLevel(sClamp, float2(uv.x, 0.5), 0.0).rgb;
  // Below clouds the haze is gray like them, not blue like the sky; in a
  // thunderstorm also dark.
  haze = lerp(haze, luma(haze) * float3(0.86, 0.89, 0.95) * (1.0 - 0.4 * uStorm.x), 0.9 * uLand.y);
  float tFront, tBack;
  float3 lightFront, lightBack;
  sunOpen = sunThrough();
  if (uGround.y > 0.5 && uPass < 0.5) {
    float cover, far;
    float3 w = water(p, v, haze, cover, far);
    cloudSplit(cloud, cloudZ, far, tFront, lightFront, tBack, lightBack);
    w = w * tFront + lightFront;
    near = lerp(near, rainBefore(rain, far), cover);
    bolt *= 1.0 - cover * step(far, boltFar);
    col = lerp(col, w, cover);
    sky = lerp(sky, w, cover);
    alpha *= 1.0 - cover;
    if (uFog.x > 0.0 && cover > 0.0) fogTau = lerp(fogTau, fogAlong(v, far, uGround.w - uGround.z), cover);
  }
  if (uGround.x > 0.5) {
    float dist;
    float4 g = land(p, v, haze, dist);
    if (g.a > 0.0) {
      cloudSplit(cloud, cloudZ, dist, tFront, lightFront, tBack, lightBack);
      col = lerp(sky * tBack + lightBack, g.rgb, g.a) * tFront + lightFront;
      landLights *= tFront;
      near = lerp(near, rainBefore(rain, dist), g.a);
      bolt *= 1.0 - g.a * step(dist, boltFar);
      alpha *= 1.0 - g.a;
      if (uFog.x > 0.0) fogTau = lerp(fogTau, fogAlong(v, dist, landHeight), g.a);
    }
  }
}

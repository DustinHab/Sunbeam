// The night sky: Milky Way, stars, planets and shooting stars. They are drawn
// into an image of their own, as light before the film curve; the final sky
// image (Face in sky.hlsl) lays it behind the clouds.
//
// The stars are shown as a sheet: azimuth runs evenly across the width, altitude
// follows the rows of the image by the same rule as for sun and moon
// (Renderer::rowOfAltitude). Around 35 degrees of altitude a degree is as wide
// as it is high.
//
// What is visible follows from one number, the faintest magnitude the eye still
// makes out at the zenith. It falls with twilight, moonlight and the lights of
// a city (Renderer::nightDepth); here the air toward the horizon and the
// brightness of the sky around each spot are added.

#include "common.hlsli"

cbuffer NightC : register(b0) {
  float2 uRes;
  float2 uView;
  float4 uEast;          // xyz: turns a direction of the star catalogue into east,
  float4 uNorth;         // north
  float4 uUp;            // and up at the place
  float4 uSheet;         // x: row of the horizon, y: focal length, logical pixels; z: fraction of the width per degree of azimuth; w: azimuth at the middle of the image, degrees
  float4 uLift;          // x: altitude in degrees that a pinhole image would reach at the top edge; y: size of a faint star, logical pixels; z: time in seconds
  float4 uDepth;         // x: faintest magnitude visible at the zenith; y: loss in magnitudes per air mass; z: brightness; w: how much the stars twinkle
  float4 uMilky;         // x: brightness of the Milky Way; y: 1 = the map is there; z: how much a bright sky around a spot hides what is there
  float4 uBody[8];       // planets: x, y in logical pixels; z: magnitude; w: how much it twinkles, as a fraction of a star
  float4 uBodyTint[8];
  float4 uTrail[4];      // shooting stars: where each appears (x, y) and where it burns out (z, w), logical pixels
  float4 uTrailPar[4];   // x: seconds since it appeared; y: seconds of its flight; z: seconds over which its trail fades; w: magnitude
  float4 uTrailLook[4];  // x: tint, 0 warm to 1 green; y: strength of a flare near the end
};

Texture3D tNoise : register(t0);
Texture2D tBase : register(t4);     // the sky without clouds: how bright the background is
Texture2D tMilky : register(t28);   // the Milky Way: right ascension to the right from 0 h, declination from +90 at the top; brightness as its square root

static const float RAD = 0.01745329252;
// The eye compresses: from one magnitude to the next the brightness in the image
// changes by 10^0.288 instead of 10^0.4.
static const float STEEP = 0.288;
static const float WHITE_AT = 2.5;   // a star of this magnitude just reaches white
static const float PEAK = 2.0;       // brighter ones do not get brighter but larger

// Brightness in the image of a point of magnitude mag when the faintest visible
// one has magnitude limit: zero at the limit.
float shine(float mag, float limit) {
  return pow(10.0, STEEP * (WHITE_AT - mag)) - pow(10.0, STEEP * (WHITE_AT - limit));
}

// Image row for an altitude in degrees, and the inverse.
float rowOf(float alt) {
  float top = uLift.x;
  if (alt < 0.0) return uSheet.x + uSheet.y * tan(-alt * RAD);
  float a = 90.0 - top;
  float u = (-top + sqrt(top * top + 4.0 * a * min(alt, 90.0))) / (2.0 * a);
  return uSheet.x - uSheet.y * tan(top * u * RAD);
}

float altOf(float y) {
  float e = atan2(uSheet.x - y, uSheet.y) / RAD;
  float u = e / uLift.x;
  return e < 0.0 ? e : min(e + (90.0 - uLift.x) * u * u, 90.0);
}

// The air a ray passes through, relative to the zenith (Kasten and Young 1989).
float airMass(float alt) {
  float h = max(alt, 0.0);
  return 1.0 / (sin(h * RAD) + 0.50572 * pow(h + 6.07995, -1.6364));
}

// Luminance of the sky where it is darkest.
float darkest() {
  float m = 1e9;
  for (int i = 0; i < 5; i++) {
    float x = 0.1 + 0.2 * float(i);
    m = min(m, luma(tBase.SampleLevel(sClamp, float2(x, 0.05), 0.0).rgb));
    m = min(m, luma(tBase.SampleLevel(sClamp, float2(x, 0.3), 0.0).rgb));
  }
  return max(m, 1e-6);
}

// Magnitudes lost at a position because the sky is brighter there than where it
// is darkest: around the moon, in the glow of twilight.
float glowLoss(float2 uv, float dark) {
  float here = luma(tBase.SampleLevel(sClamp, uv, 0.0).rgb);
  return uMilky.z * 2.5 * log10(max(here / dark, 1.0));
}

// The air close to the horizon takes more blue than red.
float3 reddened(float3 tint, float mass) {
  float x = uDepth.y * (mass - 1.0);
  float3 t = tint * float3(pow(10.0, 0.12 * x), 1.0, pow(10.0, -0.18 * x));
  return t / max(luma(t) / max(luma(tint), 1e-4), 1e-4);
}

// A value between -1 and 1 that changes smoothly, rate times per second,
// differently for every seed.
float flicker(float seed, float rate) {
  float t = uLift.z * rate + seed * 17.0;
  float i = floor(t), f = frac(t);
  f = f * f * (3.0 - 2.0 * f);
  return lerp(hash11(i + seed * 91.7), hash11(i + 1.0 + seed * 91.7), f) * 2.0 - 1.0;
}

struct Dot {
  float4 pos : SV_Position;
  float2 off : TEXCOORD0;                     // from its center, pixels
  nointerpolation float4 light : TEXCOORD1;   // rgb: its light at the center, a: width of its core in pixels
  nointerpolation float2 glare : TEXCOORD2;   // the glow around a bright one: strength relative to the center, radius in pixels
};

static const float2 CORNER[6] = {
  float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
  float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0),
};

Dot hidden() {
  Dot o;
  o.pos = float4(2.0, 2.0, 2.0, 1.0);
  o.off = 0.0;
  o.light = 0.0;
  o.glare = float2(0.0, 1.0);
  return o;
}

// A point of light at p (logical pixels) of brightness b. Up to white it is a
// small disc that gets brighter; beyond that it grows, and a glow forms around
// it, as around every glaring light.
Dot dotAt(float2 p, float b, float3 tint, uint corner) {
  float px = uRes.x / uView.x;
  float sigma = max(uLift.y * px, 0.55) * pow(max(b / PEAK, 1.0), 0.3);
  float peak = min(b, PEAK);
  float glare = b > 0.5 ? 0.006 * pow(max(b, 0.5), 0.9) : 0.0;
  float reach = 2.2 * sigma + 1.5 * px;
  float size = 3.3 * sigma + 1.0;
  if (glare > 0.002) size = max(size, reach * sqrt(max(pow(glare / 0.002, 0.6667) - 1.0, 0.0)) + 1.0);
  size = min(size, 96.0 * px);
  float2 c = CORNER[corner % 6u];
  float2 at = p * px + c * size;
  Dot o;
  o.pos = float4(at.x / uRes.x * 2.0 - 1.0, 1.0 - at.y / uRes.y * 2.0, 0.0, 1.0);
  o.off = c * size;
  o.light = float4(tint * peak, sigma);
  o.glare = float2(glare / max(peak, 1e-4), reach);
  return o;
}

struct StarIn {
  float3 dir : POSITION;    // unit vector in the catalogue: equator and equinox of J2000
  float mag : TEXCOORD0;
  float4 tint : COLOR0;     // its color; a: a number of its own
  uint corner : SV_VertexID;
};

Dot StarVS(StarIn s) {
  float3 v = float3(dot(uEast.xyz, s.dir), dot(uNorth.xyz, s.dir), dot(uUp.xyz, s.dir));
  float alt = asin(clamp(v.z, -1.0, 1.0)) / RAD;
  if (alt < 0.3) return hidden();
  float a = atan2(v.x, v.y) / RAD - uSheet.w;
  a -= 360.0 * floor((a + 180.0) / 360.0);
  float2 p = float2(uView.x * (0.5 + uSheet.z * a), rowOf(alt));
  if (p.x < -40.0 || p.x > uView.x + 40.0) return hidden();
  float mass = airMass(alt);
  float limit = uDepth.x - glowLoss(p / uView, darkest());
  float b = uDepth.z * shine(s.mag + uDepth.y * (mass - 1.0), limit);
  if (b <= 0.0) return hidden();
  // Twinkling: the air bends the light of a point back and forth, the more of it
  // lies in the way the more. Low and bright stars flash in colors too.
  float sway = uDepth.w * min(0.06 * pow(max(mass, 1.0), 1.1), 0.55);
  b *= max(1.0 + sway * flicker(s.tint.a, 9.0), 0.2);
  // Faint stars have no color to the eye.
  float3 tint = lerp(float3(1.0, 1.0, 1.0), s.tint.rgb, lerp(0.45, 1.0, smoothstep(0.05, 1.5, b)));
  float hue = sway * 0.5 * flicker(s.tint.a + 0.37, 13.0) * smoothstep(0.3, 2.0, b);
  tint *= float3(1.0 + hue, 1.0, 1.0 - hue);
  return dotAt(p, b, reddened(tint, mass), s.corner);
}

// The planets: where the image has them, by the rule of sun and moon. A planet
// is a small disc, not a point: it twinkles much less.
Dot BodyVS(uint vid : SV_VertexID) {
  uint i = vid / 6u;
  float4 body = uBody[i];
  if (body.z > 50.0) return hidden();
  float alt = altOf(body.y);
  if (alt < 0.2) return hidden();
  float mass = airMass(alt);
  float limit = uDepth.x - glowLoss(body.xy / uView, darkest());
  float b = uDepth.z * shine(body.z + uDepth.y * (mass - 1.0), limit);
  if (b <= 0.0) return hidden();
  float sway = uDepth.w * body.w * min(0.06 * pow(max(mass, 1.0), 1.1), 0.55);
  b *= max(1.0 + sway * flicker(0.13 * float(i) + 0.05, 9.0), 0.2);
  return dotAt(body.xy, b, reddened(uBodyTint[i].rgb, mass), vid);
}

float4 Star(Dot v) : SV_Target {
  float r2 = dot(v.off, v.off);
  float core = exp(-r2 / (2.0 * v.light.a * v.light.a));
  float wide = 1.0 + r2 / (v.glare.y * v.glare.y);
  float glare = v.glare.x / (wide * sqrt(wide));
  return float4(v.light.rgb * (core + glare), 1.0);
}

// ---- Milky Way ----

struct Whole {
  float4 pos : SV_Position;
  nointerpolation float dark : TEXCOORD0;
};

Whole WholeVS(uint id : SV_VertexID) {
  float2 p = float2((id << 1) & 2, id & 2);
  Whole o;
  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
  o.dark = darkest();
  return o;
}

// The map, read smoothly (cubic B-spline from four linear reads): it is shown
// enlarged, and plain linear reading would leave the grid of its pixels in the
// glow.
float3 milkyMap(float3 d) {
  float2 size;
  tMilky.GetDimensions(size.x, size.y);
  float2 uv = float2(frac(atan2(d.y, d.x) / 6.2831853), 0.5 - asin(clamp(d.z, -1.0, 1.0)) / 3.14159265);
  float2 st = uv * size - 0.5;
  float2 i = floor(st), f = st - i;
  float2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
  float2 w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
  float2 w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
  float2 w3 = f * f * f / 6.0;
  float2 g0 = w0 + w1, g1 = w2 + w3;
  float2 p0 = i - 0.5 + w1 / g0, p1 = i + 1.5 + w3 / g1;
  p0.y = clamp(p0.y, 0.5, size.y - 0.5);
  p1.y = clamp(p1.y, 0.5, size.y - 0.5);
  float3 s = g0.y * (g0.x * tMilky.SampleLevel(sWrap, float2(p0.x, p0.y) / size, 0.0).rgb + g1.x * tMilky.SampleLevel(sWrap, float2(p1.x, p0.y) / size, 0.0).rgb) +
             g1.y * (g0.x * tMilky.SampleLevel(sWrap, float2(p0.x, p1.y) / size, 0.0).rgb + g1.x * tMilky.SampleLevel(sWrap, float2(p1.x, p1.y) / size, 0.0).rgb);
  return s * s;
}

// Without a map: a band along the plane of the galaxy, wider and brighter toward
// its center, with clouds of distant stars and dark dust lanes from noise.
float3 milkyBand(float3 d) {
  static const float3 POLE = float3(-0.8677, -0.1981, 0.4560);      // north pole of the galaxy
  static const float3 CENTER = float3(-0.0549, -0.8734, -0.4838);   // and its center
  float b = asin(clamp(dot(d, POLE), -1.0, 1.0));
  float l = atan2(dot(d, cross(POLE, CENTER)), dot(d, CENTER));
  float inward = exp(-l * l / 1.2);
  float width = 0.10 + 0.10 * inward;
  float band = exp(-b * b / (2.0 * width * width));
  float3 q = d * 2.1 + 0.37;
  float clump = tNoise.SampleLevel(sWrap, q, 1.0).a;
  float lanes = tNoise.SampleLevel(sWrap, q * 2.3 + 0.2, 0.0).g;
  float body = band * (0.35 + 0.65 * inward) * saturate(0.35 + 1.5 * (clump - 0.42)) * saturate(1.5 - 1.7 * lanes * exp(-b * b / 0.004));
  return float3(0.62, 0.70, 1.0) * body;
}

float4 Milky(Whole v) : SV_Target {
  float2 p = v.pos.xy / uRes * uView;
  float alt = altOf(p.y);
  if (uMilky.x <= 0.0 || alt <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);
  float mass = airMass(alt);
  // The glow of countless faint stars only shows where the sky is dark.
  float limit = uDepth.x - glowLoss(p / uView, v.dark) - uDepth.y * (mass - 1.0);
  float seen = smoothstep(4.3, 6.6, limit);
  if (seen <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);
  float az = (uSheet.w + (p.x / uView.x - 0.5) / uSheet.z) * RAD;
  float ca = cos(alt * RAD);
  float3 e = float3(sin(az) * ca, cos(az) * ca, sin(alt * RAD));
  float3 d = e.x * uEast.xyz + e.y * uNorth.xyz + e.z * uUp.xyz;
  float3 glow = uMilky.y > 0.5 ? milkyMap(d) : milkyBand(d);
  // The map has the colors a camera records. The eye sees light this faint
  // almost without color: pale, slightly cool.
  glow = lerp(luma(glow) * float3(0.80, 0.88, 1.0), glow, 0.45);
  return float4(glow * (uMilky.x * seen * seen), 1.0);
}

// ---- Shooting stars ----
//
// A grain of dust burns up in a second or less. Its head runs along the path at
// an even pace; what it has passed keeps glowing for a moment. It brightens as
// it gets into denser air, and some flare up shortly before they go out.

struct Streak {
  float4 pos : SV_Position;
  nointerpolation float4 path : TEXCOORD0;   // start and end, pixels
  nointerpolation float4 par : TEXCOORD1;    // x: seconds since it appeared, y: of its flight, z: over which the trail fades, w: brightness
  nointerpolation float4 look : TEXCOORD2;   // rgb: its color, a: flare
  nointerpolation float2 width : TEXCOORD3;  // x: of the trail, y: how far around the path anything is drawn; pixels
};

Streak TrailVS(uint vid : SV_VertexID) {
  uint i = vid / 6u;
  float4 t = uTrail[i], par = uTrailPar[i];
  Streak o;
  o.pos = float4(2.0, 2.0, 2.0, 1.0);
  o.path = 0.0;
  o.par = 0.0;
  o.look = 0.0;
  o.width = 1.0;
  if (par.y <= 0.0) return o;
  float px = uRes.x / uView.x;
  float2 mid = 0.5 * (t.xy + t.zw);
  float mass = airMass(altOf(mid.y));
  // Something that moves has to be brighter than a star to be noticed.
  float limit = uDepth.x - glowLoss(mid / uView, darkest()) - 1.0;
  float b = uDepth.z * shine(par.w + uDepth.y * (mass - 1.0), limit);
  if (b <= 0.0) return o;
  float width = max(uLift.y * px, 0.55) * (1.0 + 0.25 * pow(max(b, 1.0), 0.4));
  // A bright one has a glow around its head: room for it.
  float reach = 4.0 * width + (b > 1.0 ? 20.0 * width : 3.0 * px);
  float2 from = t.xy * px, to = t.zw * px;
  float2 along = (to - from) / max(length(to - from), 1e-3);
  float2 across = float2(-along.y, along.x);
  float2 c = CORNER[vid % 6u];
  float2 at = (c.x < 0.0 ? from : to) + (along * c.x + across * c.y) * reach;
  o.pos = float4(at.x / uRes.x * 2.0 - 1.0, 1.0 - at.y / uRes.y * 2.0, 0.0, 1.0);
  o.path = float4(from, to);
  o.par = float4(par.xyz, b);
  float3 warm = float3(1.0, 0.86, 0.66), green = float3(0.72, 1.0, 0.86);
  o.look = float4(reddened(lerp(float3(1.0, 1.0, 1.0), lerp(warm, green, uTrailLook[i].x), 0.6), mass), uTrailLook[i].y);
  o.width = float2(width, reach);
  return o;
}

// How bright it burns along its path, 0 at the start to 1 at the end.
float burn(float s, float flare) {
  float e = (s - 0.9) / 0.04;
  return smoothstep(0.0, 0.1, s) * (0.3 + 0.7 * s * sqrt(s)) * (1.0 - smoothstep(0.88, 1.0, s)) + flare * exp(-e * e);
}

float4 Trail(Streak v) : SV_Target {
  float2 p = v.pos.xy;
  float2 a = v.path.xy, ab = v.path.zw - a;
  float len = max(length(ab), 1e-3);
  float2 dir = ab / len;
  float pace = len / v.par.y;                    // pixels per second
  float head = min(v.par.x * pace, len);         // how far the head has come
  // The nearest point of what it has passed so far.
  float q = clamp(dot(p - a, dir), 0.0, head);
  float2 gap = p - (a + dir * q);
  float age = v.par.x - q / pace;                // seconds since the head was there
  float core = exp(-dot(gap, gap) / (2.0 * v.width.x * v.width.x));
  // Most of the trail goes out at once; of a bright one a faint rest lingers.
  float wake = exp(-age / v.par.z) + 0.015 * smoothstep(3.0, 8.0, v.par.w) * exp(-age / (7.0 * v.par.z));
  float light = burn(q / len, v.look.a) * wake * core;
  // The head: a point with a small glow.
  if (v.par.x < v.par.y) {
    float2 h = p - (a + dir * head);
    float d2 = dot(h, h), w = 1.5 * v.width.x;
    float wide = 1.0 + d2 / (16.0 * w * w);
    // The glow ends before the edge of what is drawn.
    float within = 1.0 - smoothstep(0.5 * v.width.y, 0.95 * v.width.y, sqrt(d2));
    light += burn(head / len, v.look.a) * (0.7 * exp(-d2 / (2.0 * w * w)) + 0.04 * smoothstep(1.0, 8.0, v.par.w) * within / (wide * sqrt(wide)));
  }
  return float4(v.look.rgb * (min(v.par.w, 2.0 * PEAK) * light), 1.0);
}

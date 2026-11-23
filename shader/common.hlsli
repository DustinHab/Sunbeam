// Shared by all shaders. Screen coordinates are logical pixels with the origin
// at the top left, as Windows counts them at 100% scaling.

SamplerState sClamp : register(s0);   // linear, clamped at the edges
SamplerState sWrap : register(s1);    // linear, tiling (noise)

float4 Full(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

float sdRound(float2 p, float4 r, float rad) {
  float2 c = 0.5 * (r.xy + r.zw);
  float2 h = 0.5 * (r.zw - r.xy);
  rad = min(rad, min(h.x, h.y));
  float2 q = abs(p - c) - h + rad;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
}

float ign(float2 f) {
  return frac(52.9829189 * frac(dot(f, float2(0.06711056, 0.00583715))));
}

float hg(float c, float g) {
  float d = 1.0 + g * g - 2.0 * g * c;
  return (1.0 - g * g) / (12.566371 * d * sqrt(d));
}

float luma(float3 c) {
  return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float hash11(float p) {
  p = frac(p * 0.1031);
  p *= p + 33.33;
  p *= p + p;
  return frac(p);
}

float hash12(float2 p) {
  float3 p3 = frac(float3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return frac((p3.x + p3.y) * p3.z);
}

// Smooth 2D noise, 0 to 1.
float noise2(float2 p) {
  float2 i = floor(p), f = frac(p);
  float2 u = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(hash12(i), hash12(i + float2(1.0, 0.0)), u.x), lerp(hash12(i + float2(0.0, 1.0)), hash12(i + float2(1.0, 1.0)), u.x), u.y);
}

// Map of the lunar surface (NASA, Lunar Reconnaissance Orbiter): longitude to
// the right, latitude upward, the side facing us in the center. Plus the slope
// of the surface from the elevation data: r slope toward east, g toward north,
// 0.5 is level, the full range stands for -0.25 to +0.25.
Texture2D tMoon : register(t13);
Texture2D tMoonRelief : register(t14);

// The sun disc. d: distance from its center in logical pixels, r: its radius,
// squash: how much the air flattens it close to the horizon (1 = round). Toward
// the edge it gets darker and warmer: there the line of sight passes at a
// grazing angle through the outer, cooler layers, more strongly in blue than in
// red.
float3 sunFace(float2 d, float r, float squash, out float cover) {
  d.y /= squash;
  float dist = length(d);
  cover = 1.0 - smoothstep(r - 0.8, r + 0.8, dist);
  float mu = sqrt(max(1.0 - dist * dist / (r * r), 0.0));
  return (1.0 - float3(0.52, 0.66, 0.80) * (1.0 - mu)) * cover;
}

// The moon disc. d: distance from its center in logical pixels, r: its radius,
// lit: illuminated fraction, limb: direction of the bright side, spin: direction
// of the lunar north pole (both in radians, counterclockwise from up), squash:
// squash at the horizon. Returns the brightness per color, 1 = bright highlands
// in full light; cover = coverage of the disc including the dark side.
float3 moonFace(float2 d, float r, float lit, float limb, float spin, float squash, out float cover) {
  d.y /= squash;
  float dist = length(d);
  cover = 1.0 - smoothstep(r - 0.8, r + 0.8, dist);
  if (cover <= 0.0) return 0.0;
  float2 q = float2(d.x, -d.y) / r;
  float z = sqrt(max(1.0 - dot(q, q), 0.0));

  // Everything is computed in the coordinate grid of the moon, north pole up: the
  // position on the sphere and the direction from which the sun illuminates it.
  // The phase determines how far to the side the sun stands behind the moon.
  float cs = cos(spin), sn = sin(spin);
  float3 m = float3(q.x * cs + q.y * sn, -q.x * sn + q.y * cs, z);
  float ci = 2.0 * lit - 1.0, si = sqrt(max(1.0 - ci * ci, 0.0));
  float2 toSun = float2(-sin(limb) * si, cos(limb) * si);
  float3 L = float3(toSun.x * cs + toSun.y * sn, -toSun.x * sn + toSun.y * cs, ci);

  float2 at = float2(0.5 + atan2(m.x, m.z) / 6.2831853, 0.5 - asin(clamp(m.y, -1.0, 1.0)) / 3.14159265);
  // As fine as the disc is large in the image; otherwise the map would flicker.
  // Toward the edge it is compressed, because the sphere is seen at a grazing
  // angle: more and more of the map falls into one pixel there, so read more
  // coarsely.
  float crowd = log2(1.0 / max(z, 0.1));
  float3 seen = tMoon.SampleLevel(sClamp, at, log2(max(150.0 / r, 1.0)) + 0.8 * crowd).rgb;
  // The maria slightly darker than on the map: in the sky the contrast appears
  // stronger.
  float3 albedo = 1.9 * pow(max(seen, 0.0), 1.6);

  // Relief: craters and mountains tilt the surface. Fully lit, none of it is
  // visible; at the terminator, where the light falls at a grazing angle, they
  // cast light and shadow and make it jagged. Exaggerated, otherwise it would be
  // lost at this size.
  float2 slope = (tMoonRelief.SampleLevel(sClamp, at, log2(max(120.0 / r, 1.0)) + 0.7 + crowd).rg - 0.5019608) * 0.5 * 3.4;
  // It decreases toward the edge of the disc: there the surface is seen at a
  // grazing angle, and with an almost full moon the terminator lies exactly at the
  // edge of the disc. With full relief the outline got jags there.
  slope *= smoothstep(0.08, 0.55, z);
  float3 east = normalize(float3(m.z, 0.0, -m.x) + 1e-5);
  float3 north = cross(m, east);
  float3 bent = normalize(m - slope.x * east - slope.y * north);
  float flat0 = dot(m, L), mu0 = max(dot(bent, L), 0.0);
  // Lunar dust does not scatter like chalk: the full moon is equally bright up to
  // the edge instead of getting darker toward the edge.
  float shade = smoothstep(-0.10, 0.03, flat0) * saturate(2.0 * mu0 / (mu0 + z + 1e-3));
  shade = lerp(shade, pow(shade, 0.6), 0.5);

  // The dark side is not black: the Earth illuminates it faintly.
  float3 earth = float3(0.6, 0.75, 1.0) * (0.02 * (1.0 - lit) * luma(albedo));
  return (albedo * float3(1.0, 0.985, 0.95) * shade + earth) * cover;
}

float3 aces(float3 x) {
  return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// Filmic curve for display. Moderately bright areas keep their color instead of
// running to white channel by channel; only very bright ones such as the sun
// become white.
float3 develop(float3 c) {
  c = max(c, 0.0);
  float m = max(c.r, max(c.g, c.b));
  float3 keep = c * (aces(float3(m, m, m)).r / max(m, 1e-5));
  float3 t = lerp(aces(c), keep, 0.6 * (1.0 - smoothstep(3.0, 30.0, m)));
  t = pow(max(t, 0.0), 1.0 / 2.2);
  float l = luma(t);
  return saturate(lerp(float3(l, l, l), t, 1.22));
}

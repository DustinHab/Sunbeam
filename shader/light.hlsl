// Light. The viewer looks through haze that extends from the viewer (z = 0) to
// the rearmost plane (uZMax). Windows are plates at their own distance, the
// icons lie at uZMax, clouds and mountains at uZFar (as part of the background
// image, the same distance). The sun is infinitely far away.
//
// From this follows how a shadow runs: for haze at distance z, the path to the
// sun passes a plate at distance zp at the position
// p + (sun - p) * (1 - z / zp). Haze close to the plate is in shadow when the
// plate covers it in the image; haze close to the viewer only when the plate
// covers the sun itself. A shadow therefore extends from the plate to the edge
// of the image and gets steadily weaker, instead of ending after a fixed length.
//
// The plates are sorted by distance, the farthest first. uZS is the focal length
// of the view: it determines at what angle to the sun a position in the image
// lies.

#include "common.hlsli"

cbuffer LightC : register(b0) {
  float2 uRes;
  float2 uView;
  float2 uSun;
  float2 uLowRes;
  float uSunR;
  float uZS;
  float uZMax;
  float uZFar;
  float uG;
  int uN;
  float2 uTexel;        // logical pixels per texel: x sky, y icons
  float3 uDiscCol;      // luminance of the sun disc
  float uSigma;
  float3 uSunCol;
  float uJitter;
  float uAlpha;
  float uMist;
  int uSteps;
  float uThreshold;
  float3 uMistOff;
  float uExposure;
  float3 uMistScale;
  float uOverWin;
  float uBloom;
  float uRim;
  float uTime;
  float uDpr;
  float2 uSrcTexel;
  float uWeight;
  float uShade;         // how strongly shadows of windows and icons darken the sky
  float uOverSky;       // fraction of the scattered light that is added over open sky
  float uLinear;        // 1: linear output for HDR mode, otherwise encoded like the image
  float uWhite;         // brightness of white in the HDR image, in multiples of 80 cd/m²
  float uNear;          // up to this fraction of the distance the haze first increases, starting from the viewer
  float4 uCursor;       // area of the mouse pointer including clear margin, logical pixels; right less than left: no pointer
  float4 uCursorPar;    // logical pixels per texel, stillness (0 during fast motion), strength
  float4 uMoonPar;      // 1: the light source is the moon; illuminated fraction, direction of the bright side, direction of the north pole
  float4 uGlow;         // x: peak brightness in the HDR image as a multiple of white, 1 = off; y: squash of the disc at the horizon; z: 1 = the light source is lightning; w: how far the wind has carried the dust, logical pixels
  float4 uRect[32];
  float4 uPar[32];      // depth, opacity, corner radius, kind
  float4 uAux[32];      // uncertainty of the area, ghost of a window that has disappeared
  float4 uEdge[32];     // margin without light around each moving edge: left, top, right, bottom
};

Texture2D tSky : register(t0);     // sky without the sun disc, a = transmittance of the far plane
Texture2D tIcons : register(t1);   // r = coverage of the icons
Texture2D tA : register(t2);       // depending on the step: previous scattered light, scattered light, source of the glare
Texture2D tBloom : register(t3);
Texture3D tNoise : register(t4);
Texture2D tCursor : register(t5);  // r = coverage of the mouse pointer including clear margin

float2 uvOf(float2 q) {
  return q / uView;
}

// From the image value (sRGB) to linear brightness.
float3 toLinear(float3 c) {
  c = max(c, 0.0);
  float3 lo = c / 12.92, hi = pow((c + 0.055) / 1.055, 2.4);
  return float3(c.r <= 0.04045 ? lo.r : hi.r, c.g <= 0.04045 ? lo.g : hi.g, c.b <= 0.04045 ? lo.b : hi.b);
}

// Haze scatters broadly (air) and strongly forward (aerosols).
float phaseFn(float c) {
  return lerp(hg(c, 0.15), hg(c, uG), 0.92);
}

// Sky at a position including the disc of the light source. The texture is only
// half the size of the image; the disc is therefore added here with a sharp
// edge, the sun round, the moon in its phase.
float3 skyAt(float2 q) {
  float4 s = tSky.SampleLevel(sClamp, uvOf(q), 0.0);
  float2 d = q - uSun;
  float dist = length(d);
  if (dist > uSunR + 1.0) return s.rgb;
  float cover;
  float3 face = uMoonPar.x > 0.5 ? moonFace(d, uSunR, uMoonPar.y, uMoonPar.z, uMoonPar.w, uGlow.y, cover) : sunFace(d, uSunR, uGlow.y, cover);
  return s.rgb + uDiscCol * face * s.a;
}

// Far plane (clouds and mountains) and icons; the blur of the lookup is the
// penumbra.
float farVis(float2 p, float2 toSun, float z) {
  // Lightning is below the clouds, not behind them: they cast no shadow into its
  // light.
  if (uGlow.z > 0.5) return 1.0;
  float t = 1.0 - z / uZFar;
  return tSky.SampleLevel(sClamp, uvOf(p + toSun * t), log2(max(uSunR * t / uTexel.x, 1.0))).a;
}

float iconVis(float2 p, float2 toSun, float z) {
  float t = 1.0 - z / uZMax;
  return 1.0 - tIcons.SampleLevel(sClamp, uvOf(p + toSun * t), log2(max(uSunR * t / uTexel.y, 1.0))).r;
}

float layerVis(float2 p, float2 toSun, float z) {
  return iconVis(p, toSun, z) * farVis(p, toSun, z);
}

float windowVis(float2 p, float2 toSun, float z) {
  float vis = 1.0;
  [loop] for (int i = 0; i < uN; i++) {
    float4 par = uPar[i];
    if (par.x <= z) break;
    float t = 1.0 - z / par.x;
    float w = max(0.75, uSunR * t);
    vis *= 1.0 - par.y * (1.0 - smoothstep(-w, w, sdRound(p + toSun * t, uRect[i], par.z)));
  }
  return vis;
}

// Does the segment p .. p + d hit the rectangle r enlarged by m?
bool hits(float2 p, float2 d, float4 r, float m) {
  float4 g = r + float4(-m, -m, m, m);
  float2 s = float2(abs(d.x) < 1e-4 ? 1e-4 : d.x, abs(d.y) < 1e-4 ? 1e-4 : d.y);
  float2 t0 = (g.xy - p) / s, t1 = (g.zw - p) / s;
  float2 lo = min(t0, t1), hi = max(t0, t1);
  float a = max(lo.x, lo.y), b = min(hi.x, hi.y);
  return b >= max(a, 0.0) && a <= 1.0;
}

// Scattered light: view ray through the haze. r = light, g = depth of the end, b
// = light as it would be without windows and icons. The difference between b and
// r is what their shadows remove. a = shadow of the icons alone, by the same
// rule as that of the mouse pointer (see pointerShade).
float4 Scatter(float4 pos : SV_Position) : SV_Target {
  float2 p = pos.xy / uRes * uView;
  float2 toSun = uSun - p;

  // End of the view ray: frontmost opaque plate at this position.
  float zEnd = uZMax;
  int i;
  [loop] for (i = 0; i < uN; i++) {
    if (uPar[i].y > 0.999 && sdRound(p, uRect[i], uPar[i].z) < 0.0) zEnd = min(zEnd, uPar[i].x);
  }

  // Translucent plates in front of the end attenuate the view (at most four), and
  // only plates that the path to the sun can cross count as shadow.
  float4 tz = 1e9, tf = 1.0;
  int nt = 0;
  uint mask = 0u;
  [loop] for (i = 0; i < uN; i++) {
    float4 par = uPar[i];
    if (hits(p, toSun, uRect[i], uSunR + 1.0)) mask |= 1u << uint(i);
    if (par.y <= 0.999 && par.y > 0.001 && par.x < zEnd) {
      float c = 1.0 - smoothstep(-0.75, 0.75, sdRound(p, uRect[i], par.z));
      if (c > 0.0) {
        float f = 1.0 - par.y * c;
        if (nt == 0) { tz.x = par.x; tf.x = f; }
        else if (nt == 1) { tz.y = par.x; tf.y = f; }
        else if (nt == 2) { tz.z = par.x; tf.z = f; }
        else if (nt == 3) { tz.w = par.x; tf.w = f; }
        nt++;
      }
    }
  }

  float dz = zEnd / float(uSteps);
  float j = frac(ign(pos.xy) + uJitter);
  float acc = 0.0, accFar = 0.0, small = 0.0;
  // The angle to the sun depends only on the position in the image.
  float cosT = uZS / length(float3(toSun, uZS));
  float phase = phaseFn(cosT);

  [loop] for (int k = 0; k < uSteps; k++) {
    float z = (float(k) + j) * dz;

    float4 damp = lerp(1.0, tf, step(tz, z));
    float see = damp.x * damp.y * damp.z * damp.w;
    if (see < 0.002) continue;
    float fv = farVis(p, toSun, z);
    if (fv < 0.002) continue;

    float vis = 1.0;
    if (mask != 0u) {
      [loop] for (int m = 0; m < uN; m++) {
        float4 par = uPar[m];
        if (par.x <= z || vis < 0.002) break;
        if ((mask & (1u << uint(m))) != 0u) {
          float t = 1.0 - z / par.x;
          float w = max(0.75, uSunR * t);
          vis *= 1.0 - par.y * (1.0 - smoothstep(-w, w, sdRound(p + toSun * t, uRect[m], par.z)));
        }
      }
    }

    // Mist: the haze is unevenly dense, elongated in depth.
    float n = tNoise.SampleLevel(sWrap, float3(p, z) * uMistScale + uMistOff, 0.0).a;
    float dens = exp(uMist * 7.0 * (n - 0.5));
    float trans = exp(-uSigma * (z + (uZMax - z) / cosT));
    // Close to the viewer the haze is thinner; there a shadow ends softly.
    float e = see * fv * dens * phase * trans * smoothstep(0.0, uNear, z / uZMax);
    accFar += e;
    if (vis >= 0.002) {
      float iv = iconVis(p, toSun, z);
      acc += e * vis * iv;
      // As with the mouse pointer: the strongest shadow along the line of sight
      // counts, weaker the farther the position lies behind the icon, and only where
      // light would arrive at all.
      small = max(small, (1.0 - iv) * (z / uZMax) * vis * fv * see);
    }
  }
  acc *= uSigma * dz;
  accFar *= uSigma * dz;

  float4 prev = tA.Load(int3(pos.xy, 0));
  return lerp(prev, float4(acc, zEnd / uZMax, accFar, small), uAlpha);
}

static const float RIM_CAP = 1.6;
// This much of the rim light goes into the glare: the soft glow around the
// outline of a backlit window.
static const float HALO = 20.0;

// Rim light at position p: when backlit, the outline of a window glows,
// brightest where the sun is close behind the edge; the edge facing the sun also
// catches grazing light. reach widens the band for coarse grids.
float3 rimLight(float2 p, float reach) {
  int top = -1;
  float topSd = 1e9;
  [loop] for (int i = 0; i < uN; i++) {
    float sd = sdRound(p, uRect[i], uPar[i].z);
    if (sd < reach && uAux[i].y < 0.5) { top = i; topSd = sd; }
  }
  if (top < 0 || topSd <= -3.0 * reach || uRim <= 0.0) return 0.0;
  float band = smoothstep(-2.2 * reach, -0.5 * reach, topSd) * (1.0 - smoothstep(-0.3 * reach, 0.8 * reach, topSd));
  if (band <= 0.0) return 0.0;
  float4 rc = uRect[top];
  float2 ce = 0.5 * (rc.xy + rc.zw), h = 0.5 * (rc.zw - rc.xy);
  float rad = min(uPar[top].z, min(h.x, h.y));
  float2 dq = p - ce;
  float2 q = abs(dq) - h + rad;
  float2 sg = float2(dq.x < 0.0 ? -1.0 : 1.0, dq.y < 0.0 ? -1.0 : 1.0);
  float2 n = (q.x > 0.0 && q.y > 0.0) ? normalize(q) * sg : (q.x > q.y ? float2(sg.x, 0.0) : float2(0.0, sg.y));
  float2 pe = p + n * (2.0 * reach - topSd);
  float2 toSun = uSun - pe;
  float facing = max(dot(n, normalize(toSun)), 0.0);
  float z = uPar[top].x;
  float v = windowVis(pe, toSun, z) * layerVis(pe, toSun, z);
  float cosT = uZS / length(float3(toSun, uZS));
  float3 rim = uSunCol * (band * (0.45 + 0.55 * pow(facing, 0.7)) * v * phaseFn(cosT) * uRim * 0.8);
  return rim * (RIM_CAP / (RIM_CAP + luma(rim)));
}

// This much light the shadow of the mouse pointer removes from the haze close to
// the pointer. Such a small body shadows only a fraction of the haze along the
// line of sight; computed as for the windows, its shadow would be visible only
// close to the sun. Shape and falloff of the shadow follow the same computation,
// the strength is raised, and a remainder stays even in thin haze.
static const float POINTER_SHADE = 0.8;
static const float POINTER_FLOOR = 0.16;

float pointerCover(float2 q, float lod) {
  float2 uv = (q - uCursor.xy) / (uCursor.zw - uCursor.xy);
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0;
  return tCursor.SampleLevel(sClamp, uv, lod).r;
}

// Shadow of the mouse pointer at position p, 0 to 1. The path from p to the sun
// passes by the pointer or through it; the farther p lies behind it, the less
// haze along the line of sight is in its shadow.
float pointerShade(float2 p, float2 toSun) {
  if (uCursor.z <= uCursor.x) return 0.0;
  float2 c = 0.5 * (uCursor.xy + uCursor.zw);
  float rr = 0.5 * max(uCursor.z - uCursor.x, uCursor.w - uCursor.y);
  float R = max(length(toSun), 1.0);
  float2 dir = toSun / R;
  float along = dot(c - p, dir);
  float across = abs(dot(c - p, float2(-dir.y, dir.x)));
  if (along < -rr || across > rr) return 0.0;
  float s = 0.0;
  [loop] for (int k = 0; k < 8; k++) {
    float d = along + (float(k) / 3.5 - 1.0) * rr;
    float t = d / R;
    if (t > 0.0 && t < 0.97) {
      float lod = log2(max(uSunR * t / uCursorPar.x, 1.0));
      s = max(s, pointerCover(p + dir * d, lod) * (1.0 - t));
    }
  }
  return s * uCursorPar.z;
}

// Rim of the mouse pointer: when backlit, its outline glows like that of a
// window. It is drawn just outside, because the pointer itself lies above
// everything. Light hits it where it gets past windows, clouds and mountains;
// for this the pointer is placed just in front of the windows.
float3 pointerRim(float2 p) {
  if (uCursor.z <= uCursor.x || uRim <= 0.0) return 0.0;
  float2 size = uCursor.zw - uCursor.xy;
  float2 uv = (p - uCursor.xy) / size;
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0;
  float inside = tCursor.SampleLevel(sClamp, uv, 0.0).r;
  float soft = tCursor.SampleLevel(sClamp, uv, 2.0).r;
  float band = saturate(soft * 2.5) * (1.0 - inside);
  if (band <= 0.0) return 0.0;
  float2 st = 2.0 * uCursorPar.x / size;
  float2 g = float2(tCursor.SampleLevel(sClamp, uv + float2(st.x, 0.0), 2.0).r - tCursor.SampleLevel(sClamp, uv - float2(st.x, 0.0), 2.0).r,
                    tCursor.SampleLevel(sClamp, uv + float2(0.0, st.y), 2.0).r - tCursor.SampleLevel(sClamp, uv - float2(0.0, st.y), 2.0).r);
  float2 n = -g / max(length(g), 1e-4);
  float2 c = 0.5 * (uCursor.xy + uCursor.zw);
  float2 toSun = uSun - c;
  float facing = max(dot(n, normalize(toSun)), 0.0);
  float z = 0.5 * uZMax;
  float v = windowVis(c, toSun, z) * layerVis(c, toSun, z);
  float cosT = uZS / length(float3(toSun, uZS));
  float3 rim = uSunCol * (band * (0.35 + 0.65 * facing) * v * phaseFn(cosT) * uRim * 0.8 * uCursorPar.y * uCursorPar.z);
  return rim * (RIM_CAP / (RIM_CAP + luma(rim)));
}

// Source of the glare: what is directly visible of the bright sky, and the rim
// light of the windows.
float4 BloomPre(float4 pos : SV_Position) : SV_Target {
  float2 p = pos.xy / uRes * uView;
  float open = 1.0 - tIcons.SampleLevel(sClamp, uvOf(p), 0.0).r;
  [loop] for (int i = 0; i < uN; i++) {
    open *= 1.0 - uPar[i].y * (1.0 - smoothstep(-0.75, 0.75, sdRound(p, uRect[i], uPar[i].z)));
  }
  float3 c = min(skyAt(p), 120.0) * open;
  float l = max(c.r, max(c.g, c.b));
  float3 halo = rimLight(p, 1.5) * (HALO / 1.5);
  return float4(c * max(l - uThreshold, 0.0) / max(l, 1e-4) + halo, 1.0);
}

float4 BloomDown(float4 pos : SV_Position) : SV_Target {
  float2 uv = pos.xy / uRes;
  float2 t = uSrcTexel;
  float3 a = tA.SampleLevel(sClamp, uv + t * float2(-2.0, 2.0), 0.0).rgb;
  float3 b = tA.SampleLevel(sClamp, uv + t * float2(0.0, 2.0), 0.0).rgb;
  float3 c = tA.SampleLevel(sClamp, uv + t * float2(2.0, 2.0), 0.0).rgb;
  float3 d = tA.SampleLevel(sClamp, uv + t * float2(-2.0, 0.0), 0.0).rgb;
  float3 e = tA.SampleLevel(sClamp, uv, 0.0).rgb;
  float3 f = tA.SampleLevel(sClamp, uv + t * float2(2.0, 0.0), 0.0).rgb;
  float3 g = tA.SampleLevel(sClamp, uv + t * float2(-2.0, -2.0), 0.0).rgb;
  float3 h = tA.SampleLevel(sClamp, uv + t * float2(0.0, -2.0), 0.0).rgb;
  float3 i = tA.SampleLevel(sClamp, uv + t * float2(2.0, -2.0), 0.0).rgb;
  float3 j = tA.SampleLevel(sClamp, uv + t * float2(-1.0, 1.0), 0.0).rgb;
  float3 k = tA.SampleLevel(sClamp, uv + t * float2(1.0, 1.0), 0.0).rgb;
  float3 l = tA.SampleLevel(sClamp, uv + t * float2(-1.0, -1.0), 0.0).rgb;
  float3 m = tA.SampleLevel(sClamp, uv + t * float2(1.0, -1.0), 0.0).rgb;
  return float4(e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125, 1.0);
}

float4 BloomUp(float4 pos : SV_Position) : SV_Target {
  float2 uv = pos.xy / uRes;
  float2 t = uSrcTexel;
  float3 s = tA.SampleLevel(sClamp, uv, 0.0).rgb * 4.0;
  s += (tA.SampleLevel(sClamp, uv + t * float2(-1.0, 0.0), 0.0).rgb + tA.SampleLevel(sClamp, uv + t * float2(1.0, 0.0), 0.0).rgb
      + tA.SampleLevel(sClamp, uv + t * float2(0.0, -1.0), 0.0).rgb + tA.SampleLevel(sClamp, uv + t * float2(0.0, 1.0), 0.0).rgb) * 2.0;
  s += tA.SampleLevel(sClamp, uv + t * float2(-1.0, -1.0), 0.0).rgb + tA.SampleLevel(sClamp, uv + t * float2(1.0, -1.0), 0.0).rgb
     + tA.SampleLevel(sClamp, uv + t * float2(-1.0, 1.0), 0.0).rgb + tA.SampleLevel(sClamp, uv + t * float2(1.0, 1.0), 0.0).rgb;
  return float4(s / 16.0 * uWeight, 1.0);
}

// Over windows and icons the background is unknown; the computation uses the
// dark gray of a window. In front of pure black even a little light would appear
// as a gray veil.
static const float3 GROUND = float3(0.03, 0.03, 0.03);
// The light over a window gets at most this bright, so that close to the sun it
// keeps its gradient instead of becoming a white area.
static const float OVER_CAP = 0.3;
// And this bright over open sky.
static const float SKY_CAP = 0.25;

// The light as it is laid over the desktop, premultiplied: the color is added to
// the image below, the coverage darkens it first. This way shadows can make the
// sky darker and rays can make it brighter.
float4 Composite(float4 pos : SV_Position) : SV_Target {
  float2 p = pos.xy / uRes * uView;

  // quiet: where it is currently uncertain what is on the screen, the light stays
  // off. The position of a window arrives up to one frame later than Windows draws
  // it; without this, light would appear beside the edge while dragging.
  float zEnd = uZMax, cover = 0.0, quiet = 1.0;
  [loop] for (int i = 0; i < uN; i++) {
    float sd = sdRound(p, uRect[i], uPar[i].z);
    float4 aux = uAux[i], edge = uEdge[i], rc = uRect[i];
    if (aux.x > 0.0) quiet *= 1.0 - aux.x * (1.0 - smoothstep(0.0, 24.0, sd));
    if (p.y > rc.y - edge.y - 1.0 && p.y < rc.w + edge.w + 1.0) {
      if (edge.x > 0.0) quiet *= smoothstep(0.0, edge.x, abs(p.x - rc.x));
      if (edge.z > 0.0) quiet *= smoothstep(0.0, edge.z, abs(p.x - rc.z));
    }
    if (p.x > rc.x - edge.x - 1.0 && p.x < rc.z + edge.z + 1.0) {
      if (edge.y > 0.0) quiet *= smoothstep(0.0, edge.y, abs(p.y - rc.y));
      if (edge.w > 0.0) quiet *= smoothstep(0.0, edge.w, abs(p.y - rc.w));
    }
    if (aux.y > 0.5) continue;
    if (uPar[i].y > 0.999 && sd < 0.0) zEnd = min(zEnd, uPar[i].x);
    cover = max(cover, 1.0 - smoothstep(-0.75, 0.75, sd));
  }

  // Upscaling that respects window edges (same depth preferred).
  float zn = zEnd / uZMax;
  float2 lp = pos.xy / uRes * uLowRes - 0.5;
  float2 b = floor(lp), f = lp - b;
  int2 hi = int2(uLowRes) - 1;
  float3 sum = 0.0;
  float wsum = 0.0;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      float4 s = tA.Load(int3(clamp(int2(b) + int2(x, y), int2(0, 0), hi), 0));
      float wb = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
      float w = wb * (exp(-abs(s.g - zn) * 80.0) + 1e-4);
      sum += s.rba * w;
      wsum += w;
    }
  }
  float3 L = sum / max(wsum, 1e-6);

  // Scattered light in the haze, and what the shadows of windows and icons remove
  // from it. The glare arises in the eye and is equally strong everywhere.
  float3 lit = uSunCol * (L.x * uExposure);
  // In front of the lit part of the moon the haze is almost omitted: its veil
  // would otherwise remove the moon's detail, and the moon would appear as a white
  // spot in the sky. In front of the dark side and beside the disc it stays in
  // full; otherwise there would be a dark fringe around the moon, and the dark
  // side would be darker than the sky around it. Likewise where clouds or
  // mountains cover the moon.
  if (uMoonPar.x > 0.5) {
    float2 off = p - uSun;
    if (dot(off, off) < (uSunR + 1.0) * (uSunR + 1.0)) {
      float disc;
      float3 face = moonFace(off, uSunR, uMoonPar.y, uMoonPar.z, uMoonPar.w, uGlow.y, disc);
      lit *= 1.0 - 0.8 * saturate(5.0 * luma(face)) * tSky.SampleLevel(sClamp, uvOf(p), 0.0).a;
    }
  }
  float3 unshaded = uSunCol * (L.y * uExposure);
  float keep = saturate(L.x / max(L.y, 1e-6));
  // The mouse pointer casts its shadow into the haze like a small window. So do
  // the icons; their shadow comes from the scattered light.
  float pointer = 1.0 - (1.0 - pointerShade(p, uSun - p)) * (1.0 - saturate(L.z));
  keep *= 1.0 - POINTER_SHADE * pointer;
  lit *= 1.0 - POINTER_SHADE * pointer;
  float3 col = tBloom.SampleLevel(sClamp, pos.xy / uRes, 0.0).rgb * uBloom;
  float icons = tIcons.SampleLevel(sClamp, uvOf(p), 0.0).r;
  float3 sky = skyAt(p);

  float3 rim = rimLight(p, 1.0) + pointerRim(p);

  // Over windows and icons: the haze in front glows where the sun hits it.
  // The limit applies to the light as it would be without the shadows of the
  // windows; the fraction they let through thus stays an even gradient up to the
  // shadow boundary, also close to the sun.
  float3 full = uSunCol * (L.y * uExposure * uOverWin);
  full *= OVER_CAP / (OVER_CAP + luma(full));
  // The fraction acts on the final brightness: in the image the light thus falls
  // off evenly toward the shadow boundary instead of dropping off only just before
  // it.
  float3 under = GROUND + full * keep;
  float3 onWindow = (develop(GROUND + full) - develop(GROUND)) * keep + develop(under + col + rim) - develop(under);

  // Where the sky is open, everything acts before the filmic curve: in the shadow
  // of windows and icons the sky lacks light of the haze and gets darker, by the
  // fraction the shadow removes, as far as the haze there matters against the sky
  // at all. Where the sun hits the haze, light is added. This way it saturates
  // softly instead of clipping hard to white.
  float haze = luma(unshaded);
  float dark = 0.85 * uShade * (1.0 - keep) * haze / (haze + 0.6 * luma(sky) + 1e-4);
  dark = max(dark, POINTER_FLOOR * uShade * pointer);
  float3 was = develop(sky);
  // Close to the sun the haze scatters forward so strongly that it washed out the
  // sky to white over a wide area: the sun appeared as a milky ball in the image.
  // Its light therefore has an upper limit; far from the sun, where the rays lie,
  // this changes nothing.
  float3 veil = lit * uOverSky;
  veil *= SKY_CAP / (SKY_CAP + luma(veil));
  float3 want = develop(sky * (1.0 - dark) + veil + col + rim);
  float known = (1.0 - cover) * (1.0 - icons);

  // HDR mode: there Windows blends linearly, and it accepts color without coverage
  // only from a swap chain in HDR format (measured; from one with 8 bit it is
  // dropped entirely). The light is therefore added linearly, at the brightness
  // that white has in the HDR image.
  if (uLinear > 0.5) {
    float3 wasL = toLinear(was), wantL = toLinear(want);
    // An HDR display can go brighter than white. What glares in the image (the sun
    // disc, its innermost aureole, backlit cloud edges) really gets this brightness
    // instead of ending at white.
    float hot = smoothstep(2.5, 22.0, luma(sky * (1.0 - dark) + veil + col + rim));
    wantL *= 1.0 + (max(uGlow.x, 1.0) - 1.0) * hot;
    float3 fallL = (wasL - wantL) / max(wasL, 1e-5);
    float alphaL = saturate(max(fallL.r, max(fallL.g, fallL.b)));
    float3 onSkyL = max(wantL - wasL * (1.0 - alphaL), 0.0);
    float3 ground = develop(GROUND);
    float3 onWindowL = max(toLinear(ground + onWindow) - toLinear(ground), 0.0);
    return float4(lerp(onWindowL, onSkyL, known) * uWhite, alphaL * known) * quiet;
  }

  float3 fall = (was - want) / max(was, 0.004);
  float alpha = saturate(max(fall.r, max(fall.g, fall.b)));
  float3 onSky = max(want - was * (1.0 - alpha), 0.0);
  float3 added = lerp(onWindow, onSky, known);
  added += (ign(pos.xy) - 0.5) / 255.0;
  return float4(max(added, 0.0), alpha * known) * quiet;
}

// The light of a flash gets at most this bright: over windows and over open sky.
static const float FLASH_WIN = 0.3;
static const float FLASH_SKY = 0.07;

// The light of a flash over the desktop. The same computation as for the sun,
// with the lightning as the light source: its light falls through the haze,
// windows, icons and the mouse pointer cast shadows into it, their edges light
// up. Only light is added, laid over the finished light image of the sun.
float4 Flash(float4 pos : SV_Position) : SV_Target {
  float2 p = pos.xy / uRes * uView;
  float zEnd = uZMax, cover = 0.0, quiet = 1.0;
  [loop] for (int i = 0; i < uN; i++) {
    float sd = sdRound(p, uRect[i], uPar[i].z);
    float4 aux = uAux[i], edge = uEdge[i], rc = uRect[i];
    if (aux.x > 0.0) quiet *= 1.0 - aux.x * (1.0 - smoothstep(0.0, 24.0, sd));
    if (p.y > rc.y - edge.y - 1.0 && p.y < rc.w + edge.w + 1.0) {
      if (edge.x > 0.0) quiet *= smoothstep(0.0, edge.x, abs(p.x - rc.x));
      if (edge.z > 0.0) quiet *= smoothstep(0.0, edge.z, abs(p.x - rc.z));
    }
    if (p.x > rc.x - edge.x - 1.0 && p.x < rc.z + edge.z + 1.0) {
      if (edge.y > 0.0) quiet *= smoothstep(0.0, edge.y, abs(p.y - rc.y));
      if (edge.w > 0.0) quiet *= smoothstep(0.0, edge.w, abs(p.y - rc.w));
    }
    if (aux.y > 0.5) continue;
    if (uPar[i].y > 0.999 && sd < 0.0) zEnd = min(zEnd, uPar[i].x);
    cover = max(cover, 1.0 - smoothstep(-0.75, 0.75, sd));
  }

  float zn = zEnd / uZMax;
  float2 lp = pos.xy / uRes * uLowRes - 0.5;
  float2 b = floor(lp), f = lp - b;
  int2 hi = int2(uLowRes) - 1;
  float3 sum = 0.0;
  float wsum = 0.0;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      float4 s = tA.Load(int3(clamp(int2(b) + int2(x, y), int2(0, 0), hi), 0));
      float wb = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
      float w = wb * (exp(-abs(s.g - zn) * 80.0) + 1e-4);
      sum += s.rba * w;
      wsum += w;
    }
  }
  float3 L = sum / max(wsum, 1e-6);

  float3 lit = uSunCol * (L.x * uExposure);
  float keep = saturate(L.x / max(L.y, 1e-6));
  float pointer = 1.0 - (1.0 - pointerShade(p, uSun - p)) * (1.0 - saturate(L.z));
  keep *= 1.0 - POINTER_SHADE * pointer;
  lit *= 1.0 - POINTER_SHADE * pointer;
  float icons = tIcons.SampleLevel(sClamp, uvOf(p), 0.0).r;
  float3 sky = tSky.SampleLevel(sClamp, uvOf(p), 0.0).rgb;
  float3 rim = rimLight(p, 1.0) + pointerRim(p);

  float3 full = uSunCol * (L.y * uExposure * uOverWin);
  full *= FLASH_WIN / (FLASH_WIN + luma(full));
  float3 ground = develop(GROUND);
  float3 onWindow = develop(GROUND + full * keep + rim) - ground;
  // Only a trace in front of open sky: the lightning is meant to appear in front
  // of dark clouds.
  float3 veil = lit * (uOverSky * 0.35);
  veil *= FLASH_SKY / (FLASH_SKY + luma(veil));
  float3 was = develop(sky);
  float3 onSky = develop(sky + veil + rim) - was;
  float known = (1.0 - cover) * (1.0 - icons);
  if (uLinear > 0.5) {
    float3 onWindowL = max(toLinear(ground + onWindow) - toLinear(ground), 0.0);
    float3 onSkyL = max(toLinear(was + onSky) - toLinear(was), 0.0);
    return float4(lerp(onWindowL, onSkyL, known) * uWhite, 0.0) * quiet;
  }
  return float4(max(lerp(onWindow, onSky, known), 0.0), 0.0) * quiet;
}

// Dust: particles in the haze volume. They glow only where sunlight falls, and
// stand out only against a dark background. Six vertices per particle.
struct DustOut {
  float4 pos : SV_Position;
  float3 col : COLOR0;
  float2 c : TEXCOORD0;
};

float3 hash3(float n) {
  return frac(sin(float3(n, n + 1.0, n + 2.0) * float3(12.9898, 78.233, 37.719)) * 43758.5453);
}

static const float2 CORNER[6] = {
  float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
  float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0),
};

DustOut DustVS(uint vid : SV_VertexID) {
  float id = float(vid / 6u);
  float2 corner = CORNER[vid % 6u];
  float3 h = hash3(id * 1.37 + 0.5);
  float3 h2 = hash3(id * 2.71 + 9.1);
  float z = uZMax * (0.04 + 0.92 * h.z);
  float close = (1.0 - h.z) * (1.0 - h.z);

  float2 span = uView + 80.0;
  // The wind carries the dust across the image, each particle at its own speed (in
  // nine steps: this way the offset wraps seamlessly after 80 image widths).
  float2 wind = float2(-uGlow.w * (0.35 + 0.1125 * floor(h2.x * 8.999)), -1.7 * uTime * (0.35 + 0.9 * h2.x));
  float2 wob = float2(sin(uTime * (0.21 + 0.2 * h2.y) + h.x * 50.0), cos(uTime * (0.17 + 0.2 * h2.z) + h.y * 50.0))
             * (8.0 + 14.0 * h2.x);
  float2 q = h.xy * span + wind + wob;
  float2 p = q - span * floor(q / span) - 40.0;
  float2 toSun = uSun - p;

  float vis = windowVis(p, toSun, z) * layerVis(p, toSun, z);
  float behind = 0.0;
  [loop] for (int i = 0; i < uN; i++) {
    float4 par = uPar[i];
    float c = par.y * (1.0 - smoothstep(-0.75, 0.75, sdRound(p, uRect[i], par.z)));
    if (par.x < z) vis *= 1.0 - c;
    else behind = max(behind, c);
  }
  float ground = luma(tSky.SampleLevel(sClamp, uvOf(p), 3.0).rgb) * (1.0 - behind);

  float cosT = uZS / length(float3(toSun, uZS));
  float ph = phaseFn(cosT);
  float twinkle = 0.5 + 0.5 * sin(uTime * (1.1 + 2.4 * h2.y) + h2.z * 40.0);
  float size = lerp(1.4, 5.0, close);
  float b = vis * min(ph, 5.0) * (0.3 + 0.7 * twinkle) * lerp(1.0, 0.25, close) * (0.15 + 0.85 * h2.z * h2.z) / (1.0 + 4.0 * ground);

  DustOut o;
  o.col = uSunCol * min(b * 0.2, 0.7);
  o.c = corner;
  float2 ndc = float2(p.x / uView.x * 2.0 - 1.0, 1.0 - p.y / uView.y * 2.0);
  o.pos = float4(ndc + corner * (size * uDpr) / uRes, 0.0, 1.0);
  return o;
}

float4 DustPS(DustOut i) : SV_Target {
  float r = dot(i.c, i.c);
  float3 c = i.col * exp(-r * 2.6) * smoothstep(1.0, 0.55, r);
  return float4(uLinear > 0.5 ? toLinear(c) * uWhite : c, 0.0);
}

// Volumetric clouds. Shape and lighting follow the open-source implementation by
// Takram (three-geospatial, packages/clouds, MIT license, Shota Matsuda), which
// implements the method from Horizon Zero Dawn; the textures also come from
// there (the noise after Sébastien Hillaire, TileableVolumeNoise, MIT). The
// licenses are in clouds\.
//
// Three layers: low cumulus, taller cumulus, thin cirrus above. A weather map
// states per layer where clouds are; a 3D noise gives them their shape, a finer
// one erodes the edges. The result is the extinction per meter: how much light a
// position absorbs. Thin parts let light through, dense ones do not.

Texture3D tShape : register(t5);     // base shape, 128^3
Texture3D tDetail : register(t6);    // fine cells, 32^3
Texture2D tWeather : register(t7);   // weather map: r low, g middle, b high clouds
Texture2D tTurb : register(t8);      // turbulence that bends the undersides

static const float EYE_ALT = 600.0;                       // the viewer stands on the ground, 600 m above sea level
static const float3 LAYER_LO = float3(750.0, 1000.0, 7500.0);    // bottom of each layer above the ground
static const float3 LAYER_HI = float3(1400.0, 2200.0, 8000.0);   // top
static const float3 LAYER_DENS = float3(0.2, 0.2, 0.003);        // extinction per meter at full density
static const float3 LAYER_SHAPE = float3(1.0, 1.0, 0.4);         // how strongly the base shape erodes
static const float3 LAYER_DETAIL = float3(1.0, 1.0, 0.0);        // how strongly the fine cells erode
static const float3 LAYER_FILTER = float3(0.6, 0.6, 0.5);        // width of the transition at the edge of a cloud field
static const float CUMULUS_LO = 750.0, CUMULUS_HI = 2200.0;      // the two cumulus layers together
static const float WEATHER_TILE = 100e3;    // meters per tile of the weather map
static const float SHAPE_REPEAT = 0.0003;   // tiles of the base shape per meter
static const float DETAIL_REPEAT = 0.006;
static const float TURB_REPEAT = 20.0;      // tiles of the turbulence per tile of the weather map
static const float TURB_DISP = 350.0;       // how far the turbulence displaces, in meters
static const float EVOLVE_M = 400.0;        // meters the noise rises through the clouds per unit of cloudAge
static const float CL_GAIN = 1.8;           // brightness of the sunlight in the clouds relative to the sky
static const float CL_SKY = 0.2;            // fraction of the sky light
static const float CL_FAR = 60e3;           // clouds farther away are not computed

struct Weather {
  float3 hf;     // height within each layer, 0 to 1
  float3 dens;   // density per layer according to the weather map
  float2 uv;
  float3 P;      // position in the cloud field: shifted with the wind, y = height above the ground
};

// The weather map, sampled smoothly (cubic splines from four taps). One pixel of
// the map is almost 200 meters wide, so very large in the image; with linear
// interpolation between the points every cloud would get kinks along the grid.
float3 weatherSmooth(float2 uv) {
  uint tw, th;
  tWeather.GetDimensions(tw, th);
  float2 size = float2(tw, th);
  float2 st = uv * size - 0.5;
  float2 i = floor(st), f = st - i;
  float2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
  float2 w1 = (4.0 - 6.0 * f * f + 3.0 * f * f * f) / 6.0;
  float2 w3 = f * f * f / 6.0;
  float2 w2 = 1.0 - w0 - w1 - w3;
  float2 g0 = w0 + w1, g1 = w2 + w3;
  float2 h0 = (i - 0.5 + w1 / g0) / size, h1 = (i + 1.5 + w3 / g1) / size;
  return g0.y * (g0.x * tWeather.SampleLevel(sWrap, float2(h0.x, h0.y), 0.0).rgb + g1.x * tWeather.SampleLevel(sWrap, float2(h1.x, h0.y), 0.0).rgb)
       + g1.y * (g0.x * tWeather.SampleLevel(sWrap, float2(h0.x, h1.y), 0.0).rgb + g1.x * tWeather.SampleLevel(sWrap, float2(h1.x, h1.y), 0.0).rgb);
}

// Weather at a position (meters, origin at the center of the Earth). mip
// averages the map over a larger area; negative: at full resolution and sampled
// smoothly. lift raises it, for the search for the envelope.
Weather cloudWeather(float3 pos, float mip, float lift) {
  Weather w;
  float h = length(pos) - R_PLANET - EYE_ALT;
  w.P = float3(pos.x, h, pos.z) + cloudShift * CL_TILE * windAt(h);
  w.uv = w.P.xz / WEATHER_TILE;
  w.hf = saturate((h - LAYER_LO) / (LAYER_HI - LAYER_LO));
  float3 lw = (mip < 0.0 ? weatherSmooth(w.uv) : tWeather.SampleLevel(sWrap, w.uv, mip).rgb) + lift;
  // Clouds round off toward the top: there the map has to yield more.
  float3 x = clamp(pow(w.hf, 0.35) * 2.0 - 1.0, -1.0, 1.0);
  float3 factor = 1.0 - uCover * float3(1.0, 1.0, 0.85) * (1.0 - x * x);
  w.dens = saturate((lerp(lw, 1.0, LAYER_FILTER) - factor) / LAYER_FILTER);
  // Outside its layer there is no cloud, not even with a raised map.
  w.dens *= step(1e-4, w.hf) * step(w.hf, 0.9999);
  return w;
}

// Extinction per meter. fine: also the fine cells, which only erode; without
// them the result is never smaller. lod averages the base shape over a larger
// volume. grad: how high the position lies within its cloud, 0.5 at the bottom
// to 1 at the top, for the sky light.
float cloudMedia(Weather w, bool fine, float lod, out float grad) {
  float3 dens = w.dens;
  // Turbulence bends the lower third: wisps hang there.
  // The turbulence map jumps from point to point; read at full resolution it would
  // tear the clouds into shreds with hard edges.
  float3 turb = (tTurb.SampleLevel(sWrap, w.uv * TURB_REPEAT, 1.6).rgb * 2.0 - 1.0)
              * (TURB_DISP * dot(dens, saturate(1.0 - w.hf / 0.3)));
  // The shape changes over time: the noise slowly sinks through the clouds.
  float3 at = w.P + turb;
  float shape = tShape.SampleLevel(sWrap, (at - float3(0.0, cloudAge * EVOLVE_M, 0.0)) * SHAPE_REPEAT, lod).r;
  float3 lo = (1.0 - shape) * LAYER_SHAPE;
  // At the outermost edge of a cloud field, where the map yields almost nothing,
  // only the peaks of the noise would otherwise remain: nothing but isolated dots.
  dens = saturate((dens - lo) / max(1.0 - lo, 1e-4)) * smoothstep(0.0, 0.16, w.dens);
  if (fine) {
    float detail = tDetail.SampleLevel(sWrap, at * DETAIL_REPEAT, 0.0).r;
    // Billowing at the top, wispy at the bottom.
    float3 m = lerp(pow(detail, 6.0), 1.0 - detail, saturate((w.hf - 0.2) / 0.2)) * LAYER_DETAIL * 0.5;
    dens = saturate((dens * 2.0 - m) / max(1.0 - m, 1e-4));
  }
  // Cirrus is fibrous: long, thin strands across the wind, almost nothing in
  // between.
  if (dens.z > 0.0) {
    float2 q = float2(w.P.x * 0.82 + w.P.z * 0.57, -w.P.x * 0.57 + w.P.z * 0.82) / 9000.0;
    float strand = tNoise.SampleLevel(sWrap, float3(q.x * 0.22, 0.31, q.y * 1.3), 0.0).a;
    float fibre = tNoise.SampleLevel(sWrap, float3(q.x * 0.9, 0.77, q.y * 6.1), 0.0).a;
    dens.z *= saturate(strand * 2.4 - 0.75) * (0.25 + 1.5 * saturate(fibre * 2.2 - 0.6));
  }
  // Clouds get denser toward the top.
  // Thunderclouds are denser.
  dens = saturate(dens * LAYER_DENS * (0.75 * w.hf + 0.25) * (1.0 + 1.2 * uStorm.x));
  float sum = dens.x + dens.y + dens.z;
  grad = sum > 0.0 ? dot(w.hf * 0.5 + 0.5, dens) / sum : 0.75;
  return sum;
}

// Two lobes: cloud droplets scatter strongly forward and slightly backward.
float cloudPhase(float mu, float k) {
  return 0.5 * (hg(mu, 0.7 * k) + hg(mu, -0.2 * k));
}

// How much cloud lies between a position and the sun (optical depth). The first
// steps are short and see the fine structures, which gives the clouds relief;
// after that they grow and also capture the shadow of more distant clouds,
// averaged over ever larger areas of the weather map.
float sunDepth(float3 pos, float3 s, float jit) {
  float od = 0.0, st = 35.0, t = st * jit;
  [loop] for (int j = 0; j < 10; j++) {
    // A long step stands for everything that lies along it: weather map and shape
    // are averaged over an area as large as the step. A single sample at full
    // resolution would sometimes hit a dense spot and sometimes none, and would give
    // black holes in the light.
    Weather w = cloudWeather(pos + s * t, j < 3 ? -1.0 : clamp(log2(st / 195.0), 0.0, 5.5), 0.0);
    if (any(w.dens > 1e-5)) {
      float grad;
      od += cloudMedia(w, j < 2, clamp(log2(st / 52.0), 0.0, 5.0), grad) * st;
    }
    t += st;
    st *= 1.85;
  }
  return od;
}

static const float RAIN_EXT = 4.5e-4;   // extinction per meter in the heaviest rain: visibility a few kilometers
static const float RAIN_FAR = 28e3;     // rain farther away is not computed

// Rain falls where the clouds are dense, the denser the heavier: 0 to 1. It
// falls in strands that slowly move downward. The wind at the ground carries it
// along as it falls: what arrives at a position below the clouds fell from a
// cloud farther upwind, the lower the farther. So the rain veils hang slanted
// below the clouds.
float rainRate(float3 pos) {
  float h = length(pos) - R_PLANET - EYE_ALT;
  if (h > CUMULUS_LO + 250.0) return 0.0;
  float3 from = pos - float3(SURF.x, 0.0, SURF.y) * (uWind.x * max(CUMULUS_LO - h, 0.0));
  Weather w = cloudWeather(normalize(from) * (R_PLANET + EYE_ALT + 1150.0), 2.0, 0.0);
  // The stronger the weather, the less dense a cloud has to be for rain to fall
  // from it.
  // In strong weather it rains lightly everywhere, in showers below the dense
  // cells.
  float shower = smoothstep(0.62 - 0.32 * uStorm.y, 0.95 - 0.2 * uStorm.y, max(w.dens.x, w.dens.y));
  // The light rain in between hardly reduces visibility; the showers appear as
  // rain veils in the image.
  float rate = uStorm.y * lerp(shower, 1.0, 0.05 * uStorm.y);
  if (rate <= 0.0) return 0.0;
  float strand = tNoise.SampleLevel(sWrap, float3(w.P.x * 0.0012, (h + uTime * 11.0) * 0.00016, w.P.z * 0.0012), 0.0).a;
  return rate * (0.35 + 1.3 * strand) * smoothstep(CUMULUS_LO + 250.0, CUMULUS_LO - 150.0, h);
}

// Rain along the view ray, from the eye to below the clouds: its transmittance;
// dist and deep = mean distance and its spread in meters. The samples are denser
// at the front.
float rainVeil(float3 d, float jit, out float dist, out float deep) {
  dist = 0.0;
  deep = 0.0;
  if (uStorm.y <= 0.001) return 1.0;
  float3 o = float3(0.0, R_PLANET + EYE_ALT - uCloudLift, 0.0);
  float tEnd = RAIN_FAR;
  if (d.y > 0.004) tEnd = min(tEnd, raySphere(o, d, R_PLANET + EYE_ALT + CUMULUS_LO + 250.0).y);
  if (d.y < -0.004) tEnd = min(tEnd, 2500.0 / -d.y);
  const int N = 10;
  float T = 1.0, tw = 0.0, ws = 0.0, sq = 0.0, before = 0.0;
  for (int i = 0; i < N; i++) {
    float f = (float(i) + jit) / float(N);
    float t = tEnd * f * f;
    float len = t - before;
    before = t;
    // Snow hides what is behind it much sooner than rain of the same strength.
    float ext = RAIN_EXT * (1.0 + 1.5 * uSnow.x) * rainRate(o + d * t);
    if (ext <= 0.0) continue;
    float tr = exp(-ext * len);
    float part = T * (1.0 - tr);
    tw += part * t;
    ws += part;
    sq += part * t * t;
    T *= tr;
  }
  if (ws > 0.0) {
    dist = tw / ws;
    deep = sqrt(max(sq / ws - dist * dist, 0.0));
  }
  return T;
}

// Rain scatters the light that remains below the clouds: gray, slightly brighter
// than their undersides. Falling snow is whiter and brighter.
float3 rainLight() {
  return float3(0.84, 0.90, 1.0) * ((1.0 + 0.4 * uSnow.x) * (luma(uAmb) * 0.6 + luma(uCloudCol) * (SUN_E * 0.006)) * (1.0 - 0.4 * uStorm.x));
}

// Light that a position in the cloud scatters toward the viewer, per unit of
// extinction. Multiple scattering approximated (Wrenninge, Kulla): eight levels,
// each penetrates twice as deep and scatters more broadly. ph: the phase
// function per level.
float3 cloudLight(float od, float ph[8], float grad) {
  float e = 0.0, a = 1.0;
  for (int i = 0; i < 8; i++) {
    e += ph[i] * exp(-od * a);
    a *= 0.5;
  }
  // Plus the light of the sky, more at the top of the cloud than at the bottom,
  // and a little from the sunlit ground back onto the undersides.
  // Thunderclouds are so thick that hardly any light reaches their underside, and
  // below the closed cloud deck the light of the sky is missing. Only thin edges
  // stay bright.
  float heavy = uStorm.x * (1.0 - exp(-od / 30.0));
  // Over a city the lamps light the undersides at night.
  return uCloudCol * (SUN_E * CL_GAIN * e * (1.0 - 0.72 * heavy)) + uAmb * (CL_SKY * grad * (1.0 - 0.55 * uStorm.x))
       + uSunCol * (SUN_E * 0.006 * (2.0 - 2.0 * grad) * (1.0 - 0.8 * uStorm.x))
       + float3(1.0, 0.76, 0.48) * (uTown * 0.035 * (1.5 - grad));
}

static float cloudSquare;   // sum for the spread of the distance, see cloudLayer
static float cloudDeep;     // how far the cumulus clouds spread around their mean distance along the view ray, meters

// Add one sample to the result: ext extinction per meter, len path length.
void cloudAdd(float ext, float len, float t, float3 light, inout float T, inout float3 L, inout float tw, inout float ws) {
  // Powdered sugar effect: very thin parts scatter less toward the viewer.
  float3 radiance = light * ext * (1.0 - 0.8 * exp(-ext * 150.0));
  float tr = exp(-ext * len);
  L += T * (radiance - radiance * tr) / max(ext, 1e-7);
  tw += T * (1.0 - tr) * t;
  ws += T * (1.0 - tr);
  cloudSquare += T * (1.0 - tr) * t * t;
  T *= tr;
}

// Cumulus clouds along the view ray: rgb = luminance, a = transmittance; dist =
// their mean distance in meters, 0 without a cloud. Empty air is crossed in
// large steps that only query the weather map, averaged over an area as large as
// the step so that no cloud is skipped. Where it reports a cloud, in fine steps,
// shorter than the depth to which the light penetrates. All steps grow with
// distance, like the pixels.
float4 cloudLayer(float3 d, float3 s, float3 sky, float jit, out float dist) {
  dist = 0.0;
  cloudSquare = 0.0;
  cloudDeep = 0.0;
  float3 o = float3(0.0, R_PLANET + EYE_ALT - uCloudLift, 0.0);
  float t0 = raySphere(o, d, R_PLANET + EYE_ALT + CUMULUS_LO).y;
  float t1 = min(raySphere(o, d, R_PLANET + EYE_ALT + CUMULUS_HI).y, CL_FAR);
  if (t0 >= t1) return float4(0.0, 0.0, 0.0, 1.0);

  float mu = dot(d, s);
  float ph[8];
  float a = 1.0;
  int i;
  for (i = 0; i < 8; i++) {
    ph[i] = a * cloudPhase(mu, a);
    a *= 0.5;
  }

  float T = 1.0, tw = 0.0, ws = 0.0;
  float3 L = 0.0;
  float t = t0 + clamp(t0 * 0.03, 120.0, 400.0) * jit;
  bool close = false;   // the weather map reports a cloud here
  int idle = 0;         // consecutive fine steps without a cloud
  float found = 0.0;    // where the envelope was last reported
  [loop] for (i = 0; i < uCloudSteps; i++) {
    if (T < 0.01 || t >= t1) break;
    float3 pos = o + d * t;
    float fineStep = clamp(t * 0.004, 10.0, 80.0);
    float coarseStep = clamp(t * 0.03, 120.0, 400.0);
    if (!close) {
      if (any(cloudWeather(pos, 1.5, 0.08).dens > 0.0)) {
        // The cloud begins somewhere within this step: back to its start.
        close = true;
        idle = 0;
        found = t;
        t = max(t - coarseStep, t0) + fineStep * frac(jit * 13.7);
      } else {
        t += coarseStep;
      }
      continue;
    }
    Weather w = cloudWeather(pos, -1.0, 0.0);
    float ext = 0.0, grad = 0.75;
    bool some = any(w.dens > 1e-5);
    if (some) {
      // First without the fine cells: they only erode. If nothing remains even so,
      // continue with a doubled step.
      // Very thin remnants are dropped: the fine cells would leave isolated dots of
      // them.
      some = cloudMedia(w, false, 0.0, grad) > 6e-4;
      if (some) ext = cloudMedia(w, true, 0.0, grad);
    }
    if (ext > 1e-5) {
      float od = sunDepth(pos, s, lightJit);
      cloudAdd(ext, fineStep, t, cloudLight(od, ph, grad), T, L, tw, ws);
      idle = 0;
    } else if (++idle >= 5 && t > found) {
      // Fall back to large steps only beyond the position where the envelope was
      // reported. Before it the map may not report anything here yet; the next large
      // step would find the envelope again, go back again, and the ray would make no
      // progress without ever arriving. In the image a stripe of sky then remained
      // across the cloud.
      idle = 0;
      close = any(cloudWeather(pos, 1.5, 0.08).dens > 0.0);
    }
    t += some ? fineStep : 2.0 * fineStep;
  }
  if (T < 0.01) T = 0.0;
  if (ws > 0.0) {
    dist = tw / ws;
    cloudDeep = sqrt(max(cloudSquare / ws - dist * dist, 0.0));
  }
  return float4(L, T);
}

// High cirrus: a thin layer far above the cumulus clouds, crossed with few
// samples. It is so thin that the sunlight reaches it almost unobstructed.
float4 highLayer(float3 d, float3 s, float jit, out float dist) {
  dist = 0.0;
  float3 o = float3(0.0, R_PLANET + EYE_ALT - uCloudLift, 0.0);
  float t0 = raySphere(o, d, R_PLANET + EYE_ALT + LAYER_LO.z).y;
  float t1 = raySphere(o, d, R_PLANET + EYE_ALT + LAYER_HI.z).y;
  if (t0 >= t1 || t0 > 220e3) return float4(0.0, 0.0, 0.0, 1.0);

  float mu = dot(d, s);
  float ph[8];
  float a = 1.0;
  int i;
  for (i = 0; i < 8; i++) {
    ph[i] = a * cloudPhase(mu, a);
    a *= 0.5;
  }
  const int N = 6;
  float len = (t1 - t0) / float(N);
  float T = 1.0, tw = 0.0, ws = 0.0;
  float3 L = 0.0;
  // Averaged over larger areas of the map in the distance, otherwise it flickers.
  float mip = log2(max(t0 / 25e3, 1.0));
  for (i = 0; i < N; i++) {
    float t = t0 + len * (float(i) + jit);
    Weather w = cloudWeather(o + d * t, mip, 0.0);
    if (w.dens.z <= 1e-5) continue;
    float grad;
    float ext = cloudMedia(w, false, 0.0, grad);
    if (ext <= 1e-6) continue;
    cloudAdd(ext, len, t, cloudLight(ext * 120.0, ph, grad), T, L, tw, ws);
  }
  if (ws > 0.0) dist = tw / ws;
  // Toward the horizon the layer fades out.
  float fade = 1.0 - smoothstep(120e3, 220e3, t0);
  return float4(L * fade, lerp(1.0, T, fade));
}

// Transmittance of the cumulus clouds in one direction, coarse: for the search
// for a start with an unobstructed sun.
float through(float3 d) {
  if (d.y < 0.01) return 1.0;
  float3 o = float3(0.0, R_PLANET + EYE_ALT - uCloudLift, 0.0);
  float t0 = raySphere(o, d, R_PLANET + EYE_ALT + CUMULUS_LO).y;
  float t1 = min(raySphere(o, d, R_PLANET + EYE_ALT + CUMULUS_HI).y, CL_FAR);
  float dt = (t1 - t0) / 64.0, od = 0.0;
  [loop] for (int i = 0; i < 64; i++) {
    Weather w = cloudWeather(o + d * (t0 + dt * (float(i) + 0.5)), 0.0, 0.0);
    if (any(w.dens > 1e-5)) {
      float grad;
      od += cloudMedia(w, false, 0.0, grad) * dt;
    }
  }
  return exp(-od);
}

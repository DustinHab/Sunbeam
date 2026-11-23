// 3D noise, tileable in all directions. One slice per call.
//   r  base shape of the clouds (Perlin with cells)
//   g  cells, coarse
//   b  cells, fine
//   a  smooth Perlin noise: weather map and mist in the haze

cbuffer NoiseC : register(b0) {
  float uSlice;
  float uSize;
  float2 uPad;
};

float3 wrap(float3 p, float f) {
  return p - f * floor(p / f);
}

float3 hash33(float3 p) {
  uint3 q = uint3(int3(p)) * uint3(1597334673u, 3812015801u, 2798796415u);
  q = (q.x ^ q.y ^ q.z) * uint3(1597334673u, 3812015801u, 2798796415u);
  return -1.0 + 2.0 * float3(q) * (1.0 / 4294967295.0);
}

float perlin(float3 x, float freq) {
  float3 p = floor(x), w = frac(x);
  float3 u = w * w * w * (w * (w * 6.0 - 15.0) + 10.0);
  float a = dot(hash33(wrap(p, freq)), w);
  float b = dot(hash33(wrap(p + float3(1, 0, 0), freq)), w - float3(1, 0, 0));
  float c = dot(hash33(wrap(p + float3(0, 1, 0), freq)), w - float3(0, 1, 0));
  float d = dot(hash33(wrap(p + float3(1, 1, 0), freq)), w - float3(1, 1, 0));
  float e = dot(hash33(wrap(p + float3(0, 0, 1), freq)), w - float3(0, 0, 1));
  float f = dot(hash33(wrap(p + float3(1, 0, 1), freq)), w - float3(1, 0, 1));
  float g = dot(hash33(wrap(p + float3(0, 1, 1), freq)), w - float3(0, 1, 1));
  float h = dot(hash33(wrap(p + float3(1, 1, 1), freq)), w - float3(1, 1, 1));
  return lerp(lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y), lerp(lerp(e, f, u.x), lerp(g, h, u.x), u.y), u.z);
}

float perlinFbm(float3 p, float freq, int oct) {
  float s = 0.0, a = 1.0;
  for (int i = 0; i < oct; i++) {
    s += a * perlin(p * freq, freq);
    freq *= 2.0;
    a *= 0.555;
  }
  return s;
}

float cells(float3 x, float freq) {
  float3 id = floor(x), p = frac(x);
  float m = 1e4;
  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      for (int k = -1; k <= 1; k++) {
        float3 off = float3(i, j, k);
        float3 d = p - (hash33(wrap(id + off, freq)) * 0.5 + 0.5 + off);
        m = min(m, dot(d, d));
      }
    }
  }
  return 1.0 - m;
}

float cellFbm(float3 p, float freq) {
  return 0.625 * cells(p * freq, freq) + 0.25 * cells(p * freq * 2.0, freq * 2.0) + 0.125 * cells(p * freq * 4.0, freq * 4.0);
}

float4 Noise(float4 pos : SV_Position) : SV_Target {
  float3 p = float3(pos.xy / uSize, uSlice);
  float pf = perlinFbm(p, 4.0, 6);
  float c1 = cellFbm(p, 4.0);
  float c2 = cellFbm(p, 8.0);
  float shape = c1 + saturate(abs(pf)) * (1.0 - c1);
  return float4(shape, c1, c2, saturate(0.5 + 0.5 * pf));
}

#include "daycycle.h"

#include <algorithm>
#include <cmath>

namespace {

const double RAD = 3.14159265358979323846 / 180;
// A celestial body is this far from the edge at rise and set, as a fraction of
// the width.
const double EDGE = 0.06;
// The moon rises this many times faster than the sun sinks below the horizon:
// below the horizon there is little room in the image, above it a lot.
const float MOON_LIFT = 3.2f;

double wrap180(double a) {
  a = std::fmod(a + 180, 360.0);
  return (a < 0 ? a + 360 : a) - 180;
}

void place(double az, double alt, double dec, double latitude, const Renderer& r, float& x, float& y) {
  // If the body passes through the south, the image looks south and east is on the
  // left; otherwise north, east on the right.
  bool south = latitude >= dec;
  double c = std::clamp(std::sin(dec * RAD) / std::max(std::cos(latitude * RAD), 1e-6), -1.0, 1.0);
  double rise = std::acos(c) / RAD;   // azimuth of the rise, from north
  double half = std::max(south ? 180 - rise : rise, 25.0);
  double a = wrap180(az - (south ? 180 : 0));
  x = float(r.viewW() * (0.5 + (0.5 - EDGE) * a / half));
  y = r.rowOfAltitude(alt);
}

}  // namespace

Heaven heavenOfBodies(const Bodies& b, double latitude, const Renderer& r) {
  Heaven h;
  place(b.sunAz, b.sunAlt, b.sunDec, latitude, r, h.sunX, h.sunY);
  place(b.moonAz, b.moonAlt, b.moonDec, latitude, r, h.moonX, h.moonY);
  h.moonLit = float(b.moonLit);
  h.moonLimb = float(b.moonLimb);
  h.moonSpin = float(b.moonSpin);
  return h;
}

Heaven heavenOfHand(float sunX, float sunY, const Bodies& b, const Renderer& r) {
  Heaven h;
  h.sunX = sunX;
  h.sunY = sunY;
  float line = r.horizonRow();
  h.moonX = r.viewW() - sunX;
  h.moonY = line - MOON_LIFT * (sunY - line);
  // Even at new moon a thin crescent remains: a night entirely without moon would
  // have no light that could cast rays.
  h.moonLit = std::max(float(b.moonLit), 0.14f);
  // The bright side points to the sun.
  h.moonLimb = std::atan2(-(sunX - h.moonX), -(sunY - h.moonY));
  h.moonSpin = 0.35f;
  return h;
}

void sunOfMoon(float moonX, float moonY, const Renderer& r, float& sunX, float& sunY) {
  float line = r.horizonRow();
  sunX = r.viewW() - moonX;
  sunY = line + (line - moonY) / MOON_LIFT;
}

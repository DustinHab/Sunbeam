#include "astro.h"

#include <algorithm>
#include <cmath>

namespace {

const double PI = 3.14159265358979323846;
const double RAD = PI / 180;

double wrap360(double a) {
  a = std::fmod(a, 360.0);
  return a < 0 ? a + 360 : a;
}

double sind(double a) { return std::sin(a * RAD); }
double cosd(double a) { return std::cos(a * RAD); }

struct Equatorial {
  double ra, dec;   // degrees
};

Equatorial fromEcliptic(double lon, double lat, double tilt) {
  double ra = std::atan2(sind(lon) * cosd(tilt) - std::tan(lat * RAD) * sind(tilt), cosd(lon)) / RAD;
  double dec = std::asin(std::clamp(sind(lat) * cosd(tilt) + cosd(lat) * sind(tilt) * sind(lon), -1.0, 1.0)) / RAD;
  return {wrap360(ra), dec};
}

// Altitude and azimuth from hour angle and declination.
void horizontal(double hourAngle, double dec, double latitude, double& az, double& alt) {
  double sinAlt = sind(latitude) * sind(dec) + cosd(latitude) * cosd(dec) * cosd(hourAngle);
  alt = std::asin(std::clamp(sinAlt, -1.0, 1.0)) / RAD;
  double north = sind(dec) * cosd(latitude) - cosd(dec) * cosd(hourAngle) * sind(latitude);
  double east = -cosd(dec) * sind(hourAngle);
  az = wrap360(std::atan2(east, north) / RAD);
}

}  // namespace

Bodies bodiesAt(double time, double latitude, double longitude) {
  Bodies b;
  double d = time / 86400.0 + 2440587.5 - 2451545.0;   // days since 1.1.2000, 12:00
  double tilt = 23.439 - 0.00000036 * d;
  double sidereal = wrap360(280.46061837 + 360.98564736629 * d + longitude);   // local sidereal time

  // Sun.
  double g = 357.529 + 0.98560028 * d;
  double sunLon = wrap360(280.459 + 0.98564736 * d + 1.915 * sind(g) + 0.020 * sind(2 * g));
  Equatorial sun = fromEcliptic(sunLon, 0, tilt);
  horizontal(sidereal - sun.ra, sun.dec, latitude, b.sunAz, b.sunAlt);
  b.sunDec = sun.dec;

  // Moon: mean longitude and the largest perturbations.
  double L = 218.316 + 13.176396 * d;
  double M = 134.963 + 13.064993 * d;    // mean anomaly of the moon
  double F = 93.272 + 13.229350 * d;     // distance from the ascending node
  double D = 297.850 + 12.190749 * d;    // mean elongation
  double moonLon = L + 6.289 * sind(M) + 1.274 * sind(2 * D - M) + 0.658 * sind(2 * D) + 0.214 * sind(2 * M) - 0.186 * sind(g) -
                   0.114 * sind(2 * F);
  double moonLat = 5.128 * sind(F) + 0.280 * sind(M + F) + 0.277 * sind(M - F) + 0.173 * sind(2 * D - F) + 0.055 * sind(2 * D + F - M) +
                   0.046 * sind(2 * D - F - M);
  double distance = 385001 - 20905 * cosd(M) - 3699 * cosd(2 * D - M) - 2956 * cosd(2 * D);   // km
  Equatorial moon = fromEcliptic(wrap360(moonLon), moonLat, tilt);
  double hour = sidereal - moon.ra;
  horizontal(hour, moon.dec, latitude, b.moonAz, b.moonAlt);
  // The moon is close: seen from the observer's place it is lower than seen from
  // the center of the earth.
  b.moonAlt -= std::asin(6378.14 / distance) / RAD * cosd(b.moonAlt);
  b.moonDec = moon.dec;

  // Illuminated fraction from the angular distance to the sun.
  double cosElong = sind(sun.dec) * sind(moon.dec) + cosd(sun.dec) * cosd(moon.dec) * cosd(sun.ra - moon.ra);
  b.moonLit = 0.5 * (1 - std::clamp(cosElong, -1.0, 1.0));
  // The bright side points to the sun. Its angle from the celestial north pole,
  // and the angle of the pole against the zenith; the difference is what is seen.
  double limb = std::atan2(cosd(sun.dec) * sind(sun.ra - moon.ra), sind(sun.dec) * cosd(moon.dec) - cosd(sun.dec) * sind(moon.dec) * cosd(sun.ra - moon.ra));
  double pole = std::atan2(sind(hour), std::tan(latitude * RAD) * cosd(moon.dec) - sind(moon.dec) * cosd(hour));
  b.moonLimb = limb - pole;
  b.moonSpin = -pole;
  return b;
}

DayTimes dayTimes(double dayStart, double latitude, double longitude) {
  DayTimes t;
  // Rise and set count when the upper edge of the disk touches the horizon; the
  // air lifts the image there by a good half degree.
  const double EDGE = -0.833;
  const int STEP = 120;
  double before = bodiesAt(dayStart, latitude, longitude).sunAlt;
  t.noonAlt = before;
  t.noon = dayStart;
  for (int s = STEP; s <= 86400; s += STEP) {
    double alt = bodiesAt(dayStart + s, latitude, longitude).sunAlt;
    if (alt > t.noonAlt) {
      t.noonAlt = alt;
      t.noon = dayStart + s;
    }
    if ((before < EDGE) != (alt < EDGE)) {
      double at = dayStart + s - STEP + STEP * (EDGE - before) / (alt - before);
      if (alt > before && t.rise == 0) t.rise = at;
      if (alt < before && t.set == 0) t.set = at;
    }
    before = alt;
  }
  return t;
}

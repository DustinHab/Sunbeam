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

double timeOfSunAltitude(double around, double altitude, bool setting, double latitude, double longitude) {
  const int STEP = 240;
  double best = around, miss = 1e9, cross = 0, gap = 1e9;
  double before = bodiesAt(around - 43200 - STEP, latitude, longitude).sunAlt;
  for (int s = -43200; s <= 43200; s += STEP) {
    double alt = bodiesAt(around + s, latitude, longitude).sunAlt;
    if ((alt < before) == setting) {
      if ((before - altitude) * (alt - altitude) <= 0 && alt != before) {
        // Of several crossings on this branch, the one closest in time.
        double at = s - STEP + STEP * (altitude - before) / (alt - before);
        if (std::abs(at) < gap) gap = std::abs(at), cross = around + at;
      }
      if (std::abs(alt - altitude) < miss) miss = std::abs(alt - altitude), best = around + s;
    }
    before = alt;
  }
  return gap < 1e8 ? cross : best;
}

// ---- Stars, planets, meteor showers ----

namespace {

// An orbit around the sun: its elements at the start of 2000 and how they change
// per century. Semi-major axis in astronomical units, eccentricity, inclination,
// mean longitude, longitude of the perihelion and of the ascending node in
// degrees, for the ecliptic and equinox of J2000. From E. M. Standish, "Keplerian
// Elements for Approximate Positions of the Major Planets" (JPL), valid from
// 1800 to 2050 and good to a few minutes of arc.
struct Orbit {
  double a, e, i, L, peri, node;
  double da, de, di, dL, dperi, dnode;
};

const Orbit ORBITS[PLANETS + 1] = {
    {0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593, 0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081},
    {0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255, 0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418},
    {1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891, 0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343},
    {5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909, -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106},
    {9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448, -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794},
    {19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503, -0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589},
    // The Earth, strictly the center of mass of Earth and moon.
    {1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0, 0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0},
};
const int EARTH = PLANETS;

const Shower SHOWER_LIST[SHOWERS] = {
    {"Quadrantids", 283.15, 230, 49, 41, 2.1, 110, 2.5, 2.5},
    {"Lyrids", 32.32, 271, 34, 49, 2.1, 18, 0.22, 0.22},
    {"Eta Aquariids", 45.5, 338, -1, 66, 2.4, 50, 0.08, 0.08},
    {"Southern Delta Aquariids", 127.0, 340, -16, 41, 2.5, 25, 0.09, 0.09},
    {"Perseids", 140.0, 48, 58, 59, 2.2, 100, 0.2, 0.25},
    {"Orionids", 208.0, 95, 16, 66, 2.5, 20, 0.12, 0.12},
    {"Leonids", 235.27, 152, 22, 71, 2.5, 15, 0.39, 0.39},
    {"Geminids", 262.2, 112, 33, 35, 2.6, 150, 0.25, 0.59},
    {"Ursids", 270.7, 217, 76, 33, 3.0, 10, 0.61, 0.61},
};

double length3(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Place in an orbit at T centuries after the start of 2000: astronomical units,
// ecliptic of J2000.
void orbitAt(const Orbit& o, double T, double* out) {
  double a = o.a + o.da * T, e = o.e + o.de * T, i = (o.i + o.di * T) * RAD;
  double peri = o.peri + o.dperi * T, node = (o.node + o.dnode * T) * RAD;
  double w = peri * RAD - node;
  double M = wrap360(o.L + o.dL * T - peri);
  M = (M > 180 ? M - 360 : M) * RAD;
  // Kepler's equation.
  double E = M + e * std::sin(M);
  for (int k = 0; k < 8; k++) E -= (E - e * std::sin(E) - M) / (1 - e * std::cos(E));
  double x = a * (std::cos(E) - e), y = a * std::sqrt(1 - e * e) * std::sin(E);
  double cw = std::cos(w), sw = std::sin(w), cn = std::cos(node), sn = std::sin(node), ci = std::cos(i), si = std::sin(i);
  out[0] = (cw * cn - sw * sn * ci) * x + (-sw * cn - cw * sn * ci) * y;
  out[1] = (cw * sn + sw * cn * ci) * x + (-sw * sn + cw * cn * ci) * y;
  out[2] = sw * si * x + cw * si * y;
}

// Apparent brightness of a planet, the main terms of A. Mallama and J. L. Hilton
// (2018). r: its distance from the sun, d: from the Earth, astronomical units;
// phase: the angle sun, planet, Earth in degrees; rings: how far Saturn's rings
// are tilted toward the Earth, degrees.
double magnitudeOf(int body, double r, double d, double phase, double rings) {
  double away = 5 * std::log10(r * d), a = phase;
  switch (body) {
    case 0:
      return -0.613 + away + a * (6.3280e-2 + a * (-1.6336e-3 + a * (3.3644e-5 + a * (-3.4265e-7 + a * (1.6893e-9 - 3.0334e-12 * a)))));
    case 1:
      return a < 163.7 ? -4.384 + away + a * (-1.044e-3 + a * (3.687e-4 + a * (-2.814e-6 + 8.938e-9 * a)))
                       : 236.05828 + away + a * (-2.81914 + 8.39034e-3 * a);
    case 2:
      return a <= 50 ? -1.601 + away + a * (2.267e-2 - 1.302e-4 * a) : -0.367 + away + a * (-0.02573 + 0.0003445 * a);
    case 3:
      return -9.395 + away + a * (-3.7e-4 + 6.16e-4 * a);
    case 4: {
      double s = std::sin(std::abs(rings) * RAD);
      return -8.914 + away - 1.825 * s + 0.026 * a - 0.378 * s * std::exp(-2.25 * a);
    }
    default:
      return -7.110 + away + a * (6.587e-3 + 1.045e-4 * a);
  }
}

// Turns a direction from the equator and equinox of J2000 to those of T
// centuries later: the axis of the Earth wanders (precession, angles of the
// IAU 1976).
void precession(double T, double* m) {
  const double SEC = RAD / 3600;
  double zeta = (2306.2181 + 0.30188 * T) * T * SEC, z = (2306.2181 + 1.09468 * T) * T * SEC, theta = (2004.3109 - 0.42665 * T) * T * SEC;
  double cz = std::cos(zeta), sz = std::sin(zeta), cZ = std::cos(z), sZ = std::sin(z), ct = std::cos(theta), st = std::sin(theta);
  m[0] = cz * ct * cZ - sz * sZ, m[1] = -sz * ct * cZ - cz * sZ, m[2] = -st * cZ;
  m[3] = cz * ct * sZ + sz * cZ, m[4] = -sz * ct * sZ + cz * cZ, m[5] = -st * sZ;
  m[6] = cz * st, m[7] = -sz * st, m[8] = ct;
}

// Azimuth and altitude of a direction of the star catalogue.
void horizontalOf(const double* frame, const double* e, double& az, double& alt) {
  double east = frame[0] * e[0] + frame[1] * e[1] + frame[2] * e[2];
  double north = frame[3] * e[0] + frame[4] * e[1] + frame[5] * e[2];
  double up = frame[6] * e[0] + frame[7] * e[1] + frame[8] * e[2];
  alt = std::asin(std::clamp(up, -1.0, 1.0)) / RAD;
  az = wrap360(std::atan2(east, north) / RAD);
}

}  // namespace

const Shower& shower(int index) { return SHOWER_LIST[std::clamp(index, 0, SHOWERS - 1)]; }

Stars starsAt(double time, double latitude, double longitude) {
  Stars s;
  double d = time / 86400.0 + 2440587.5 - 2451545.0;
  double T = d / 36525.0;
  double sidereal = wrap360(280.46061837 + 360.98564736629 * d + longitude) * RAD;
  double lat = latitude * RAD;
  // From the equator of the date to east, north and up at the place.
  const double local[9] = {-std::sin(sidereal),                 std::cos(sidereal),                  0,
                           -std::sin(lat) * std::cos(sidereal), -std::sin(lat) * std::sin(sidereal), std::cos(lat),
                           std::cos(lat) * std::cos(sidereal),  std::cos(lat) * std::sin(sidereal),  std::sin(lat)};
  double turn[9];
  precession(T, turn);
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) s.frame[r * 3 + c] = local[r * 3] * turn[c] + local[r * 3 + 1] * turn[3 + c] + local[r * 3 + 2] * turn[6 + c];
  }

  double earth[3];
  orbitAt(ORBITS[EARTH], T, earth);
  double sunAway = length3(earth);
  const double tilt = 23.43928 * RAD;   // of the ecliptic against the equator of J2000
  for (int b = 0; b < PLANETS; b++) {
    // The light of a planet has been on its way for minutes to hours: it is seen
    // where it was then.
    double p[3], g[3], away = 0;
    for (int pass = 0; pass < 2; pass++) {
      orbitAt(ORBITS[b], T - away * 0.0057755183 / 36525, p);
      for (int c = 0; c < 3; c++) g[c] = p[c] - earth[c];
      away = length3(g);
    }
    double r = length3(p);
    double phase = std::acos(std::clamp((r * r + away * away - sunAway * sunAway) / (2 * r * away), -1.0, 1.0)) / RAD;
    double rings = 0;
    if (b == 4) {
      const double incl = 28.075216 * RAD, node = (169.508470 + 1.394681 * T) * RAD;
      double lon = std::atan2(g[1], g[0]), above = std::asin(g[2] / away);
      rings = std::asin(std::sin(incl) * std::cos(above) * std::sin(lon - node) - std::cos(incl) * std::sin(above)) / RAD;
    }
    const double e[3] = {g[0] / away, (g[1] * std::cos(tilt) - g[2] * std::sin(tilt)) / away, (g[1] * std::sin(tilt) + g[2] * std::cos(tilt)) / away};
    Planet& out = s.planet[b];
    horizontalOf(s.frame, e, out.az, out.alt);
    out.dec = std::asin(std::clamp(turn[6] * e[0] + turn[7] * e[1] + turn[8] * e[2], -1.0, 1.0)) / RAD;
    out.magnitude = magnitudeOf(b, r, away, phase, rings);
  }

  s.sunLongitude = wrap360(std::atan2(-earth[1], -earth[0]) / RAD);
  s.solarHour = std::fmod(std::fmod(time / 3600.0 + longitude / 15.0, 24.0) + 24.0, 24.0);
  for (int i = 0; i < SHOWERS; i++) {
    const Shower& w = SHOWER_LIST[i];
    const double e[3] = {cosd(w.dec) * cosd(w.ra), cosd(w.dec) * sind(w.ra), sind(w.dec)};
    horizontalOf(s.frame, e, s.showerAz[i], s.showerAlt[i]);
    // A sharp maximum on a broad base: a tenth of the meteors spread over four
    // times as many days.
    double off = std::fmod(s.sunLongitude - w.peak + 540.0, 360.0) - 180.0;
    double slope = (off < 0 ? w.rise : w.fall) * std::abs(off);
    s.showerRate[i] = w.rate * (0.9 * std::pow(10.0, -slope) + 0.1 * std::pow(10.0, -slope / 4));
  }
  return s;
}

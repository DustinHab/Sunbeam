// Position of sun and moon in the sky for a place and a time. Computed with the
// short series from the Astronomical Almanac; the sun is accurate to a hundredth
// of a degree, the moon to a few tenths. That is enough for an image. Plus the
// stars, the planets and the meteor showers.

#pragma once

struct Bodies {
  double sunAz = 0, sunAlt = 0;     // degrees; azimuth from north through east, altitude above the horizon
  double sunDec = 0;                // declination
  double moonAz = 0, moonAlt = 0;
  double moonDec = 0;
  double moonLit = 0;               // illuminated fraction of the moon disk, 0 to 1
  double moonLimb = 0;              // direction of the bright side, radians, counterclockwise from the zenith
  double moonSpin = 0;              // direction of the moon's north pole, likewise
};

// time: seconds since 1.1.1970 UTC. latitude to the north, longitude to the
// east, in degrees.
Bodies bodiesAt(double time, double latitude, double longitude);

struct DayTimes {
  double rise = 0, set = 0;   // rise and set of the sun, seconds since 1970; 0 if there is none
  double noon = 0;            // highest position
  double noonAlt = 0;         // its altitude in degrees
};

// Rise and set within the day from dayStart to dayStart + 24 h.
DayTimes dayTimes(double dayStart, double latitude, double longitude);

// The time within twelve hours of around at which the sun is at this altitude:
// on its way down (setting) or on its way up. If it does not get there on that
// day, the time at which it comes closest.
double timeOfSunAltitude(double around, double altitude, bool setting, double latitude, double longitude);

// The planets the eye can see, from the sun outward.
const int PLANETS = 6;   // Mercury, Venus, Mars, Jupiter, Saturn, Uranus

struct Planet {
  double az = 0, alt = 0;   // degrees, as for the sun
  double dec = 0;           // declination
  double magnitude = 0;     // apparent brightness
};

// A meteor shower: the Earth crosses the trail of a comet at the same place of
// its orbit every year. Dates, radiants, speeds and rates after the working
// list of the International Meteor Organization.
struct Shower {
  const char* name;
  double peak;       // ecliptic longitude of the sun at the maximum, degrees, equinox of J2000
  double ra, dec;    // the radiant: the point of the sky its meteors seem to come from
  double speed;      // of its meteors, kilometers per second
  double spread;     // ratio of the numbers of meteors from one magnitude to the next
  double rate;       // meteors per hour at the maximum under a dark sky with the radiant at the zenith
  double rise, fall; // how fast the rate grows before the maximum and falls after it: powers of ten per degree of the sun's longitude
};

const int SHOWERS = 9;
const Shower& shower(int index);

// The sky of the stars for a place and a time.
struct Stars {
  // Turns a direction of the star catalogue (unit vector, equator and equinox of
  // J2000) into east, north and up at the place: three rows.
  double frame[9] = {};
  Planet planet[PLANETS];
  double showerAz[SHOWERS] = {}, showerAlt[SHOWERS] = {};   // where each radiant is
  double showerRate[SHOWERS] = {};                          // and its rate now, with the radiant at the zenith
  double solarHour = 0;      // local solar time in hours
  double sunLongitude = 0;   // ecliptic longitude of the sun, equinox of J2000
};

Stars starsAt(double time, double latitude, double longitude);

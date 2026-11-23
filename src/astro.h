// Position of sun and moon in the sky for a place and a time. Computed with the
// short series from the Astronomical Almanac; the sun is accurate to a hundredth
// of a degree, the moon to a few tenths. That is enough for an image.

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

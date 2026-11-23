// Live weather: fetches the weather at the configured place at intervals and
// converts it into the positions of the sliders Clouds, Wind and Fog and into
// the temperature. The data comes from Open-Meteo (open-meteo.com, CC BY 4.0),
// without registration; only the place is sent, rounded to a tenth of a degree.

#pragma once
#include <memory>
#include <string>

struct WeatherNow {
  bool valid = false;
  float cover = 0;        // cloud cover, 0 to 1
  float rain = 0;         // precipitation in mm per hour
  float wind = 0;         // wind at 10 m height, m/s
  float gust = 0;         // gusts, m/s
  float temperature = 0;  // degrees Celsius
  float dayTemperature = 0;   // mean of the day
  float visibility = 1e5f;    // meters
  float snowDepth = 0;    // snow on the ground, meters
  float elevation = 0;    // height of the place above the sea, meters
  int code = 0;           // WMO weather code as Open-Meteo delivers it
};

// The air cools by this much per meter of height.
const double LAPSE = 0.0065;

// Positions of the sliders for this weather: Clouds 0 to 2, Wind 0 to 1.
// Whether what falls is rain or snow follows from the temperature.
void weatherSliders(const WeatherNow& w, float& clouds, float& wind);

// Position of the slider Fog for a visibility in meters: none from FOG_FROM
// upward, full at FOG_TO. And back. The lower part of the slider is stretched
// (FOG_BEND): there, mist over a few kilometers, small steps show most.
const double FOG_FROM = 10000, FOG_TO = 150, FOG_BEND = 1.5;
float fogOfVisibility(double meters);
double visibilityOfFog(float fog);
// The slider for this weather: from the visibility, at least half if fog is
// reported.
float weatherFog(const WeatherNow& w);

// What the season brings at a place when no live weather tells: the temperature
// at sea level in degrees Celsius, now (with the course of the day) and as the
// mean of the day. time: seconds since 1970.
void seasonTemperature(double latitude, double longitude, double time, float& now, float& day);

// Reads the response of the service. Returns false if it cannot be read.
bool parseWeather(const std::string& json, WeatherNow& out);

// Fetches the weather once, blocking. error: what went wrong.
bool fetchWeather(double latitude, double longitude, WeatherNow& out, std::string& error);

// Fetches it in the background again and again: every quarter of an hour, sooner
// after a failure.
class WeatherFeed {
 public:
  ~WeatherFeed();
  // Starts for this place (or switches to it). The same place again changes
  // nothing.
  void start(double latitude, double longitude);
  void stop();
  bool running() const { return bool(state_); }
  // A new report if one has arrived since the last pickup.
  bool take(WeatherNow& out);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

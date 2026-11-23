// Live weather: fetches the weather at the configured place at intervals and
// converts it into the positions of the sliders Clouds and Wind. The data comes
// from Open-Meteo (open-meteo.com, CC BY 4.0), without registration; only the
// place is sent, rounded to a tenth of a degree.

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
  int code = 0;           // WMO weather code as Open-Meteo delivers it
};

// Positions of the sliders for this weather: Clouds 0 to 2, Wind 0 to 1.
void weatherSliders(const WeatherNow& w, float& clouds, float& wind);

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

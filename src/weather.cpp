#include "weather.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "net.h"

namespace {

const wchar_t* HOST = L"api.open-meteo.com";
const double EVERY = 15 * 60;   // seconds between two fetches; the service recomputes this often
const double RETRY = 2 * 60;    // after a failure
const double WIND_FULL = 17;    // wind in m/s at full slider, like SURF_MAX in render.cpp

// To a tenth of a degree: precise enough for the weather, and the service learns
// only the region.
double coarse(double degrees) { return std::round(degrees * 10) / 10; }

// The number after "name": in the section text; if it is missing, out stays as
// it is.
bool number(const std::string& text, const char* name, float& out) {
  std::string key = std::string("\"") + name + "\":";
  size_t at = text.find(key);
  if (at == std::string::npos) return false;
  const char* begin = text.c_str() + at + key.size();
  char* end = nullptr;
  double v = std::strtod(begin, &end);
  if (end == begin) return false;
  out = float(v);
  return true;
}

}  // namespace

bool parseWeather(const std::string& json, WeatherNow& out) {
  // Before it there is "current_units" with the same names; the section with the
  // values is meant.
  size_t from = json.find("\"current\":{");
  if (from == std::string::npos) return false;
  size_t to = json.find('}', from);
  std::string now = json.substr(from, to == std::string::npos ? std::string::npos : to - from);
  WeatherNow w;
  float cover = 0, fall = 0, span = 900, code = 0;
  if (!number(now, "cloud_cover", cover) || !number(now, "wind_speed_10m", w.wind)) return false;
  number(now, "precipitation", fall);
  number(now, "interval", span);
  number(now, "weather_code", code);
  number(now, "wind_gusts_10m", w.gust);
  number(now, "wind_direction_10m", w.windDir);
  float high = 0;
  if (number(now, "cloud_cover_high", high)) w.high = std::clamp(high / 100, 0.0f, 1.0f);
  float part = 0;
  if (number(now, "cloud_cover_low", part)) w.low = std::clamp(part / 100, 0.0f, 1.0f);
  if (number(now, "cloud_cover_mid", part)) w.mid = std::clamp(part / 100, 0.0f, 1.0f);
  number(now, "temperature_2m", w.temperature);
  number(now, "visibility", w.visibility);
  number(now, "snow_depth", w.snowDepth);
  // The height of the place is at the top, before the sections.
  number(json.substr(0, json.find("\"current_units\"")), "elevation", w.elevation);
  // The mean of the day is the first entry of a list.
  w.dayTemperature = w.temperature;
  size_t day = json.find("\"daily\":{");
  size_t mean = day == std::string::npos ? day : json.find("\"temperature_2m_mean\":[", day);
  if (mean != std::string::npos) {
    const char* begin = json.c_str() + mean + 23;
    char* end = nullptr;
    double v = std::strtod(begin, &end);
    if (end != begin) w.dayTemperature = float(v);
  }
  w.cover = std::clamp(cover / 100, 0.0f, 1.0f);
  // The precipitation applies to the preceding interval (usually a quarter of an
  // hour): scaled up to the hour.
  w.rain = std::max(fall, 0.0f) * 3600 / std::max(span, 60.0f);
  w.code = int(std::lround(code));
  w.valid = true;
  out = w;
  return true;
}

bool fetchWeather(double latitude, double longitude, WeatherNow& out, std::string& error) {
  wchar_t path[400];
  swprintf_s(path,
             L"/v1/forecast?latitude=%.1f&longitude=%.1f&current=cloud_cover,cloud_cover_low,cloud_cover_mid,cloud_cover_high,precipitation,weather_code,wind_speed_10m,wind_direction_10m,"
             L"wind_gusts_10m,temperature_2m,visibility,snow_depth&daily=temperature_2m_mean&forecast_days=1&timezone=auto&wind_speed_unit=ms",
             coarse(latitude), coarse(longitude));
  std::string body;
  if (!httpGet(HOST, path, body, error)) return false;
  if (!parseWeather(body, out)) {
    error = "response not readable";
    return false;
  }
  return true;
}

void weatherSliders(const WeatherNow& w, float& clouds, float& wind, float& cirrus) {
  // The slider Clouds stands for the low and middle clouds; the high ones have the
  // slider Cirrus. A sky covered by cirrostratus alone is clear below it. The
  // layers overlap at random.
  float cover = w.low >= 0 && w.mid >= 0 ? 1 - (1 - w.low) * (1 - w.mid) : w.cover;
  cirrus = std::clamp(w.high, 0.0f, 1.0f);
  // Up to almost fully covered the slider follows the cloud cover; a closed cloud
  // deck is above 100 %.
  float sky = cover < 0.9f ? cover * 0.95f / 0.9f : 0.95f + (cover - 0.9f) * 2.5f;
  bool thunder = w.code >= 95;
  // Rain and snow alike: something falls. The sky draws it as snow when it is
  // cold enough.
  bool falling = w.rain > 0.05f || (w.code >= 51 && w.code <= 67) || (w.code >= 71 && w.code <= 77) || (w.code >= 80 && w.code <= 86);
  // From drizzle (0.1 mm per hour) to cloudburst (10 mm per hour).
  float heavy = std::clamp(std::log10(std::max(w.rain, 0.1f) / 0.1f) / 2, 0.0f, 1.0f);
  if (thunder) clouds = 1.66f + 0.3f * heavy;
  else if (falling) clouds = 1.32f + 0.28f * heavy;
  else clouds = sky;
  clouds = std::clamp(clouds, 0.0f, 2.0f);
  // Gusts count as well; the slider grows more slowly than the wind (see surfSpeed
  // in render.cpp).
  float blow = w.gust > w.wind ? 0.75f * w.wind + 0.25f * w.gust : w.wind;
  wind = std::clamp(float(std::pow(std::max(blow, 0.0f) / WIND_FULL, 2.0 / 3.0)), 0.0f, 1.0f);
}

// The slider grows with the logarithm of the visibility: each step thickens the
// fog by the same factor. Ten kilometers and more is ordinary air, which the
// haze of the image already shows.
float fogOfVisibility(double meters) {
  double steps = std::clamp(std::log(FOG_FROM / std::max(meters, 1.0)) / std::log(FOG_FROM / FOG_TO), 0.0, 1.0);
  return float(std::pow(steps, 1 / FOG_BEND));
}

double visibilityOfFog(float fog) {
  return FOG_FROM * std::pow(FOG_FROM / FOG_TO, -std::pow(double(std::clamp(fog, 0.0f, 1.0f)), FOG_BEND));
}

float weatherFog(const WeatherNow& w) {
  float fog = fogOfVisibility(w.visibility);
  // Reported fog means less than a kilometer of visibility, whatever the model
  // computes for the place.
  if (w.code == 45 || w.code == 48) fog = std::max(fog, fogOfVisibility(900));
  return fog;
}

// A rough climate from the latitude alone: the mean of the year falls from 27
// degrees at the equator by a third of a degree per degree of latitude; the
// seasons swing more the farther from the equator, warmest a month after the
// longest day; the afternoon is a few degrees warmer than the night.
void seasonTemperature(double latitude, double longitude, double time, float& now, float& day) {
  const double PI = 3.14159265358979;
  double lat = std::abs(latitude);
  double year = std::fmod(time / 86400.0 / 365.2425, 1.0);              // 0 at the turn of the year
  double warmest = (latitude >= 0 ? 200.0 : 17.0) / 365.0;               // 19 July, in the south 17 January
  double mean = 27.0 - 0.33 * lat + 0.17 * lat * std::cos(2 * PI * (year - warmest));
  double hour = std::fmod(time / 3600.0 + longitude / 15.0, 24.0);       // local solar time
  day = float(mean);
  now = float(mean + 4.0 * std::cos(2 * PI * (hour - 15.0) / 24.0));
}

struct WeatherFeed::State {
  std::mutex lock;
  std::condition_variable wake;
  bool quit = false, fresh = false;
  WeatherNow latest;
  double latitude = 0, longitude = 0;
};

WeatherFeed::~WeatherFeed() { stop(); }

void WeatherFeed::start(double latitude, double longitude) {
  latitude = coarse(latitude), longitude = coarse(longitude);
  if (state_ && state_->latitude == latitude && state_->longitude == longitude) return;
  stop();
  state_ = std::make_shared<State>();
  state_->latitude = latitude;
  state_->longitude = longitude;
  // The thread holds its own state and ends by itself: nobody has to wait for a
  // hanging connection on exit.
  std::thread([s = state_] {
    for (;;) {
      WeatherNow w;
      std::string error;
      bool ok = fetchWeather(s->latitude, s->longitude, w, error);
      std::unique_lock<std::mutex> hold(s->lock);
      if (s->quit) return;
      if (ok) {
        s->latest = w;
        s->fresh = true;
      }
      if (s->wake.wait_for(hold, std::chrono::duration<double>(ok ? EVERY : RETRY), [&] { return s->quit; })) return;
    }
  }).detach();
}

void WeatherFeed::stop() {
  if (!state_) return;
  {
    std::lock_guard<std::mutex> hold(state_->lock);
    state_->quit = true;
  }
  state_->wake.notify_all();
  state_.reset();
}

bool WeatherFeed::take(WeatherNow& out) {
  if (!state_) return false;
  std::lock_guard<std::mutex> hold(state_->lock);
  if (!state_->fresh) return false;
  state_->fresh = false;
  out = state_->latest;
  return true;
}

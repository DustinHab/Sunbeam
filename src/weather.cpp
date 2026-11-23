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
  number(now, "temperature_2m", w.temperature);
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
  wchar_t path[320];
  swprintf_s(path,
             L"/v1/forecast?latitude=%.1f&longitude=%.1f&current=cloud_cover,precipitation,weather_code,wind_speed_10m,wind_gusts_10m,temperature_2m"
             L"&wind_speed_unit=ms",
             coarse(latitude), coarse(longitude));
  std::string body;
  if (!httpGet(HOST, path, body, error)) return false;
  if (!parseWeather(body, out)) {
    error = "response not readable";
    return false;
  }
  return true;
}

void weatherSliders(const WeatherNow& w, float& clouds, float& wind) {
  // Up to almost fully covered the slider follows the cloud cover; a closed cloud
  // deck is above 100 %.
  float sky = w.cover < 0.9f ? w.cover * 0.95f / 0.9f : 0.95f + (w.cover - 0.9f) * 2.5f;
  bool thunder = w.code >= 95;
  bool snow = (w.code >= 71 && w.code <= 77) || w.code == 85 || w.code == 86;
  bool fog = w.code == 45 || w.code == 48;
  bool wet = !snow && (w.rain > 0.05f || (w.code >= 51 && w.code <= 67) || (w.code >= 80 && w.code <= 82));
  // From drizzle (0.1 mm per hour) to cloudburst (10 mm per hour).
  float heavy = std::clamp(std::log10(std::max(w.rain, 0.1f) / 0.1f) / 2, 0.0f, 1.0f);
  if (thunder) clouds = 1.66f + 0.3f * heavy;
  else if (wet) clouds = 1.32f + 0.28f * heavy;
  // The sky cannot show snow and fog yet: for now a closed cloud deck.
  else if (snow || fog) clouds = std::max(sky, 1.12f);
  else clouds = sky;
  clouds = std::clamp(clouds, 0.0f, 2.0f);
  // Gusts count as well; the slider grows more slowly than the wind (see surfSpeed
  // in render.cpp).
  float blow = w.gust > w.wind ? 0.75f * w.wind + 0.25f * w.gust : w.wind;
  wind = std::clamp(float(std::pow(std::max(blow, 0.0f) / WIND_FULL, 2.0 / 3.0)), 0.0f, 1.0f);
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

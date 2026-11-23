#include "place.h"

#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <thread>

#include "net.h"

namespace {

const wchar_t* HOST = L"geocoding-api.open-meteo.com";

// The value of a string member "key" in a JSON object, unescaped. Empty if absent.
std::string member(const std::string& object, const char* key) {
  std::string find = std::string("\"") + key + "\":\"";
  size_t at = object.find(find);
  if (at == std::string::npos) return "";
  std::string out;
  for (size_t i = at + find.size(); i < object.size() && object[i] != '"'; i++) {
    if (object[i] != '\\' || i + 1 >= object.size()) {
      out += object[i];
      continue;
    }
    char c = object[++i];
    if (c == 'u' && i + 4 < object.size()) {
      // \uXXXX to UTF-8; names outside the basic plane do not occur.
      unsigned code = unsigned(std::strtoul(object.substr(i + 1, 4).c_str(), nullptr, 16));
      i += 4;
      if (code < 0x80) {
        out += char(code);
      } else if (code < 0x800) {
        out += char(0xc0 | (code >> 6));
        out += char(0x80 | (code & 0x3f));
      } else {
        out += char(0xe0 | (code >> 12));
        out += char(0x80 | ((code >> 6) & 0x3f));
        out += char(0x80 | (code & 0x3f));
      }
    } else {
      out += c == 'n' ? ' ' : c;
    }
  }
  return out;
}

bool number(const std::string& object, const char* key, double& out) {
  std::string find = std::string("\"") + key + "\":";
  size_t at = object.find(find);
  if (at == std::string::npos) return false;
  const char* begin = object.c_str() + at + find.size();
  char* end = nullptr;
  double v = std::strtod(begin, &end);
  if (end == begin) return false;
  out = v;
  return true;
}

}  // namespace

bool parseCoordinates(const std::wstring& text, Place& out) {
  // Two numbers, each optionally followed by a letter for the hemisphere.
  double value[2] = {};
  wchar_t side[2] = {};
  const wchar_t* at = text.c_str();
  for (int i = 0; i < 2; i++) {
    while (*at && (std::iswspace(*at) || *at == L',' || *at == L';' || *at == L'°')) at++;
    wchar_t* end = nullptr;
    value[i] = std::wcstod(at, &end);
    if (end == at) return false;
    at = end;
    while (*at && (std::iswspace(*at) || *at == L'°')) at++;
    wchar_t c = wchar_t(std::towupper(*at));
    if (c == L'N' || c == L'S' || c == L'E' || c == L'W') {
      side[i] = c;
      at++;
    }
  }
  while (*at && std::iswspace(*at)) at++;
  if (*at) return false;
  if (side[0] == L'S') value[0] = -value[0];
  if (side[1] == L'W') value[1] = -value[1];
  if (side[0] == L'E' || side[0] == L'W' || side[1] == L'N' || side[1] == L'S') return false;
  if (std::abs(value[0]) > 90 || std::abs(value[1]) > 180) return false;
  out.name.clear();
  out.latitude = value[0];
  out.longitude = value[1];
  return true;
}

bool parsePlace(const std::string& json, Place& out) {
  size_t from = json.find("\"results\":[{");
  if (from == std::string::npos) return false;
  // The first result ends at the brace that closes it; the values hold no nested objects.
  size_t to = json.find('}', from);
  std::string first = json.substr(from, to == std::string::npos ? std::string::npos : to - from);
  Place p;
  if (!number(first, "latitude", p.latitude) || !number(first, "longitude", p.longitude)) return false;
  std::string name = member(first, "name"), region = member(first, "admin1"), country = member(first, "country");
  if (!region.empty() && region != name) name += ", " + region;
  if (!country.empty() && country != name) name += ", " + country;
  p.name = widen(name);
  out = p;
  return true;
}

bool findPlace(const std::wstring& name, Place& out, std::string& error) {
  error.clear();
  std::string body;
  if (!httpGet(HOST, L"/v1/search?name=" + urlEncode(name) + L"&count=1&language=en&format=json", body, error)) return false;
  return parsePlace(body, out);
}

std::wstring coordinateText(double latitude, double longitude) {
  wchar_t text[64];
  swprintf_s(text, L"%.1f° %c, %.1f° %c", std::abs(latitude), latitude >= 0 ? L'N' : L'S', std::abs(longitude), longitude >= 0 ? L'E' : L'W');
  return text;
}

struct PlaceSearch::State {
  std::mutex lock;
  bool done = false, found = false;
  Place place;
  std::string error;
};

PlaceSearch::~PlaceSearch() = default;

void PlaceSearch::start(const std::wstring& name) {
  // A search still under way is dropped: its thread keeps its own state and ends by itself.
  state_ = std::make_shared<State>();
  std::thread([s = state_, name] {
    Place p;
    std::string error;
    bool found = findPlace(name, p, error);
    std::lock_guard<std::mutex> hold(s->lock);
    s->found = found;
    s->place = p;
    s->error = error;
    s->done = true;
  }).detach();
}

bool PlaceSearch::take(bool& found, Place& out, std::string& error) {
  if (!state_) return false;
  {
    std::lock_guard<std::mutex> hold(state_->lock);
    if (!state_->done) return false;
    found = state_->found;
    out = state_->place;
    error = state_->error;
  }
  state_.reset();
  return true;
}

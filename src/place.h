// A place on Earth, entered as a name or as coordinates. Names are looked up
// with the Open-Meteo geocoding service (location data: GeoNames, CC BY 4.0).
// Only the typed name is sent.

#pragma once
#include <memory>
#include <string>

struct Place {
  std::wstring name;                   // as shown: "Bern, Canton of Bern, Switzerland"; empty for bare coordinates
  double latitude = 0, longitude = 0;  // degrees, north and east positive
};

// Reads coordinates such as "46.9 7.4", "46.9, 7.4", "46.9N 7.4E" or
// "33.9 S, 151.2 E". Returns false if the text is not a pair of coordinates.
bool parseCoordinates(const std::wstring& text, Place& out);

// Reads the answer of the geocoding service: the first result.
bool parsePlace(const std::string& json, Place& out);

// Looks a name up, waiting for the answer. error: what went wrong; empty if the
// service answered but knows no such place.
bool findPlace(const std::wstring& name, Place& out, std::string& error);

// "46.9° N, 7.4° E"
std::wstring coordinateText(double latitude, double longitude);

// Looks names up in the background, one at a time.
class PlaceSearch {
 public:
  ~PlaceSearch();
  void start(const std::wstring& name);
  bool busy() const { return bool(state_); }
  // True once when the search has ended. found: whether out is valid; error as in findPlace.
  bool take(bool& found, Place& out, std::string& error);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

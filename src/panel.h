// The window with the settings: four pages with sliders. It shows and changes
// what the app passes to it here, and reports every change at once so that the
// sky follows while a slider is dragged.

#pragma once
#include <windows.h>

#include <functional>
#include <string>

#include "render.h"

struct PanelHost {
  Params* par = nullptr;
  int* course = nullptr;             // course of the sun: 0 fixed, 1 by the clock, 2 time lapse
  double* latitude = nullptr;
  double* longitude = nullptr;
  float* lapseMinutes = nullptr;
  bool* realWeather = nullptr;       // live weather: the weather at the place then sets clouds, wind, fog and temperature
  float* fog = nullptr;              // the slider Fog as set by hand, 0 to 1
  float* temperature = nullptr;      // the slider Temperature as set by hand, degrees Celsius; below -50: by the season
  const Renderer* render = nullptr;
  HICON icon = nullptr;
  std::function<double()> shownTime;         // the time the sky currently shows, seconds since 1970
  std::function<void(bool done)> changed;    // something was changed; done: the user change is complete
  std::function<bool()> autostart;
  std::function<void(bool)> setAutostart;
  // Location: what the field shows, the note below it (coordinates, or the
  // state of a search), and what happens with a newly entered text.
  std::function<std::wstring()> place;
  std::function<std::wstring()> placeNote;
  std::function<void(const std::wstring&)> setPlace;
};

// Shows the window or brings it to the front.
void openPanel(HINSTANCE inst, const PanelHost& host);
void closePanel();
// Draws each page to a file page1.png to page4.png without showing a window.
bool renderPanel(const std::wstring& dir, const PanelHost& host, float scale);

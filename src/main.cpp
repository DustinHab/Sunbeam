// Sunbeam: a sun in the desktop background, windows and icons cast shadows into
// the haze.
//
//   Sunbeam.exe                        operation on the desktop
//   Sunbeam.exe --check <folder>       renders the image to files without a
//                                      window and measures
//   Sunbeam.exe --scene <file>         writes down what is in the way on the
//                                      desktop
//   Sunbeam.exe --real <folder> [x y]  renders the image for the real position
//                                      of the windows to files
//   Sunbeam.exe --sky <folder>         renders the sky over the day to files
//   Sunbeam.exe --drift <folder>       compares drifting clouds with the correct
//                                      image
//   Sunbeam.exe --panel <folder>       draws the settings window to files
//   Sunbeam.exe --life <folder>        lets two days pass and writes down how
//                                      often ships, aircraft and birds appeared
//   Sunbeam.exe --blendtest <file>     measures on the display whether the light
//                                      window enters the image
//   Sunbeam.exe --probe <folder> [s]   attaches to the desktop briefly and
//                                      writes down whether everything is in
//                                      place; with "blind" after the seconds
//                                      nothing appears, the loop runs unseen
//   Sunbeam.exe --weather <folder> [place]
//                                      looks the place up (a name or
//                                      coordinates), fetches its weather once
//                                      and writes down where the sliders went

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "check.h"
#include "motion.h"
#include "scene.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  // All dimensions in real device pixels, independent of the scaling.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args(argv, argv + argc);
  LocalFree(argv);

  // A check run does not take the graphics card from what the user is doing: it
  // only gets what other programs leave. Timings are then only valid while the
  // card is free; with the environment variable SUNBEAM_FULL set, the run takes
  // its normal share.
  wchar_t full[8];
  if (args.size() >= 2 && args[1].rfind(L"--", 0) == 0 && !GetEnvironmentVariableW(L"SUNBEAM_FULL", full, 8)) standAside(true);

  if (args.size() >= 3 && args[1] == L"--check") {
    int w = args.size() >= 6 ? _wtoi(args[3].c_str()) : 2560;
    int h = args.size() >= 6 ? _wtoi(args[4].c_str()) : 1440;
    float scale = args.size() >= 6 ? float(_wtof(args[5].c_str())) : 1.5f;
    return runCheck(args[2], w, h, scale);
  }
  if (args.size() >= 3 && args[1] == L"--sky") {
    int w = args.size() >= 6 ? _wtoi(args[3].c_str()) : 2560;
    int h = args.size() >= 6 ? _wtoi(args[4].c_str()) : 1440;
    float scale = args.size() >= 6 ? float(_wtof(args[5].c_str())) : 1.5f;
    return runSky(args[2], w, h, scale);
  }
  if (args.size() >= 3 && args[1] == L"--panel") return runPanelCheck(args[2]);
  if (args.size() >= 3 && args[1] == L"--life") return runLifeCheck(args[2]);
  if (args.size() >= 3 && args[1] == L"--drift") return runDrift(args[2], 2560, 1440, 1.5f);
  if (args.size() >= 3 && args[1] == L"--weather") {
    // Without a place a fixed one, so that a check reveals nothing about the user.
    std::wstring query;
    for (size_t k = 3; k < args.size(); k++) query += (k > 3 ? L" " : L"") + args[k];
    return runWeatherCheck(args[2], query.empty() ? L"Berlin" : query);
  }
  if (args.size() >= 3 && args[1] == L"--scene") {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring text = describeDesktop(primaryMonitor());
    FILE* f = nullptr;
    if (_wfopen_s(&f, args[2].c_str(), L"w, ccs=UTF-8") || !f) return 2;
    fputws(text.c_str(), f);
    fclose(f);
    CoUninitialize();
    return 0;
  }
  if (args.size() >= 3 && args[1] == L"--blendtest") return runBlendCheck(args[2]);
  if (args.size() >= 3 && args[1] == L"--real") {
    float x = args.size() >= 5 ? float(_wtof(args[3].c_str())) : 0.5f;
    float y = args.size() >= 5 ? float(_wtof(args[4].c_str())) : 0.6f;
    return runReal(args[2], x, y);
  }
  if (args.size() >= 3 && args[1] == L"--drag") {
    float x = args.size() >= 5 ? float(_wtof(args[3].c_str())) : 0.5f;
    float y = args.size() >= 5 ? float(_wtof(args[4].c_str())) : 0.3f;
    return runDrag(args[2], x, y);
  }
  if (args.size() >= 3 && args[1] == L"--pace") {
    float x = args.size() >= 5 ? float(_wtof(args[3].c_str())) : 0.5f;
    float y = args.size() >= 5 ? float(_wtof(args[4].c_str())) : 0.3f;
    return runPace(args[2], x, y);
  }
  if (args.size() >= 3 && args[1] == L"--probe") {
    return runApp(instance, args.size() >= 4 ? _wtof(args[3].c_str()) : 3.0, args[2], args.size() >= 5 && args[4] == L"blind");
  }
  return runApp(instance);
}

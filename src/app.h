#pragma once
#include <windows.h>

#include <string>

// Operation on the desktop: sky behind the icons, light above everything, icon
// in the notification area. Returns when the program exits.
// With probeSeconds > 0 it runs only that long, without an icon in the
// notification area, and writes to probeDir what it found.
int runApp(HINSTANCE instance, double probeSeconds = 0, const std::wstring& probeDir = L"");

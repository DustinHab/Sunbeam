// HTTPS requests with WinHTTP, for the weather and the place search.

#pragma once
#include <string>

// Fetches https://<host><path>. Returns false and fills error if the service
// cannot be reached or answers with a status other than 200.
bool httpGet(const wchar_t* host, const std::wstring& path, std::string& body, std::string& error);

// Percent-encodes text for use in a query string (UTF-8).
std::wstring urlEncode(const std::wstring& text);

// UTF-8 to UTF-16.
std::wstring widen(const std::string& utf8);

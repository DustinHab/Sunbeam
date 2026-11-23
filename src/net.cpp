#include "net.h"

#include <windows.h>
#include <winhttp.h>

namespace {

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

}  // namespace

bool httpGet(const wchar_t* host, const std::wstring& path, std::string& body, std::string& error) {
  Handle session, connection, request;
  session.h = WinHttpOpen(L"Sunbeam", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h) {
    error = "cannot open a connection";
    return false;
  }
  WinHttpSetTimeouts(session.h, 8000, 8000, 8000, 12000);
  connection.h = WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (connection.h) request.h = WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    error = "service not reachable (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  body.clear();
  for (;;) {
    DWORD ready = 0;
    if (!WinHttpQueryDataAvailable(request.h, &ready) || ready == 0) break;
    size_t at = body.size();
    body.resize(at + ready);
    DWORD got = 0;
    if (!WinHttpReadData(request.h, &body[at], ready, &got)) got = 0;
    body.resize(at + got);
    if (got == 0 || body.size() > 65536) break;
  }
  if (status != 200) {
    error = "response " + std::to_string(status);
    return false;
  }
  return true;
}

std::wstring urlEncode(const std::wstring& text) {
  int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), utf8.data(), n, nullptr, nullptr);
  static const wchar_t hex[] = L"0123456789ABCDEF";
  std::wstring out;
  for (unsigned char c : utf8) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += wchar_t(c);
    } else {
      out += L'%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::wstring widen(const std::string& utf8) {
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), out.data(), n);
  return out;
}

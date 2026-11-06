// sys.h on Windows.
#include "app/sys.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <ctime>

std::wstring pathJoin(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  wchar_t last = dir.back();
  return last == L'\\' || last == L'/' ? dir + name : dir + L"\\" + name;
}

std::wstring pathDir(const std::wstring& path) {
  size_t s = path.find_last_of(L"\\/");
  return s == std::wstring::npos ? L"." : path.substr(0, s);
}

std::wstring pathName(const std::wstring& path) {
  size_t s = path.find_last_of(L"\\/");
  return s == std::wstring::npos ? path : path.substr(s + 1);
}

FILE* openFile(const std::wstring& path, const char* mode) {
  std::wstring m(mode, mode + strlen(mode));
  return _wfopen(path.c_str(), m.c_str());
}

bool readFile(const std::wstring& path, std::vector<uint8_t>& out, size_t maxBytes) {
  FILE* f = openFile(path, "rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  long long n = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  if (n < 0 || (unsigned long long)n > maxBytes) {
    fclose(f);
    return false;
  }
  out.resize((size_t)n);
  size_t r = n ? fread(out.data(), 1, (size_t)n, f) : 0;
  fclose(f);
  return r == (size_t)n;
}

bool readText(const std::wstring& path, std::string& out) {
  std::vector<uint8_t> d;
  if (!readFile(path, d)) return false;
  out.assign(d.begin(), d.end());
  return true;
}

bool fileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool fileModified(const std::wstring& path, int* day, int* month, int* hour, int* minute) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
  FILETIME lt;
  SYSTEMTIME st;
  FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
  FileTimeToSystemTime(&lt, &st);
  *day = st.wDay;
  *month = st.wMonth;
  *hour = st.wHour;
  *minute = st.wMinute;
  return true;
}

void makeDir(const std::wstring& path) { CreateDirectoryW(path.c_str(), nullptr); }

bool listFiles(const std::wstring& dir, std::vector<std::wstring>& names) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pathJoin(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return true;
}

bool listDirs(const std::wstring& dir, std::vector<std::wstring>& names) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pathJoin(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') names.push_back(fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return true;
}

std::string timeText(const char* fmt) {
  time_t t = time(nullptr);
  tm lt;
  localtime_s(&lt, &t);
  char buf[64];
  strftime(buf, sizeof buf, fmt, &lt);
  return buf;
}

void openUrl(const char* url) {
  std::wstring w(url, url + strlen(url));
  ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool httpGet(const char* url, std::string& out) {
  out.clear();
  std::wstring w(url, url + strlen(url));
  wchar_t host[256], path[2048];
  URL_COMPONENTS uc = {};
  uc.dwStructSize = sizeof uc;
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return false;
  HINTERNET s = WinHttpOpen(L"SuperMarioAllStars-port", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return false;
  WinHttpSetTimeouts(s, 8000, 8000, 8000, 8000);
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                  : nullptr;
  bool ok = r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA,
                                    0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr);
  DWORD status = 0, size = sizeof status;
  if (ok)
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &size, WINHTTP_NO_HEADER_INDEX);
  ok = ok && status == 200;
  while (ok && out.size() < (1u << 20)) {
    DWORD avail = 0, got = 0;
    if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
    const size_t at = out.size();
    out.resize(at + avail);
    if (!WinHttpReadData(r, &out[at], avail, &got)) ok = false;
    out.resize(at + got);
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return ok && !out.empty();
}

void openFolder(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

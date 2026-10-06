#include "agent.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include <windows.h>

namespace rb {

namespace {
std::mutex g_wsMutex;
std::map<std::string, int> g_wsBusy;
}  // namespace

bool acquireWs(const std::string& ws) {
  std::lock_guard<std::mutex> lock(g_wsMutex);
  if (g_wsBusy[ws] > 0) return false;
  g_wsBusy[ws] = 1;
  return true;
}

void releaseWs(const std::string& ws) {
  std::lock_guard<std::mutex> lock(g_wsMutex);
  g_wsBusy[ws] = 0;
}

void ensureDir(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
}

std::string readFileToString(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void writeStringToFile(const std::filesystem::path& p, const std::string& data) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool isReservedDeviceName(const std::string& component) {
  std::string base = component;
  const size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (base.size() < 3 || base.size() > 4) return false;
  std::string upper;
  upper.reserve(base.size());
  for (char c : base) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  static const char* kNames[] = {"CON",  "PRN",  "AUX",  "NUL", "COM1", "COM2",
                                  "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                  "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                  "LPT6", "LPT7", "LPT8", "LPT9"};
  for (const char* n : kNames) {
    if (upper == n) return true;
  }
  return false;
}

bool relPathSafe(const std::string& rel) {
  if (rel.empty() || rel.size() > 4096) return false;
  if (rel[0] == '/' || rel[0] == '\\') return false;
  if (rel.find('\\') != std::string::npos) return false;
  if (rel.find("..") != std::string::npos) return false;
  if (rel.find(':') != std::string::npos) return false;
  size_t start = 0;
  while (start < rel.size()) {
    const size_t slash = rel.find('/', start);
    const std::string comp = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (comp.empty() || comp == ".") return false;
    if (isReservedDeviceName(comp)) return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

std::string nowIso() {
  SYSTEMTIME st;
  GetSystemTime(&st);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                st.wSecond);
  return buf;
}

}  // namespace rb

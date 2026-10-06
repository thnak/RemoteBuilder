#include "agent.hpp"

#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace rb {

namespace {
std::mutex g_wsMutex;
std::map<std::string, int> g_wsBusy;
}

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

}  // namespace rb

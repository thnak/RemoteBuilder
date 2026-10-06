#include "inventory.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>

#include "agent.hpp"

namespace rb {

std::string jsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::filesystem::path wsRoot(const std::string& ws) {
  return std::filesystem::u8path(g_state.root) / std::filesystem::u8path(ws);
}

bool validateWsName(const std::string& ws) {
  if (ws.size() < 1 || ws.size() > 64) return false;
  for (unsigned char c : ws) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
    if (c == '.') continue;
  }
  if (ws.find("..") != std::string::npos) return false;
  if (ws[0] == '.') return false;
  return true;
}

namespace {

int countTree(const std::filesystem::path& dir, long long& bytes) {
  long long files = 0;
  bytes = 0;
  try {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied)) {
      std::error_code dec;
      if (entry.is_directory(dec) || dec) continue;
      std::error_code sec;
      const auto sz = entry.file_size(sec);
      if (sec) continue;
      files++;
      bytes += static_cast<long long>(sz);
    }
  } catch (...) {
    return static_cast<int>(files);
  }
  return static_cast<int>(files);
}

int detectCores() {
  using Fn = DWORD(WINAPI*)(WORD);
  const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  if (k32) {
    if (const Fn fn = reinterpret_cast<Fn>(GetProcAddress(k32, "GetActiveProcessorCount"))) {
      const DWORD n = fn(0xFFFF);
      if (n > 0 && n < 4096) return static_cast<int>(n);
    }
  }
  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? static_cast<int>(si.dwNumberOfProcessors) : 1;
}

std::string detectArch() {
  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
    case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    default: return "unknown";
  }
}

std::string hostname() {
  char buf[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD n = sizeof(buf);
  if (GetComputerNameA(buf, &n) && n > 0) return std::string(buf, n);
  return "worker";
}

}  // namespace

void startLoadSampler() {
  std::thread([] {
    bool havePrev = false;
    ULARGE_INTEGER prevIdle{};
    ULARGE_INTEGER prevKernel{};
    ULARGE_INTEGER prevUser{};
    const auto asUInt = [](const FILETIME& ft) {
      ULARGE_INTEGER v{};
      v.LowPart = ft.dwLowDateTime;
      v.HighPart = ft.dwHighDateTime;
      return v;
    };
    for (;;) {
      FILETIME idleFT{}, kernelFT{}, userFT{};
      if (GetSystemTimes(&idleFT, &kernelFT, &userFT)) {
        ULARGE_INTEGER idle, kernelT, user;
        idle.LowPart = idleFT.dwLowDateTime;
        idle.HighPart = idleFT.dwHighDateTime;
        kernelT.LowPart = kernelFT.dwLowDateTime;
        kernelT.HighPart = kernelFT.dwHighDateTime;
        user.LowPart = userFT.dwLowDateTime;
        user.HighPart = userFT.dwHighDateTime;
        if (havePrev) {
          const unsigned long long dt = (kernelT.QuadPart - prevKernel.QuadPart) +
                                        (user.QuadPart - prevUser.QuadPart);
          unsigned long long idleB = idle.QuadPart - prevIdle.QuadPart;
          int pct = 0;
          if (dt > 0) {
            const unsigned long long busy = dt > idleB ? dt - idleB : 0;
            pct = static_cast<int>((busy * 100 + dt / 2) / dt);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
          }
          g_state.cpuLoadPct.store(pct);
        }
        prevIdle = asUInt(idleFT);
        prevKernel = asUInt(kernelFT);
        prevUser = asUInt(userFT);
        havePrev = true;
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }).detach();
}

std::string buildInventoryJson() {
  const int cores = detectCores();
  const std::string arch = detectArch();
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  GlobalMemoryStatusEx(&mem);
  std::ostringstream o;
  o << "{"
    << "\"name\":\"" << jsonEscape(g_state.machineName.empty() ? hostname() : g_state.machineName) << "\""
    << ",\"agentVer\":\"" << jsonEscape(kAgentVersion) << "\""
    << ",\"os\":\"windows\""
    << ",\"arch\":\"" << arch << "\""
    << ",\"cores\":" << cores
    << ",\"cpuLoadPct\":" << g_state.cpuLoadPct.load()
    << ",\"memTotalMB\":" << (mem.ullTotalPhys / (1024ULL * 1024ULL))
    << ",\"memFreeMB\":" << (mem.ullAvailPhys / (1024ULL * 1024ULL))
    << ",\"jobs\":{\"running\":" << g_state.runningJobs.load()
    << ",\"queued\":" << g_state.queuedJobs.load() << "}"
    << ",\"workspaces\":[";
  std::error_code ec;
  std::filesystem::directory_iterator it(std::filesystem::u8path(g_state.root), ec), end;
  bool first = true;
  if (!ec) {
    for (; it != end; it.increment(ec)) {
      if (ec) break;
      if (!it->is_directory(ec) || ec) continue;
      if (!first) o << ",";
      first = false;
      long long bytes = 0;
      const int files = countTree(it->path(), bytes);
      o << "{\"ws\":\"" << jsonEscape(it->path().filename().u8string()) << "\""
        << ",\"countFiles\":" << files
        << ",\"bytes\":" << bytes << "}";
    }
  }
  o << "]}";
  return o.str();
}

}  // namespace rb

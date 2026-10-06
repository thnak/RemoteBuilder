#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "agent.hpp"
#include "http.hpp"
#include "inventory.hpp"

using namespace rb;  // NOLINT(google-build-using-namespace)

namespace {

std::string localAppData() {
  char buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf));
  if (n > 0 && n < sizeof(buf)) return std::string(buf, n);
  return ".";
}

std::string hostname() {
  char buf[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD n = sizeof(buf);
  if (GetComputerNameA(buf, &n) && n > 0) return std::string(buf, n);
  return "worker";
}

std::string genToken() {
  std::random_device rd;
  std::string token;
  token.reserve(32);
  static const char kHex[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    const uint32_t v = rd();
    token += kHex[(v >> 28) & 0xF];
    token += kHex[(v >> 20) & 0xF];
  }
  return token;
}

void printHelp(const char* prog) {
  std::printf(
      "RemoteBuilder agent %s\n"
      "usage: %s [--port N] [--token T] [--root DIR] [--name NAME] [--maxjobs N]\n"
      "  --port   HTTP listen port (default 7333)\n"
      "  --token  auth token (default: auto-generate, stored in %%LOCALAPPDATA%%\\rb-agent\\token.txt)\n"
      "  --root   workspaces root (default: %%LOCALAPPDATA%%\\rb-agent\\workspaces)\n"
      "  --name   machine name reported to the coordinator (default: hostname)\n"
      "  --maxjobs concurrent jobs (default 2)\n",
      kAgentVersion, prog);
}

}  // namespace

int main(int argc, char** argv) {
  std::string tokenArg, rootArg, nameArg;
  int port = 7333;
  int maxJobs = 2;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--port") port = std::atoi(next("--port").c_str());
    else if (a == "--token") tokenArg = next("--token");
    else if (a == "--root") rootArg = next("--root");
    else if (a == "--name") nameArg = next("--name");
    else if (a == "--maxjobs") maxJobs = std::atoi(next("--maxjobs").c_str());
    else if (a == "--help" || a == "-h") { printHelp(argv[0]); return 0; }
    else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      printHelp(argv[0]);
      return 2;
    }
  }
  if (port <= 0 || port > 65535) {
    std::fprintf(stderr, "bad --port\n");
    return 2;
  }

  g_state.port = port;
  g_state.maxJobs = maxJobs < 1 ? 1 : maxJobs;
  g_state.machineName = nameArg.empty() ? hostname() : nameArg;
  g_state.root = rootArg.empty() ? (localAppData() + "\\rb-agent\\workspaces") : rootArg;
  ensureDir(std::filesystem::u8path(g_state.root));

  std::string token = tokenArg;
  const bool generated = token.empty();
  if (generated) {
    const std::filesystem::path dir = std::filesystem::u8path(localAppData() + "\\rb-agent");
    const std::filesystem::path tokenFile = dir / "token.txt";
    token = readFileToString(tokenFile);
    if (token.empty()) {
      token = genToken();
      ensureDir(dir);
      writeStringToFile(tokenFile, token);
    }
  }
  g_state.token = token;
  startLoadSampler();

  std::printf("RemoteBuilder agent %s\n", kAgentVersion);
  std::printf("  name   : %s\n", g_state.machineName.c_str());
  std::printf("  port   : %d\n", g_state.port);
  std::printf("  root   : %s\n", g_state.root.c_str());
  std::printf("  token  : %s%s\n", token.c_str(), generated ? " (generated)" : "");
  std::fflush(stdout);
  return runServer();
}

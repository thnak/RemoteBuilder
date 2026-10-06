#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include "jobs.hpp"

namespace rb {

struct AgentState {
  std::string machineName;
  std::string token;
  std::string root;
  int port = 7333;
  int maxJobs = 2;
  std::unique_ptr<JobRunner> jobs;
  std::atomic<int> runningJobs{0};
  std::atomic<int> queuedJobs{0};
  std::atomic<int> cpuLoadPct{0};
};

extern AgentState g_state;

inline const char* kAgentVersion = "0.1.0";

bool validateWsName(const std::string& ws);
bool acquireWs(const std::string& ws);
void releaseWs(const std::string& ws);
std::filesystem::path wsRoot(const std::string& ws);
std::string jsonEscape(const std::string& in);
void ensureDir(const std::filesystem::path& p);
std::string localAppDataDir();
std::string randomHexToken();
std::string readFileToString(const std::filesystem::path& p);
void writeStringToFile(const std::filesystem::path& p, const std::string& data);

bool relPathSafe(const std::string& rel);
std::string nowIso();

}  // namespace rb

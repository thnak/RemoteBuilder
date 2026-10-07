#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "json.hpp"

namespace rb {

enum class JobStatus { Queued, Running, Done, Killed, Failed };

struct JobRecord {
  std::string id;
  std::string ws;
  std::string cmd;
  std::string cwdRel;
  int timeoutSec = 0;
  std::map<std::string, std::string> env;
  std::atomic<int> status{static_cast<int>(JobStatus::Queued)};
  std::atomic<bool> killRequested{false};
  DWORD exitCode = 0;
  std::string startedAt;
  std::string endedAt;
  std::string logPath;
  HANDLE jobObj = nullptr;
  HANDLE proc = nullptr;
};

class JobRunner {
 public:
  JobRunner(int maxJobs);
  ~JobRunner();

  std::string enqueue(const std::string& ws, const JsonValue& request,
                        std::string& err);
  bool get(const std::string& id, JsonValue& out, std::string& err);
  bool list(JsonValue& out, std::string& err);
  bool kill(const std::string& id, std::string& err);
  bool readLog(const std::string& id, long long offset, std::string& out,
                 long long& total, std::string& err);
  long long logBytes(const std::string& id, std::string& err);

 private:
  void dispatchLoop();
  void runJob(const std::string& id);

  int maxJobs_;
  std::thread dispatcher_;
  std::mutex mutex_;
  std::condition_variable queueCv_;
  std::deque<std::string> queue_;
  std::map<std::string, JobRecord> jobs_;
  std::atomic<long long> nextId_{1};
  std::atomic<bool> shuttingDown_{false};
  std::atomic<int> runningCount_{0};
};

}  // namespace rb

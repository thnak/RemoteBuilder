#include "jobs.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>

#include "agent.hpp"

namespace rb {

namespace {

std::string jobStatusName(int status) {
  switch (static_cast<JobStatus>(status)) {
    case JobStatus::Queued: return "queued";
    case JobStatus::Running: return "running";
    case JobStatus::Done: return "done";
    case JobStatus::Killed: return "killed";
    case JobStatus::Failed: return "failed";
  }
  return "unknown";
}

long long logFileSize(const std::filesystem::path& p) {
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (!GetFileAttributesExW(p.wstring().c_str(),
                            GetFileExInfoStandard, &fa)) {
    return -1;
  }
  ULARGE_INTEGER sz;
  sz.HighPart = fa.nFileSizeHigh;
  sz.LowPart = fa.nFileSizeLow;
  return static_cast<long long>(sz.QuadPart);
}

std::string buildEnvBlock(
    const std::map<std::string, std::string>& overrides) {
  std::map<std::string, std::string> env;
  char* envs = GetEnvironmentStringsA();
  if (envs) {
    for (char* p = envs; *p != '\0';) {
      const std::string entry(p);
      const size_t eq = entry.find('=');
      if (eq != std::string::npos && eq > 0) {
        env[entry.substr(0, eq)] = entry.substr(eq + 1);
      }
      p += entry.size() + 1;
    }
    FreeEnvironmentStringsA(envs);
  }
  for (const auto& [k, v] : overrides) {
    if (k.empty()) continue;
    env[k] = v;
  }
  std::string block;
  for (const auto& [k, v] : env) {
    block += k;
    block += '=';
    block += v;
    block += '\0';
  }
  block += '\0';
  return block;
}

}  // namespace

JobRunner::JobRunner(int maxJobs) : maxJobs_(maxJobs < 1 ? 1 : maxJobs) {
  dispatcher_ = std::thread(&JobRunner::dispatchLoop, this);
}

JobRunner::~JobRunner() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shuttingDown_ = true;
  }
  queueCv_.notify_all();
  if (dispatcher_.joinable()) {
    dispatcher_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [id, job] : jobs_) {
    if (job.jobObj) CloseHandle(job.jobObj);
    if (job.proc) CloseHandle(job.proc);
  }
}

std::string JobRunner::enqueue(const std::string& ws,
                                   const JsonValue& request,
                                   std::string& err) {
  const JsonValue* cmd = request.find("cmd");
  if (!cmd || cmd->type != JsonValue::String || cmd->strV.empty()) {
    err = "request body must have cmd (string)";
    return {};
  }
  std::string cwdRel;
  if (const JsonValue* cwd = request.find("cwd")) {
    if (cwd->type == JsonValue::String) {
      if (!relPathSafe(cwd->strV)) {
        err = "unsafe cwd path";
        return {};
      }
      cwdRel = cwd->strV;
    }
  }
  int timeoutSec = 0;
  if (const JsonValue* timeout = request.find("timeoutSec")) {
    if (timeout->type == JsonValue::Number) {
      timeoutSec = static_cast<int>(timeout->numV);
      if (timeoutSec < 0) timeoutSec = 0;
    }
  }
  std::map<std::string, std::string> env;
  if (const JsonValue* envV = request.find("env")) {
    if (envV->type == JsonValue::Object) {
      for (const auto& [k, v] : envV->objV) {
        if (v.type == JsonValue::String) {
          env[k] = v.strV;
        }
      }
    }
  }
  std::string id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    id = "j" + std::to_string(nextId_++);
    JobRecord& job = jobs_.try_emplace(id).first->second;
    job.id = id;
    job.ws = ws;
    job.cmd = cmd->strV;
    job.cwdRel = cwdRel;
    job.timeoutSec = timeoutSec;
    job.env = env;
    job.logPath =
        (wsRoot(ws) / "jobs" / id / "log.txt").u8string();
  }
  if (!acquireWs(ws)) {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.erase(id);
    err = "workspace busy (job running)";
    return {};
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(id);
  }
  queueCv_.notify_all();
  return id;
}

void JobRunner::dispatchLoop() {
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    queueCv_.wait(lock, [this] {
      return !queue_.empty() || shuttingDown_.load();
    });
    if (shuttingDown_.load() && queue_.empty()) {
      return;
    }
    if (queue_.empty()) {
      continue;
    }
    const int active = runningCount_.load();
    if (active >= maxJobs_) {
      queueCv_.wait_for(lock, std::chrono::milliseconds(250));
      continue;
    }
    const std::string id = queue_.front();
    queue_.pop_front();
    runningCount_++;
    g_state.queuedJobs.store(
        static_cast<int>(queue_.size()));
    lock.unlock();
    runJob(id);
    lock.lock();
    runningCount_--;
  }
}

void JobRunner::runJob(const std::string& id) {
  JobRecord* job = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      return;
    }
    job = &it->second;
    job->status.store(static_cast<int>(JobStatus::Running));
    job->startedAt = nowIso();
  }
  g_state.runningJobs.fetch_add(1);

  const std::filesystem::path logPath =
      std::filesystem::u8path(job->logPath);
  ensureDir(logPath.parent_path());
  const std::wstring logPathW = logPath.wstring();
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE hLog = CreateFileW(
      logPathW.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  const std::filesystem::path cwd =
      job->cwdRel.empty()
          ? wsRoot(job->ws)
          : wsRoot(job->ws) / std::filesystem::u8path(job->cwdRel);
  std::string envBlock = buildEnvBlock(job->env);
  std::string commandLine =
      "cmd.exe /d /s /c \"" + job->cmd + " 2>&1\"";

  HANDLE hJob = CreateJobObjectA(nullptr, nullptr);
  if (hJob) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                            &jeli, sizeof(jeli));
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = hLog;
  si.hStdError = hLog;
  PROCESS_INFORMATION pi{};
  std::string cwdA = cwd.u8string();

  const BOOL ok = CreateProcessA(
      nullptr, &commandLine[0], nullptr, nullptr, TRUE,
      CREATE_SUSPENDED, envBlock.empty() ? nullptr : &envBlock[0],
      cwdA.c_str(), &si, &pi);

  if (!ok || hJob == nullptr) {
    job->status.store(static_cast<int>(JobStatus::Failed));
    job->exitCode = ok ? 0 : GetLastError();
    job->endedAt = nowIso();
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (hLog) CloseHandle(hLog);
    if (hJob) CloseHandle(hJob);
    job->proc = nullptr;
    job->jobObj = nullptr;
    g_state.runningJobs.fetch_sub(1);
    releaseWs(job->ws);
    return;
  }

  AssignProcessToJobObject(hJob, pi.hProcess);
  job->jobObj = hJob;
  job->proc = pi.hProcess;
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  const DWORD waitMs = job->timeoutSec > 0
                           ? static_cast<DWORD>(job->timeoutSec) * 1000
                           : INFINITE;
  const DWORD wr = WaitForSingleObject(pi.hProcess, waitMs);
  if (wr == WAIT_TIMEOUT) {
    job->killRequested.store(true);
    if (hJob) TerminateJobObject(hJob, 1);
    WaitForSingleObject(pi.hProcess, 10000);
  }
  GetExitCodeProcess(pi.hProcess, &job->exitCode);
  job->status.store(static_cast<int>(
      job->killRequested.load() ? JobStatus::Killed : JobStatus::Done));
  job->endedAt = nowIso();

  CloseHandle(pi.hProcess);
  CloseHandle(hJob);
  CloseHandle(hLog);
  job->proc = nullptr;
  job->jobObj = nullptr;
  g_state.runningJobs.fetch_sub(1);
  releaseWs(job->ws);
}

bool JobRunner::get(const std::string& id, JsonValue& out,
                        std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = jobs_.find(id);
  if (it == jobs_.end()) {
    err = "no such job";
    return false;
  }
  const JobRecord& job = it->second;
  out.type = JsonValue::Object;
  JsonValue v;
  v.type = JsonValue::String;
  v.strV = job.id;
  out.objV["jobId"] = v;
  v.strV = job.ws;
  out.objV["ws"] = v;
  v.strV = job.cmd;
  out.objV["cmd"] = v;
  v.strV = jobStatusName(job.status.load());
  out.objV["status"] = v;
  v.type = JsonValue::Number;
  v.numV = static_cast<double>(job.exitCode);
  out.objV["exitCode"] = v;
  v.numV = static_cast<double>(job.timeoutSec);
  out.objV["timeoutSec"] = v;
  v.type = JsonValue::String;
  v.strV = job.startedAt;
  out.objV["startedAt"] = v;
  v.strV = job.endedAt;
  out.objV["endedAt"] = v;
  v.type = JsonValue::Number;
  const long long total = logFileSize(
      std::filesystem::u8path(job.logPath));
  v.numV = static_cast<double>(total < 0 ? 0 : total);
  out.objV["logBytes"] = v;
  return true;
}

bool JobRunner::list(JsonValue& out, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  out.type = JsonValue::Object;
  JsonValue arr;
  arr.type = JsonValue::Array;
  for (const auto& [id, job] : jobs_) {
    JsonValue j;
    j.type = JsonValue::Object;
    JsonValue v;
    v.type = JsonValue::String;
    v.strV = job.id;
    j.objV["jobId"] = v;
    v.strV = job.ws;
    j.objV["ws"] = v;
    v.strV = job.cmd;
    j.objV["cmd"] = v;
    v.strV = jobStatusName(job.status.load());
    j.objV["status"] = v;
    v.type = JsonValue::Number;
    v.numV = static_cast<double>(job.exitCode);
    j.objV["exitCode"] = v;
    v.numV = static_cast<double>(job.timeoutSec);
    j.objV["timeoutSec"] = v;
    v.type = JsonValue::String;
    v.strV = job.startedAt;
    j.objV["startedAt"] = v;
    v.strV = job.endedAt;
    j.objV["endedAt"] = v;
    v.type = JsonValue::Number;
    const long long total = logFileSize(
        std::filesystem::u8path(job.logPath));
    v.numV = static_cast<double>(total < 0 ? 0 : total);
    j.objV["logBytes"] = v;
    arr.arrV.push_back(j);
  }
  out.objV["jobs"] = arr;
  return true;
}

bool JobRunner::kill(const std::string& id, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = jobs_.find(id);
  if (it == jobs_.end()) {
    err = "no such job";
    return false;
  }
  JobRecord& job = it->second;
  const int status = job.status.load();
  if (status == static_cast<int>(JobStatus::Done) ||
      status == static_cast<int>(JobStatus::Killed) ||
      status == static_cast<int>(JobStatus::Failed)) {
    err = "job already finished";
    return false;
  }
  job.killRequested.store(true);
  if (job.jobObj) {
    TerminateJobObject(job.jobObj, 1);
  } else if (job.proc) {
    TerminateProcess(job.proc, 1);
  }
  return true;
}

long long JobRunner::logBytes(const std::string& id, std::string& err) {
  std::filesystem::path logPath;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      err = "no such job";
      return -1;
    }
    logPath = std::filesystem::u8path(it->second.logPath);
  }
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (!GetFileAttributesExW(logPath.wstring().c_str(),
                            GetFileExInfoStandard, &fa)) {
    err = "log not found";
    return -1;
  }
  ULARGE_INTEGER sz;
  sz.HighPart = fa.nFileSizeHigh;
  sz.LowPart = fa.nFileSizeLow;
  return static_cast<long long>(sz.QuadPart);
}

bool JobRunner::readLog(const std::string& id, long long offset,
                            std::string& out, long long& total,
                            std::string& err) {
  total = logBytes(id, err);
  if (total < 0) return false;
  if (offset < 0) offset = 0;
  if (offset > total) {
    err = "offset beyond end of log";
    return false;
  }
  const HANDLE h = CreateFileW(
      std::filesystem::u8path([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_[id].logPath;
      }())
          .wstring()
          .c_str(),
      GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    err = "cannot open log";
    return false;
  }
  LARGE_INTEGER dist;
  dist.QuadPart = offset;
  if (!SetFilePointerEx(h, dist, nullptr, FILE_BEGIN)) {
    err = "seek failed";
    CloseHandle(h);
    return false;
  }
  out.clear();
  out.resize(static_cast<size_t>(total - offset));
  DWORD read = 0;
  if (!ReadFile(h, out.data(),
                static_cast<DWORD>(out.size()), &read, nullptr)) {
    err = "read failed";
    CloseHandle(h);
    return false;
  }
  out.resize(read);
  CloseHandle(h);
  return true;
}

}  // namespace rb

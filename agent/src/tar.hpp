#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace rb {

struct ManifestEntry {
  std::string p;
  long long s;
  std::string h;
};

struct PackEntry {
  std::string path;
  std::string data;
};

std::string packTar(const std::vector<PackEntry>& entries);

struct TarStats {
  int appliedFiles = 0;
  long long appliedBytes = 0;
  int manifestFiles = 0;
  int prunedFiles = 0;
};

bool parseManifestJson(const std::string& text, std::vector<ManifestEntry>& out,
                       std::string& err);

class TarApplier {
 public:
  explicit TarApplier(const std::filesystem::path& wsDir);

  bool feed(const char* data, size_t len);
  bool finish();

  const std::string& error() const { return error_; }
  const TarStats& stats() const { return stats_; }

 private:
  bool beginEntry();
  bool openCurrent();

  std::filesystem::path wsDir_;
  std::string error_;
  TarStats stats_;

  enum class Stage { Header, Payload, Skip, Done };
  Stage stage_ = Stage::Header;
  char header_[512]{};
  size_t headerFilled_ = 0;

  std::ofstream out_;
  std::string curRel_;
  long long curSize_ = 0;
  long long remaining_ = 0;
  bool curIsManifest_ = false;
  std::string manifestBuf_;
  bool haveManifest_ = false;

  long long skipRemaining_ = 0;
};

}  // namespace rb

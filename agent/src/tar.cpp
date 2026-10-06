#include "tar.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>

#include "agent.hpp"

namespace rb {

namespace {

std::string readField(const char* field, size_t len) {
  std::string out(field, strnlen(field, len));
  return out;
}

long long readOctal(const char* field, size_t len) {
  long long value = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = field[i];
    if (c >= '0' && c <= '7') {
      value = value * 8 + (c - '0');
    } else if (c == 0 || c == ' ') {
      break;
    }
  }
  return value;
}

std::string utf8FromCodepoint(unsigned cp) {
  std::string out;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

bool isHex40(const std::string& s) {
  if (s.size() != 40) return false;
  for (char c : s) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!ok) return false;
  }
  return true;
}

bool unescapeJsonString(const std::string& raw, std::string& out, std::string& err) {
  out.clear();
  for (size_t i = 0; i < raw.size(); i++) {
    const char c = raw[i];
    if (c != '\\') {
      out += c;
      continue;
    }
    if (i + 1 >= raw.size()) {
      err = "dangling escape";
      return false;
    }
    const char e = raw[++i];
    switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        if (i + 4 >= raw.size()) {
          err = "short \\u escape";
          return false;
        }
        unsigned cp = 0;
        for (int j = 0; j < 4; j++) {
          const char h = raw[i + 1 + j];
          cp <<= 4;
          if (h >= '0' && h <= '9') cp += h - '0';
          else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
          else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
          else {
            err = "bad \\u escape";
            return false;
          }
        }
        i += 4;
        out += utf8FromCodepoint(cp);
        break;
      }
      default:
        err = "unknown escape";
        return false;
    }
  }
  return true;
}

}  // namespace

bool parseManifestJson(const std::string& text, std::vector<ManifestEntry>& out,
                       std::string& err) {
  out.clear();
  size_t i = 0;
  const auto skipWs = [&] {
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
      i++;
    }
  };
  const auto expect = [&](char c) {
    skipWs();
    if (i >= text.size() || text[i] != c) {
      err = std::string("expected '") + c + "' at " + std::to_string(i);
      return false;
    }
    i++;
    return true;
  };
  const auto readString = [&](std::string& out) {
    skipWs();
    if (i >= text.size() || text[i] != '"') {
      err = "expected string at " + std::to_string(i);
      return false;
    }
    i++;
    std::string raw;
    while (i < text.size() && text[i] != '"') {
      raw += text[i++];
    }
    if (i >= text.size()) {
      err = "unterminated string";
      return false;
    }
    i++;
    return unescapeJsonString(raw, out, err);
  };

  if (!expect('{')) return false;
  bool first = true;
  bool sawV = false;
  bool sawFiles = false;
  for (;;) {
    skipWs();
    if (i < text.size() && text[i] == '}') {
      i++;
      break;
    }
    if (!first && !expect(',')) return false;
    first = false;
    std::string key;
    if (!readString(key)) return false;
    if (!expect(':')) return false;
    if (key == "v") {
      skipWs();
      if (i >= text.size() || text[i] != '1') {
        err = "unsupported manifest version";
        return false;
      }
      i++;
      sawV = true;
    } else if (key == "files") {
      if (!expect('[')) return false;
      bool firstEntry = true;
      for (;;) {
        skipWs();
        if (i < text.size() && text[i] == ']') {
          i++;
          break;
        }
        if (!firstEntry && !expect(',')) return false;
        firstEntry = false;
        if (!expect('{')) return false;
        ManifestEntry e;
        bool hasP = false, hasS = false, hasH = false;
        bool entryFirst = true;
        for (;;) {
          skipWs();
          if (i < text.size() && text[i] == '}') {
            i++;
            break;
          }
          if (!entryFirst && !expect(',')) return false;
          entryFirst = false;
          std::string ek;
          if (!readString(ek)) return false;
          if (!expect(':')) return false;
          if (ek == "p") {
            if (!readString(e.p)) return false;
            hasP = true;
          } else if (ek == "s") {
            skipWs();
            const size_t start = i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') i++;
            if (start == i) {
              err = "expected number at " + std::to_string(i);
              return false;
            }
            e.s = std::stoll(text.substr(start, i - start));
            hasS = true;
          } else if (ek == "h") {
            if (!readString(e.h)) return false;
            hasH = true;
          } else {
            err = "unknown manifest field '" + ek + "'";
            return false;
          }
        }
        if (!hasP || !hasS || !hasH) {
          err = "manifest entry missing p/s/h";
          return false;
        }
        if (!relPathSafe(e.p)) {
          err = "unsafe manifest path '" + e.p + "'";
          return false;
        }
        if (e.s < 0) {
          err = "negative size";
          return false;
        }
        if (!isHex40(e.h)) {
          err = "bad hash for '" + e.p + "'";
          return false;
        }
        out.push_back(std::move(e));
      }
      sawFiles = true;
    } else {
      err = "unknown manifest key '" + key + "'";
      return false;
    }
  }
  skipWs();
  if (i != text.size()) {
    err = "trailing data";
    return false;
  }
  if (!sawV || !sawFiles) {
    err = "manifest missing v/files";
    return false;
  }
  return true;
}

TarApplier::TarApplier(const std::filesystem::path& wsDir) : wsDir_(wsDir) {}

bool TarApplier::beginEntry() {
  bool allZero = true;
  for (char c : header_) {
    if (c != 0) {
      allZero = false;
      break;
    }
  }
  if (allZero) {
    stage_ = Stage::Done;
    return true;
  }
  const std::string name = readField(header_, 100);
  const std::string prefix = readField(header_ + 345, 155);
  const std::string rel = prefix.empty() ? name : prefix + "/" + name;
  const char typeflag = header_[156];
  curSize_ = readOctal(header_ + 124, 12);
  if (!relPathSafe(rel)) {
    error_ = "unsafe tar path '" + rel + "'";
    return false;
  }
  curRel_ = rel;
  if (typeflag == '5') {
    skipRemaining_ = curSize_;
    stage_ = Stage::Skip;
    return true;
  }
  if (typeflag != '0' && typeflag != 0) {
    skipRemaining_ = curSize_;
    stage_ = Stage::Skip;
    return true;
  }
  curIsManifest_ = (rel == "manifest.json");
  if (curIsManifest_) {
    manifestBuf_.clear();
  } else if (!openCurrent()) {
    return false;
  }
  remaining_ = curSize_;
  stage_ = Stage::Payload;
  return true;
}

bool TarApplier::openCurrent() {
  const std::filesystem::path target = wsDir_ / std::filesystem::u8path(curRel_);
  std::error_code ec;
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
      error_ = "mkdir failed for " + curRel_;
      return false;
    }
  }
  out_.open(target, std::ios::binary | std::ios::trunc);
  if (!out_) {
    error_ = "cannot open " + curRel_;
    return false;
  }
  return true;
}

bool TarApplier::feed(const char* data, size_t len) {
  while (len > 0) {
    switch (stage_) {
      case Stage::Done:
        return true;
      case Stage::Header: {
        const size_t need = 512 - headerFilled_;
        const size_t take = need < len ? need : len;
        memcpy(header_ + headerFilled_, data, take);
        headerFilled_ += take;
        data += take;
        len -= take;
        if (headerFilled_ == 512 && !beginEntry()) return false;
        break;
      }
      case Stage::Payload: {
        const size_t take = static_cast<size_t>(
            remaining_ < static_cast<long long>(len) ? remaining_ : len);
        if (curIsManifest_) {
          manifestBuf_.append(data, take);
        } else {
          out_.write(data, static_cast<std::streamsize>(take));
          if (!out_) {
            error_ = "write failed for " + curRel_;
            return false;
          }
        }
        remaining_ -= take;
        data += take;
        len -= take;
        if (remaining_ == 0) {
          if (curIsManifest_) {
            haveManifest_ = true;
          } else {
            out_.close();
            stats_.appliedFiles++;
            stats_.appliedBytes += curSize_;
          }
          const long long pad = (512 - (curSize_ % 512)) % 512;
          if (pad > 0) {
            skipRemaining_ = pad;
            stage_ = Stage::Skip;
          } else {
            stage_ = Stage::Header;
            headerFilled_ = 0;
          }
        }
        break;
      }
      case Stage::Skip: {
        const size_t take = static_cast<size_t>(
            skipRemaining_ < static_cast<long long>(len) ? skipRemaining_ : len);
        skipRemaining_ -= take;
        data += take;
        len -= take;
        if (skipRemaining_ == 0) {
          stage_ = Stage::Header;
          headerFilled_ = 0;
        }
        break;
      }
    }
  }
  return true;
}

bool TarApplier::finish() {
  if (stage_ != Stage::Done && stage_ != Stage::Header) {
    error_ = "tar ended mid-entry";
    return false;
  }
  if (!haveManifest_) {
    error_ = "tar ended without manifest.json entry";
    return false;
  }
  std::vector<ManifestEntry> entries;
  std::string err;
  if (!parseManifestJson(manifestBuf_, entries, err)) {
    error_ = "bad manifest.json: " + err;
    return false;
  }
  std::vector<ManifestEntry> oldEntries;
  const std::filesystem::path manifestPath = wsDir_ / "manifest.json";
  const std::string oldText = readFileToString(manifestPath);
  if (!oldText.empty() &&
      !parseManifestJson(oldText, oldEntries, err)) {
    error_ = "existing manifest.json is corrupt: " + err;
    return false;
  }
  std::set<std::string> fresh;
  for (const ManifestEntry& e : entries) fresh.insert(e.p);
  for (const ManifestEntry& old : oldEntries) {
    if (fresh.count(old.p) == 0) {
      std::error_code ec;
      const std::filesystem::path stale = wsDir_ / std::filesystem::u8path(old.p);
      const bool removed = std::filesystem::remove(stale, ec);
      if (removed) stats_.prunedFiles++;
    }
  }
  writeStringToFile(manifestPath, manifestBuf_);
  stats_.manifestFiles = static_cast<int>(entries.size());
  return true;
}

namespace {

void writeOctalField(char* field, size_t digits, long long value) {
  std::string s;
  long long v = value;
  if (v == 0) s = "0";
  while (v > 0) {
    s += static_cast<char>('0' + (v % 8));
    v /= 8;
  }
  std::reverse(s.begin(), s.end());
  if (s.size() > digits) s = s.substr(s.size() - digits);
  while (s.size() < digits) s.insert(s.begin(), '0');
  for (size_t i = 0; i < digits; i++) field[i] = s[i];
  field[digits] = 0;
}

void writeStrField(char* field, size_t len, const std::string& value) {
  const size_t n = value.size() < len ? value.size() : len;
  memcpy(field, value.data(), n);
  if (n < len) field[n] = 0;
}

void splitTarPath(const std::string& rel, std::string& name,
                  std::string& prefix) {
  if (rel.size() <= 100) {
    name = rel;
    prefix.clear();
    return;
  }
  size_t idx = rel.find_last_of('/');
  while (idx != std::string::npos) {
    const std::string p = rel.substr(0, idx);
    const std::string n = rel.substr(idx + 1);
    if (n.size() > 0 && n.size() <= 100 && p.size() <= 155) {
      name = n;
      prefix = p;
      return;
    }
    idx = rel.find_last_of('/', idx - 1);
  }
  name = rel;
  prefix.clear();
}

std::string packHeader(const PackEntry& e) {
  std::string header(512, '\0');
  std::string name;
  std::string prefix;
  splitTarPath(e.path, name, prefix);
  writeStrField(&header[0], 100, name);
  writeOctalField(&header[100], 7, 0644);
  writeOctalField(&header[108], 7, 0);
  writeOctalField(&header[116], 7, 0);
  writeOctalField(&header[124], 11,
                  static_cast<long long>(e.data.size()));
  writeOctalField(&header[136], 11, 0);
  for (size_t i = 148; i < 156; i++) header[i] = ' ';
  header[156] = '0';
  memcpy(&header[257], "ustar", 5);
  header[262] = 0;
  memcpy(&header[263], "00", 2);
  writeStrField(&header[265], 32, "remotebuilder");
  writeStrField(&header[297], 32, "remotebuilder");
  writeOctalField(&header[329], 7, 0);
  writeOctalField(&header[337], 7, 0);
  writeStrField(&header[345], 155, prefix);
  long long sum = 0;
  for (char c : header) sum += static_cast<unsigned char>(c);
  writeOctalField(&header[148], 6, sum);
  header[154] = 0;
  header[155] = ' ';
  return header;
}

}  // namespace

std::string packTar(const std::vector<PackEntry>& entries) {
  std::string out;
  for (const PackEntry& e : entries) {
    out += packHeader(e);
    out += e.data;
    const size_t pad = (512 - (e.data.size() % 512)) % 512;
    out.append(pad, '\0');
  }
  out.append(1024, '\0');
  return out;
}

}  // namespace rb

#include "json.hpp"

#include <cctype>

namespace rb {

const JsonValue* JsonValue::find(const std::string& key) const {
  if (type != Object) return nullptr;
  const auto it = objV.find(key);
  if (it == objV.end()) return nullptr;
  return &it->second;
}

namespace {

struct Parser {
  const std::string& text;
  size_t i = 0;
  std::string err;

  explicit Parser(const std::string& t) : text(t) {}

  void skipWs() {
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
            text[i] == '\r')) {
      i++;
    }
  }

  bool peek(char c) {
    skipWs();
    return i < text.size() && text[i] == c;
  }

  bool expect(char c) {
    skipWs();
    if (i >= text.size() || text[i] != c) {
      err = std::string("expected '") + c + "' at " + std::to_string(i);
      return false;
    }
    i++;
    return true;
  }

  bool parseString(std::string& out) {
    skipWs();
    if (i >= text.size() || text[i] != '"') {
      err = "expected string at " + std::to_string(i);
      return false;
    }
    i++;
    out.clear();
    while (i < text.size() && text[i] != '"') {
      const char c = text[i++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= text.size()) {
        err = "dangling escape";
        return false;
      }
      const char e = text[i++];
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
          if (i + 4 > text.size()) {
            err = "short \\u escape";
            return false;
          }
          unsigned cp = 0;
          for (int j = 0; j < 4; j++) {
            const char h = text[i++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp += h - '0';
            else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
            else {
              err = "bad \\u escape";
              return false;
            }
          }
          if (cp < 0x80) {
            out += static_cast<char>(cp);
          } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          }
          break;
        }
        default:
          err = "unknown escape";
          return false;
      }
    }
    if (i >= text.size()) {
      err = "unterminated string";
      return false;
    }
    i++;
    return true;
  }

  bool parseValue(JsonValue& out) {
    skipWs();
    if (i >= text.size()) {
      err = "unexpected end";
      return false;
    }
    const char c = text[i];
    if (c == '{') return parseObject(out);
    if (c == '[') return parseArray(out);
    if (c == '"') {
      out.type = JsonValue::String;
      return parseString(out.strV);
    }
    if (c == 't' || c == 'f') return parseBool(out);
    if (c == 'n') return parseNull(out);
    return parseNumber(out);
  }

  bool parseObject(JsonValue& out) {
    if (!expect('{')) return false;
    out.type = JsonValue::Object;
    bool first = true;
    for (;;) {
      if (peek('}')) {
        i++;
        break;
      }
      if (!first && !expect(',')) return false;
      first = false;
      std::string key;
      if (!parseString(key)) return false;
      if (!expect(':')) return false;
      JsonValue v;
      if (!parseValue(v)) return false;
      out.objV[key] = std::move(v);
    }
    return true;
  }

  bool parseArray(JsonValue& out) {
    if (!expect('[')) return false;
    out.type = JsonValue::Array;
    bool first = true;
    for (;;) {
      if (peek(']')) {
        i++;
        break;
      }
      if (!first && !expect(',')) return false;
      first = false;
      JsonValue v;
      if (!parseValue(v)) return false;
      out.arrV.push_back(std::move(v));
    }
    return true;
  }

  bool parseBool(JsonValue& out) {
    if (text.compare(i, 4, "true") == 0) {
      out.type = JsonValue::Bool;
      out.boolV = true;
      i += 4;
      return true;
    }
    if (text.compare(i, 5, "false") == 0) {
      out.type = JsonValue::Bool;
      out.boolV = false;
      i += 5;
      return true;
    }
    err = "bad literal at " + std::to_string(i);
    return false;
  }

  bool parseNull(JsonValue& out) {
    if (text.compare(i, 4, "null") == 0) {
      out.type = JsonValue::Null;
      i += 4;
      return true;
    }
    err = "bad literal at " + std::to_string(i);
    return false;
  }

  bool parseNumber(JsonValue& out) {
    const size_t start = i;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) i++;
    while (i < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[i])) ||
            text[i] == '.' || text[i] == 'e' || text[i] == 'E' ||
            text[i] == '+' || text[i] == '-')) {
      i++;
    }
    if (start == i) {
      err = "expected number at " + std::to_string(i);
      return false;
    }
    out.type = JsonValue::Number;
    out.numV = std::stod(text.substr(start, i - start));
    return true;
  }
};

}  // namespace

bool parseJson(const std::string& text, JsonValue& out, std::string& err) {
  Parser p(text);
  if (!p.parseValue(out)) {
    err = p.err;
    return false;
  }
  p.skipWs();
  if (p.i != text.size()) {
    err = "trailing data at " + std::to_string(p.i);
    return false;
  }
  return true;
}

namespace {

void stringifyValue(const JsonValue& v, std::string& out) {
  switch (v.type) {
    case JsonValue::Null:
      out += "null";
      break;
    case JsonValue::Bool:
      out += v.boolV ? "true" : "false";
      break;
    case JsonValue::Number: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g", v.numV);
      out += buf;
      break;
    }
    case JsonValue::String: {
      out += '"';
      for (char c : v.strV) {
        switch (c) {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\b': out += "\\b"; break;
          case '\f': out += "\\f"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '\t': out += "\\t"; break;
          default:
            if (static_cast<unsigned char>(c) < 0x20) {
              char esc[8];
              std::snprintf(esc, sizeof(esc), "\\u%04x",
                            static_cast<unsigned char>(c));
              out += esc;
            } else {
              out += c;
            }
        }
      }
      out += '"';
      break;
    }
    case JsonValue::Array:
      out += '[';
      for (size_t i = 0; i < v.arrV.size(); i++) {
        if (i) out += ',';
        stringifyValue(v.arrV[i], out);
      }
      out += ']';
      break;
    case JsonValue::Object:
      out += '{';
      for (auto it = v.objV.begin(); it != v.objV.end(); ++it) {
        if (it != v.objV.begin()) out += ',';
        out += '"';
        for (char c : it->first) {
          if (c == '"' || c == '\\') out += '\\';
          out += c;
        }
        out += "\":";
        stringifyValue(it->second, out);
      }
      out += '}';
      break;
  }
}

}  // namespace

std::string stringifyJson(const JsonValue& v) {
  std::string out;
  stringifyValue(v, out);
  return out;
}

}  // namespace rb

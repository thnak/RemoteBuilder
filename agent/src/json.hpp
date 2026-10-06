#pragma once
#include <map>
#include <string>
#include <vector>

namespace rb {

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool boolV = false;
  double numV = 0;
  std::string strV;
  std::vector<JsonValue> arrV;
  JsonObject objV;

  const JsonValue* find(const std::string& key) const;
};

bool parseJson(const std::string& text, JsonValue& out, std::string& err);
std::string stringifyJson(const JsonValue& v);

}  // namespace rb

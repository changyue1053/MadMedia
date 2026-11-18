#include "core/util/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace immd {

namespace {

struct Parser {
  const char* p;
  const char* end;
  const char* base;

  Error err(const std::string& what) const {
    return make_err(Err::Protocol, "json: " + what + " at offset " + std::to_string(p - base));
  }
  void skip_ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  bool eat(char c) {
    if (p < end && *p == c) {
      ++p;
      return true;
    }
    return false;
  }

  Expected<Json> value(int depth) {
    if (depth > Json::kMaxDepth) return err("depth limit");
    skip_ws();
    if (p >= end) return err("unexpected end");
    char c = *p;
    if (c == '{') return object(depth);
    if (c == '[') return array(depth);
    if (c == '"') {
      std::string s;
      Error e = string(&s);
      if (e) return e;
      return Json::str(std::move(s));
    }
    if (c == 't') return literal("true", Json::boolean(true));
    if (c == 'f') return literal("false", Json::boolean(false));
    if (c == 'n') return literal("null", Json());
    if (c == '-' || (c >= '0' && c <= '9')) return number();
    return err(std::string("unexpected char '") + c + "'");
  }

  Expected<Json> literal(const char* lit, Json v) {
    size_t n = std::strlen(lit);
    if (size_t(end - p) < n || std::memcmp(p, lit, n) != 0) return err("bad literal");
    p += n;
    return v;
  }

  Expected<Json> number() {
    // strtod 依赖 NUL 结尾：parse() 入口从 std::string 取 c_str()，end 即 NUL 处，安全。
    char* numend = nullptr;
    double v = std::strtod(p, &numend);
    if (numend == p || numend > end) return err("bad number");
    if (!std::isfinite(v)) return err("number overflow");
    p = numend;
    return Json::number(v);
  }

  Error string(std::string* out) {
    if (!eat('"')) return err("expected string");
    while (p < end) {
      unsigned char c = static_cast<unsigned char>(*p);
      if (c == '"') {
        ++p;
        return {};
      }
      if (c < 0x20) return err("raw control char in string");
      if (c == '\\') {
        ++p;
        if (p >= end) return err("bad escape");
        char e = *p++;
        switch (e) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (Error er = hex4(&cp)) return er;
            if (cp >= 0xD800 && cp <= 0xDBFF) {  // 代理对
              if (!(p + 1 < end && p[0] == '\\' && p[1] == 'u')) return err("lone high surrogate");
              p += 2;
              uint32_t lo = 0;
              if (Error er = hex4(&lo)) return er;
              if (lo < 0xDC00 || lo > 0xDFFF) return err("bad low surrogate");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              return err("lone low surrogate");
            }
            append_utf8(out, cp);
            break;
          }
          default: return err("bad escape char");
        }
      } else {
        out->push_back(static_cast<char>(c));
        ++p;
      }
    }
    return err("unterminated string");
  }

  Error hex4(uint32_t* out) {
    if (end - p < 4) return err("bad \\u");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = p[i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return err("bad hex");
    }
    p += 4;
    *out = v;
    return {};
  }

  static void append_utf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
      out->push_back(char(cp));
    } else if (cp < 0x800) {
      out->push_back(char(0xC0 | (cp >> 6)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(char(0xE0 | (cp >> 12)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(char(0xF0 | (cp >> 18)));
      out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    }
  }

  Expected<Json> object(int depth) {
    eat('{');
    Json j = Json::object();
    skip_ws();
    if (eat('}')) return j;
    while (true) {
      skip_ws();
      std::string key;
      if (Error e = string(&key)) return e;
      skip_ws();
      if (!eat(':')) return err("expected ':'");
      auto v = value(depth + 1);
      if (!v.ok()) return v.error();
      j.set(key, std::move(v.value()));
      skip_ws();
      if (eat(',')) continue;
      if (eat('}')) return j;
      return err("expected ',' or '}'");
    }
  }

  Expected<Json> array(int depth) {
    eat('[');
    Json j = Json::array();
    skip_ws();
    if (eat(']')) return j;
    while (true) {
      auto v = value(depth + 1);
      if (!v.ok()) return v.error();
      j.push(std::move(v.value()));
      skip_ws();
      if (eat(',')) continue;
      if (eat(']')) return j;
      return err("expected ',' or ']'");
    }
  }
};

void dump_string(const std::string& s, std::string* out) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

void dump_value(const Json& j, std::string* out) {
  switch (j.type()) {
    case Json::Type::Null: *out += "null"; break;
    case Json::Type::Bool: *out += j.bval() ? "true" : "false"; break;
    case Json::Type::Num: {
      double v = j.num();
      if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 9.007199254740992e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        *out += buf;
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        *out += buf;
      }
      break;
    }
    case Json::Type::Str: dump_string(j.sval(), out); break;
    case Json::Type::Arr: {
      out->push_back('[');
      bool first = true;
      for (auto& v : j.arr()) {
        if (!first) out->push_back(',');
        first = false;
        dump_value(v, out);
      }
      out->push_back(']');
      break;
    }
    case Json::Type::Obj: {
      out->push_back('{');
      bool first = true;
      for (auto& kv : j.obj()) {
        if (!first) out->push_back(',');
        first = false;
        dump_string(kv.first, out);
        out->push_back(':');
        dump_value(kv.second, out);
      }
      out->push_back('}');
      break;
    }
  }
}

}  // namespace

std::string Json::dump() const {
  std::string out;
  dump_value(*this, &out);
  return out;
}

Expected<Json> Json::parse(const std::string& text) {
  if (text.size() > kMaxLen) return make_err(Err::Protocol, "json: input too large");
  Parser ps{text.c_str(), text.c_str() + text.size(), text.c_str()};
  auto v = ps.value(0);
  if (!v.ok()) return v.error();
  ps.skip_ws();
  if (ps.p != ps.end) return ps.err("trailing garbage");
  return v;
}

}  // namespace immd


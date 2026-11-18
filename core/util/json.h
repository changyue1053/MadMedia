// 防御式最小 JSON：协议层唯一的不可信文本输入面（架构 §11 parser 纪律）。
// 硬限制：输入 ≤4MB、嵌套 ≤32 层；任何畸形输入返回 Err::Protocol，绝不崩溃（fuzz-lite 保障）。
// 数字以 double 存储，整数序列化保持无小数点形态（size 等字段 ≤2^53 安全）。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/util/error.h"

namespace immd {

class Json {
 public:
  enum class Type { Null, Bool, Num, Str, Arr, Obj };

  Json() = default;
  static Json boolean(bool b) {
    Json j;
    j.type_ = Type::Bool;
    j.bool_ = b;
    return j;
  }
  static Json number(double v) {
    Json j;
    j.type_ = Type::Num;
    j.num_ = v;
    return j;
  }
  static Json str(std::string s) {
    Json j;
    j.type_ = Type::Str;
    j.str_ = std::move(s);
    return j;
  }
  static Json array() {
    Json j;
    j.type_ = Type::Arr;
    return j;
  }
  static Json object() {
    Json j;
    j.type_ = Type::Obj;
    return j;
  }

  Type type() const { return type_; }
  bool is_obj() const { return type_ == Type::Obj; }
  bool is_arr() const { return type_ == Type::Arr; }
  bool is_str() const { return type_ == Type::Str; }
  bool is_num() const { return type_ == Type::Num; }

  // ---- 构造器（链式）----
  Json& set(const std::string& k, Json v) {
    obj_.emplace_back(k, std::move(v));
    return *this;
  }
  Json& push(Json v) {
    arr_.push_back(std::move(v));
    return *this;
  }

  // ---- 读取（对象字段，缺失给默认值——契约字段缺失由调用方决定是否为错）----
  const Json* find(const std::string& k) const {
    if (type_ != Type::Obj) return nullptr;
    for (auto& kv : obj_)
      if (kv.first == k) return &kv.second;
    return nullptr;
  }
  bool has(const std::string& k) const { return find(k) != nullptr; }
  std::string get_str(const std::string& k, const std::string& def = "") const {
    auto* v = find(k);
    return (v && v->type_ == Type::Str) ? v->str_ : def;
  }
  int64_t get_i64(const std::string& k, int64_t def = 0) const {
    auto* v = find(k);
    return (v && v->type_ == Type::Num) ? static_cast<int64_t>(v->num_) : def;
  }
  bool get_bool(const std::string& k, bool def = false) const {
    auto* v = find(k);
    return (v && v->type_ == Type::Bool) ? v->bool_ : def;
  }

  double num() const { return num_; }
  int64_t i64() const { return static_cast<int64_t>(num_); }
  bool bval() const { return bool_; }
  const std::string& sval() const { return str_; }
  const std::vector<Json>& arr() const { return arr_; }
  const std::vector<std::pair<std::string, Json>>& obj() const { return obj_; }

  std::string dump() const;
  static Expected<Json> parse(const std::string& text);

  static constexpr size_t kMaxLen = 4u << 20;
  static constexpr int kMaxDepth = 32;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Json> arr_;
  std::vector<std::pair<std::string, Json>> obj_;
};

}  // namespace immd


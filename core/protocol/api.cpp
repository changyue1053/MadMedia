#include "core/protocol/api.h"

#include "core/util/json.h"

namespace immd {

namespace {
Expected<std::vector<uint32_t>> parse_u32_array(const Json* arr, const char* what) {
  if (!arr || !arr->is_arr()) return make_err(Err::Protocol, std::string("missing array: ") + what);
  std::vector<uint32_t> out;
  for (auto& v : arr->arr()) {
    if (!v.is_num() || v.num() < 0) return make_err(Err::Protocol, std::string("bad element in ") + what);
    out.push_back(static_cast<uint32_t>(v.i64()));
  }
  return out;
}
}  // namespace

std::string build_init(const TaskRecord& r, const Config& cfg, int64_t suggest_part_size) {
  Json j = Json::object();
  j.set("client_task_id", Json::str(r.task_id))
      .set("tenant_id", Json::str(cfg.tenant_id))
      .set("kind", Json::str(kind_name(r.kind)))
      .set("mime", Json::str(r.mime))
      .set("size", Json::number(double(r.size)))
      .set("sha256", Json::str(r.sha256_hex))
      .set("quick_hash", Json::str(r.quick_hash_hex))
      .set("suggest_part_size", Json::number(double(suggest_part_size)));
  if (r.streaming) j.set("streaming", Json::boolean(true));  // 流式：无 hash/size 开单（架构 §14 语音）
  return j.dump();
}

std::string build_complete_streaming(const std::string& upload_id, const std::string& sha256_hex,
                                     int64_t size, uint32_t nparts) {
  Json j = Json::object();
  j.set("upload_id", Json::str(upload_id))
      .set("sha256", Json::str(sha256_hex))
      .set("size", Json::number(double(size)))
      .set("nparts", Json::number(double(nparts)));
  return j.dump();
}

Expected<InitAnswer> parse_init(const std::string& body) {
  auto p = Json::parse(body);
  if (!p.ok()) return p.error();
  auto& j = p.value();
  InitAnswer a;
  std::string result = j.get_str("result");
  if (result == "instant") {
    a.kind = InitAnswer::Kind::Instant;
    a.media_id = j.get_str("media_id");
    if (a.media_id.empty()) return make_err(Err::Protocol, "instant without media_id");
    return a;
  }
  if (result == "challenge") {
    a.kind = InitAnswer::Kind::Challenge;
    a.challenge_id = j.get_str("challenge_id");
    a.nonce = j.get_str("nonce");
    const Json* rs = j.find("ranges");
    if (a.challenge_id.empty() || a.nonce.empty() || !rs || !rs->is_arr())
      return make_err(Err::Protocol, "bad challenge answer");
    for (auto& r : rs->arr()) {
      if (!r.is_arr() || r.arr().size() != 2) return make_err(Err::Protocol, "bad challenge range");
      a.ranges.emplace_back(r.arr()[0].i64(), r.arr()[1].i64());
    }
    return a;
  }
  if (result == "upload") {
    a.kind = InitAnswer::Kind::Upload;
    a.upload_id = j.get_str("upload_id");
    a.part_size = j.get_i64("part_size");
    a.nparts = static_cast<uint32_t>(j.get_i64("nparts"));
    // nparts == 0 合法：流式开单（语音）时总片数未知，complete 补报
    if (a.upload_id.empty() || a.part_size <= 0)
      return make_err(Err::Protocol, "bad upload answer");
    auto rec = parse_u32_array(j.find("received_parts"), "received_parts");
    if (!rec.ok()) return rec.error();
    a.received = std::move(rec.value());
    return a;
  }
  return make_err(Err::Protocol, "unknown init result: " + result);
}

std::string build_part_urls(const std::string& upload_id, const std::vector<uint32_t>& parts) {
  Json j = Json::object();
  j.set("upload_id", Json::str(upload_id));
  Json arr = Json::array();
  for (auto p : parts) arr.push(Json::number(p));
  j.set("parts", std::move(arr));
  return j.dump();
}

Expected<std::map<uint32_t, std::string>> parse_part_urls(const std::string& body) {
  auto p = Json::parse(body);
  if (!p.ok()) return p.error();
  const Json* urls = p.value().find("urls");
  if (!urls || !urls->is_obj()) return make_err(Err::Protocol, "missing urls");
  std::map<uint32_t, std::string> out;
  for (auto& kv : urls->obj()) {
    if (!kv.second.is_str() || kv.second.sval().empty())
      return make_err(Err::Protocol, "bad url entry");
    out[static_cast<uint32_t>(std::stoul(kv.first))] = kv.second.sval();
  }
  return out;
}

std::string build_complete(const std::string& upload_id, const std::string& sha256_hex) {
  Json j = Json::object();
  j.set("upload_id", Json::str(upload_id)).set("sha256", Json::str(sha256_hex));
  return j.dump();
}

Expected<CompleteAnswer> parse_complete_ok(const std::string& body) {
  auto p = Json::parse(body);
  if (!p.ok()) return p.error();
  CompleteAnswer a;
  a.media_id = p.value().get_str("media_id");
  if (a.media_id.empty()) return make_err(Err::Protocol, "complete without media_id");
  return a;
}

Expected<CompleteAnswer> parse_complete_missing(const std::string& body) {
  auto p = Json::parse(body);
  if (!p.ok()) return p.error();
  CompleteAnswer a;
  auto miss = parse_u32_array(p.value().find("missing"), "missing");
  if (!miss.ok()) return miss.error();
  a.missing = std::move(miss.value());
  return a;
}

std::string build_challenge(const std::string& challenge_id, const std::string& proof_hex) {
  Json j = Json::object();
  j.set("challenge_id", Json::str(challenge_id)).set("proof", Json::str(proof_hex));
  return j.dump();
}

std::string build_dl(const std::string& media_id) {
  Json j = Json::object();
  j.set("media_id", Json::str(media_id));
  return j.dump();
}

Expected<DlAnswer> parse_dl(const std::string& body) {
  auto p = Json::parse(body);
  if (!p.ok()) return p.error();
  DlAnswer a;
  a.url = p.value().get_str("url");
  a.size = p.value().get_i64("size");
  a.sha256_hex = p.value().get_str("sha256");
  if (a.url.empty() || a.size <= 0) return make_err(Err::Protocol, "bad dl answer");
  return a;
}

}  // namespace immd


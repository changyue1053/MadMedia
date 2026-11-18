#include "core/parser/imagemeta.h"

#include <cstring>

namespace immd {

namespace {

bool starts_with(const uint8_t* d, size_t len, const char* magic, size_t n) {
  return len >= n && std::memcmp(d, magic, n) == 0;
}

bool fourcc_at(const uint8_t* d, size_t len, size_t off, const char* cc) {
  return len >= off + 4 && std::memcmp(d + off, cc, 4) == 0;
}

uint16_t be16(const uint8_t* d) { return uint16_t((d[0] << 8) | d[1]); }
uint32_t be32(const uint8_t* d) {
  return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
}
uint16_t le16(const uint8_t* d) { return uint16_t(d[0] | (d[1] << 8)); }
uint32_t le32(const uint8_t* d) {
  return uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
}

// 跳过 UTF-8 BOM 与空白后做文本前缀匹配（SVG/HTML 嗅探用，大小写不敏感）
bool text_prefix(const uint8_t* d, size_t len, const char* prefix) {
  size_t i = 0;
  if (len >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) i = 3;
  while (i < len && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n')) ++i;
  size_t n = std::strlen(prefix);
  if (len - i < n) return false;
  for (size_t k = 0; k < n; ++k) {
    char c = char(d[i + k]);
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != prefix[k]) return false;
  }
  return true;
}

// ---------------- EXIF（TIFF）：只找 orientation 与 GPS 指针 ----------------
void parse_tiff(const uint8_t* t, size_t len, ImageHead* out) {
  if (len < 8) return;
  bool le;
  if (t[0] == 'I' && t[1] == 'I') le = true;
  else if (t[0] == 'M' && t[1] == 'M') le = false;
  else return;
  auto rd16 = [&](size_t o) { return le ? le16(t + o) : be16(t + o); };
  auto rd32 = [&](size_t o) { return le ? le32(t + o) : be32(t + o); };
  if (rd16(2) != 42) return;
  uint32_t ifd = rd32(4);
  if (ifd + 2 > len) return;
  uint32_t count = rd16(ifd);
  if (count > 512) return;  // IFD 条目上限（fuzz CPU/越界防线）
  for (uint32_t i = 0; i < count; ++i) {
    size_t e = ifd + 2 + size_t(i) * 12;
    if (e + 12 > len) return;
    uint16_t tag = rd16(e);
    uint16_t type = rd16(e + 2);
    if (tag == 0x0112 && type == 3) {  // Orientation, SHORT
      int v = rd16(e + 8);
      if (v >= 1 && v <= 8) out->orientation = v;
    } else if (tag == 0x8825) {  // GPS IFD 指针：存在即视为携带定位（隐私面从严）
      out->has_gps = true;
    }
  }
}

// ---------------- JPEG 段遍历 ----------------
Error parse_jpeg(const uint8_t* d, size_t len, ImageHead* out) {
  size_t off = 2;  // 跳过 SOI
  int segments = 0;
  while (off + 4 <= len) {
    if (++segments > 1024) return make_err(Err::Protocol, "jpeg: too many segments");
    if (d[off] != 0xFF) return make_err(Err::Protocol, "jpeg: bad marker alignment");
    uint8_t m = d[off + 1];
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {  // 独立标记
      off += 2;
      continue;
    }
    uint16_t seglen = be16(d + off + 2);
    if (seglen < 2 || off + 2 + seglen > len) return make_err(Err::Protocol, "jpeg: segment overflow");
    const uint8_t* body = d + off + 4;
    size_t body_len = size_t(seglen) - 2;
    bool is_sof = (m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC;
    if (is_sof && body_len >= 5) {
      out->height = be16(body + 1);
      out->width = be16(body + 3);
      return {};  // SOF 后信息已齐（方向与 GPS 在其前的 APP1）
    }
    if (m == 0xE1 && body_len > 6 && std::memcmp(body, "Exif\0\0", 6) == 0) {
      parse_tiff(body + 6, body_len - 6, out);
    }
    if (m == 0xDA) return make_err(Err::Protocol, "jpeg: SOS before SOF");
    off += 2 + seglen;
  }
  return make_err(Err::Protocol, "jpeg: no SOF");
}

}  // namespace

std::string sniff_mime(const uint8_t* d, size_t len) {
  if (len < 4) return "application/octet-stream";
  if (starts_with(d, len, "\xFF\xD8\xFF", 3)) return "image/jpeg";
  if (starts_with(d, len, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
  if (starts_with(d, len, "GIF87a", 6) || starts_with(d, len, "GIF89a", 6)) return "image/gif";
  if (starts_with(d, len, "RIFF", 4) && fourcc_at(d, len, 8, "WEBP")) return "image/webp";
  if (starts_with(d, len, "BM", 2)) return "image/bmp";
  if (fourcc_at(d, len, 4, "ftyp")) {  // ISO-BMFF：按 major brand 细分
    if (len >= 12) {
      std::string brand(reinterpret_cast<const char*>(d + 8), 4);
      if (brand == "heic" || brand == "heix" || brand == "mif1" || brand == "msf1")
        return "image/heic";
      if (brand == "avif") return "image/avif";
      return "video/mp4";
    }
  }
  if (starts_with(d, len, "%PDF", 4)) return "application/pdf";
  if (starts_with(d, len, "PK\x03\x04", 4)) return "application/zip";
  if (starts_with(d, len, "\x1f\x8b", 2)) return "application/gzip";
  if (starts_with(d, len, "MZ", 2)) return "application/x-msdownload";  // PE：黑名单默认告警（§14）
  if (starts_with(d, len, "\x7f" "ELF", 4)) return "application/x-executable";
  if (starts_with(d, len, "OggS", 4)) return "audio/ogg";
  if (starts_with(d, len, "ID3", 3)) return "audio/mpeg";
  // 文本类高危：SVG/HTML 是存储型 XSS 面（独立域 + attachment，架构 §14）
  if (text_prefix(d, len, "<svg") || (text_prefix(d, len, "<?xml") && len < (1u << 20))) {
    // <?xml 可能是任意 XML；粗探 svg 根元素
    for (size_t i = 0; i + 4 <= len && i < 4096; ++i)
      if (d[i] == '<' && i + 4 <= len && std::memcmp(d + i, "<svg", 4) == 0) return "image/svg+xml";
    if (text_prefix(d, len, "<svg")) return "image/svg+xml";
    return "application/xml";
  }
  if (text_prefix(d, len, "<!doctype") || text_prefix(d, len, "<html")) return "text/html";
  return "application/octet-stream";
}

Expected<ImageHead> parse_image_head(const uint8_t* d, size_t len, int64_t max_pixels) {
  ImageHead h;
  h.mime = sniff_mime(d, len);
  if (h.mime == "image/jpeg") {
    h.kind = ImageKind::Jpeg;
    if (Error e = parse_jpeg(d, len, &h)) return e;
  } else if (h.mime == "image/png") {
    h.kind = ImageKind::Png;
    // 签名(8) + IHDR 长度(4) + "IHDR"(4) + w(4) h(4)
    if (len < 24 || !fourcc_at(d, len, 12, "IHDR")) return make_err(Err::Protocol, "png: bad IHDR");
    h.width = be32(d + 16);
    h.height = be32(d + 20);
  } else if (h.mime == "image/gif") {
    h.kind = ImageKind::Gif;
    if (len < 10) return make_err(Err::Protocol, "gif: too short");
    h.width = le16(d + 6);
    h.height = le16(d + 8);
  } else if (h.mime == "image/webp") {
    h.kind = ImageKind::WebP;
    // VP8X 扩展头带画布尺寸；VP8/VP8L 简单头不解析（dims 置 0，交解码器）
    if (fourcc_at(d, len, 12, "VP8X") && len >= 30) {
      h.width = 1 + (uint32_t(d[24]) | (uint32_t(d[25]) << 8) | (uint32_t(d[26]) << 16));
      h.height = 1 + (uint32_t(d[27]) | (uint32_t(d[28]) << 8) | (uint32_t(d[29]) << 16));
    }
  } else if (h.mime == "image/heic" || h.mime == "image/avif") {
    h.kind = ImageKind::Heic;
    // best-effort：定位 ispe（体积 20 字节定长：size+fourcc+ver/flags+w+h）
    for (size_t i = 4; i + 16 <= len; ++i) {
      if (d[i] == 'i' && d[i + 1] == 's' && d[i + 2] == 'p' && d[i + 3] == 'e' && i >= 4 &&
          be32(d + i - 4) == 20) {
        h.width = be32(d + i + 8);
        h.height = be32(d + i + 12);
        break;
      }
    }
  } else {
    return make_err(Err::Protocol, "not an image: " + h.mime);
  }
  if (int64_t(h.width) * int64_t(h.height) > max_pixels) h.bomb = true;
  return h;
}

Expected<std::vector<uint8_t>> jpeg_strip_metadata(const uint8_t* d, size_t len) {
  if (!starts_with(d, len, "\xFF\xD8\xFF", 3)) return make_err(Err::Protocol, "not jpeg");
  // Start with the fixed SOI bytes before reserving capacity. This also avoids
  // GCC 14 -O3 misdiagnosing push_back after reserve as free-nonheap-object.
  std::vector<uint8_t> out{0xFF, 0xD8};
  out.reserve(len);
  size_t off = 2;
  int segments = 0;
  while (off + 4 <= len) {
    if (++segments > 1024) return make_err(Err::Protocol, "jpeg: too many segments");
    if (d[off] != 0xFF) return make_err(Err::Protocol, "jpeg: bad marker alignment");
    uint8_t m = d[off + 1];
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      out.push_back(0xFF);
      out.push_back(m);
      off += 2;
      continue;
    }
    uint16_t seglen = be16(d + off + 2);
    if (seglen < 2 || off + 2 + seglen > len) return make_err(Err::Protocol, "jpeg: segment overflow");
    bool strip = (m >= 0xE1 && m <= 0xEF) || m == 0xFE;  // APP1..APP15 + COM；APP0(JFIF) 保留
    if (!strip) out.insert(out.end(), d + off, d + off + 2 + seglen);
    if (m == 0xDA) {  // SOS：其后为熵数据直至 EOI，原样拷贝，结束
      out.insert(out.end(), d + off + 2 + seglen, d + len);
      return out;
    }
    off += 2 + seglen;
  }
  return make_err(Err::Protocol, "jpeg: no SOS");
}

}  // namespace immd


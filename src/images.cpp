#include "semop/images.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>

#include "semop/provider.hpp"
#include "util.hpp"

namespace semop {

namespace {

using Bytes = std::vector<std::uint8_t>;

Error bad(std::string message) { return detail::invalid(ErrorKind::invalid_argument, std::move(message)); }

std::uint32_t be32(const Bytes& b, std::size_t at) {
  return (std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at + 1]) << 16) | (std::uint32_t(b[at + 2]) << 8) | b[at + 3];
}
std::uint16_t be16(const Bytes& b, std::size_t at) { return std::uint16_t((b[at] << 8) | b[at + 1]); }

std::string mib(std::size_t bytes) {
  char text[32];
  std::snprintf(text, sizeof text, "%.1f MiB", double(bytes) / (1024.0 * 1024.0));
  return text;
}

std::string new_placeholder() {
  static thread_local std::mt19937_64 rng{std::random_device{}() ^ (std::uint64_t(std::random_device{}()) << 32)};
  std::uint8_t u[16];
  for (int i = 0; i < 16; i += 8) {
    const std::uint64_t r = rng();
    for (int j = 0; j < 8; ++j) u[i + j] = std::uint8_t(r >> (8 * j));
  }
  u[6] = std::uint8_t((u[6] & 0x0F) | 0x40);  // version 4
  u[8] = std::uint8_t((u[8] & 0x3F) | 0x80);  // RFC 4122 variant
  static const char* hex = "0123456789abcdef";
  std::string text = "image-";
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
    text += hex[u[i] >> 4];
    text += hex[u[i] & 15];
  }
  return text;
}

// PNG: IHDR size; reject animated PNG (an acTL chunk before the image data).
std::optional<Error> read_png(const Bytes& b, int& width, int& height) {
  if (b.size() < 33 || b[12] != 'I' || b[13] != 'H' || b[14] != 'D' || b[15] != 'R') return bad("malformed PNG: no IHDR header");
  width = int(std::min<std::uint32_t>(be32(b, 16), 1u << 30));
  height = int(std::min<std::uint32_t>(be32(b, 20), 1u << 30));
  std::size_t at = 8;
  while (at + 8 <= b.size()) {
    const std::uint32_t length = be32(b, at);
    const std::string kind(b.begin() + std::ptrdiff_t(at + 4), b.begin() + std::ptrdiff_t(at + 8));
    if (kind == "acTL") return bad("animated PNGs are not accepted; send a still frame");
    if (kind == "IDAT" || kind == "IEND") break;
    at += 12 + std::size_t(length);
  }
  return std::nullopt;
}

std::optional<int> exif_orientation(const Bytes& b, std::size_t start, std::size_t end) {
  // `start` is the TIFF header inside an APP1 "Exif\0\0" segment.
  if (end < start + 8 || end > b.size()) return std::nullopt;
  const bool little = b[start] == 'I' && b[start + 1] == 'I';
  if (!little && !(b[start] == 'M' && b[start + 1] == 'M')) return std::nullopt;
  const auto u16 = [&](std::size_t at) -> std::uint32_t {
    return little ? std::uint32_t(b[at] | (b[at + 1] << 8)) : std::uint32_t((b[at] << 8) | b[at + 1]);
  };
  const auto u32 = [&](std::size_t at) -> std::uint32_t {
    return little ? (u16(at) | (u16(at + 2) << 16)) : ((u16(at) << 16) | u16(at + 2));
  };
  const std::size_t ifd = start + u32(start + 4);
  if (ifd + 2 > end) return std::nullopt;
  const std::uint32_t count = u16(ifd);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t entry = ifd + 2 + 12 * std::size_t(i);
    if (entry + 12 > end) return std::nullopt;
    if (u16(entry) == 0x0112) return int(u16(entry + 8));
  }
  return std::nullopt;
}

std::optional<Error> read_jpeg(const Bytes& b, int& width, int& height, std::optional<int>& orientation, bool& progressive) {
  std::size_t at = 2;
  while (at + 4 <= b.size()) {
    if (b[at] != 0xFF) return bad("malformed JPEG: bad marker");
    const std::uint8_t marker = b[at + 1];
    if (marker == 0xFF) {
      ++at;
      continue;
    }
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      at += 2;
      continue;
    }
    const std::size_t length = be16(b, at + 2);
    const std::size_t body = at + 4;
    const std::size_t end = std::min(b.size(), at + 2 + length);
    if (marker == 0xE1 && end >= body + 6 && std::equal(b.begin() + std::ptrdiff_t(body), b.begin() + std::ptrdiff_t(body + 6), "Exif\0\0")) {
      const auto o = exif_orientation(b, body + 6, end);
      if (o && *o != 1) orientation = o;
    }
    const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (sof) {
      if (end < body + 5) break;
      height = be16(b, body + 1);
      width = be16(b, body + 3);
      progressive = marker != 0xC0 && marker != 0xC1;
      return std::nullopt;
    }
    if (marker == 0xDA) break;
    at += 2 + length;
  }
  return bad("malformed JPEG: no frame header with its size");
}

int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool is_placeholder_at(const std::string& s, std::size_t at) {
  // image-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
  static const char* shape = "image-hhhhhhhh-hhhh-hhhh-hhhh-hhhhhhhhhhhh";
  constexpr std::size_t n = 42;
  if (at + n > s.size()) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const char c = s[at + i];
    if (shape[i] == 'h' ? !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) : c != shape[i]) return false;
  }
  return true;
}

void scan(const std::string& text, std::vector<std::string>& out) {
  for (std::size_t at = text.find("image-"); at != std::string::npos; at = text.find("image-", at + 1)) {
    if (is_placeholder_at(text, at)) out.push_back(text.substr(at, 42));
  }
}

void scan(const Json& value, std::vector<std::string>& out) {
  if (value.is_string()) {
    scan(value.get_ref<const std::string&>(), out);
  } else if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      scan(it.key(), out);
      scan(it.value(), out);
    }
  } else if (value.is_array()) {
    for (const auto& item : value) scan(item, out);
  }
}

}  // namespace

std::string_view Image::format_name() const noexcept { return format() == ImageFormat::png ? "png" : "jpeg"; }

Result<Image> Image::from_bytes(Bytes bytes) {
  namespace lim = image_limits;
  if (bytes.size() > lim::max_file_bytes) return bad("image is " + mib(bytes.size()) + "; the limit is 16 MiB per file");
  Data data;
  static const std::uint8_t png_sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (bytes.size() >= 8 && std::equal(std::begin(png_sig), std::end(png_sig), bytes.begin())) {
    data.format = ImageFormat::png;
    if (auto e = read_png(bytes, data.width, data.height)) return *e;
  } else if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
    data.format = ImageFormat::jpeg;
    if (auto e = read_jpeg(bytes, data.width, data.height, data.exif_orientation, data.progressive)) return *e;
  } else {
    return bad("image must be PNG or JPEG (unrecognized file signature)");
  }
  for (const auto& [side, n] : {std::pair<const char*, int>{"width", data.width}, {"height", data.height}}) {
    if (n < lim::min_side || n > lim::max_side) {
      return bad(std::string("image ") + side + " is " + std::to_string(n) + " px; each side must be 256-1568 px " +
                 "(images are rejected, not resized: resize it first)");
    }
  }
  const std::int64_t area = std::int64_t(data.width) * data.height;
  if (area < lim::min_area || area > lim::max_area) {
    return bad("image area is " + std::to_string(area) + " px; it must be 78400-2458624 px");
  }
  data.bytes = std::move(bytes);
  data.placeholder = new_placeholder();
  return Image(std::make_shared<const Data>(std::move(data)));
}

Result<Image> Image::from_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return bad("can't read image '" + path + "'");
  Bytes bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad()) return bad("can't read image '" + path + "'");
  return from_bytes(std::move(bytes));
}

Result<Image> Image::from_base64(std::string_view text) {
  if (text.substr(0, 5) == "data:") return bad("base64 image must not have a data: URL prefix");
  Bytes bytes;
  std::uint32_t buffer = 0;
  int bits = 0;
  std::size_t chars = 0;
  std::size_t padding = 0;
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    ++chars;
    if (c == '=') {
      ++padding;
      continue;
    }
    const int v = base64_value(c);
    if (v < 0 || padding > 0) return bad("invalid base64 image data");
    buffer = (buffer << 6) | std::uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(std::uint8_t(buffer >> bits));
    }
  }
  if (chars % 4 != 0 || padding > 2) return bad("invalid base64 image data (bad length or padding)");
  return from_bytes(std::move(bytes));
}

std::string Image::base64() const {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto& b = bytes();
  std::string out;
  out.reserve((b.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < b.size(); i += 3) {
    const std::uint32_t n = (std::uint32_t(b[i]) << 16) | (i + 1 < b.size() ? std::uint32_t(b[i + 1]) << 8 : 0) |
                            (i + 2 < b.size() ? b[i + 2] : 0);
    out += table[(n >> 18) & 63];
    out += table[(n >> 12) & 63];
    out += i + 1 < b.size() ? table[(n >> 6) & 63] : '=';
    out += i + 2 < b.size() ? table[n & 63] : '=';
  }
  return out;
}

Result<Json> frames(const std::vector<Image>& images, const std::vector<double>& seconds_ago) {
  if (images.empty()) return bad("frames needs at least one image");
  if (!seconds_ago.empty() && seconds_ago.size() != images.size()) {
    return bad("got " + std::to_string(images.size()) + " frames but " + std::to_string(seconds_ago.size()) +
               " seconds_ago values");
  }
  Json out = Json::array();
  for (std::size_t i = 0; i < images.size(); ++i) {
    Json item = Json::object();
    if (!seconds_ago.empty()) item["seconds_ago"] = seconds_ago[i];
    item["frame"] = images[i].placeholder();
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<std::string> image_placeholders(const Json& value) {
  std::vector<std::string> out;
  scan(value, out);
  return out;
}

std::vector<std::string> image_placeholders(const Questions& questions) {
  std::vector<std::string> out;
  for (const auto& [name, question] : questions) {
    std::visit([&](const auto& q) { scan(q.instructions, out); }, question);
    if (const auto* b = std::get_if<Boolean>(&question)) {
      if (b->yes) scan(*b->yes, out);
      if (b->no) scan(*b->no, out);
    } else if (const auto* c = std::get_if<Choice>(&question)) {
      for (const auto& option : c->options) {
        scan(option.name, out);
        if (option.description) scan(*option.description, out);
      }
    } else if (const auto* s = std::get_if<Score>(&question)) {
      for (const auto& level : s->levels) scan(level, out);
    }
  }
  return out;
}

Result<Located> locate(Provider& provider, const Json& state, const std::string& target, const LocateOptions& options) {
  if (detail::blank(target)) return bad("target must be a non-empty description");
  const Questions questions{
      {"present", Boolean{"Is there " + target + " in " + options.where + "?", {}, {}, options.min_presence}},
      {"box", Box{target + " in " + options.where, options.min_box_confidence}},
  };
  auto answers = ask(provider, state, questions);
  if (!answers) return answers.error();
  Located located{std::nullopt, std::nullopt, answers->at("present"), answers->at("box")};
  located.found = located.presence.boolean();
  if (located.found == true) located.region = located.box.region();
  return located;
}

}  // namespace semop

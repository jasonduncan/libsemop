#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "semop/answers.hpp"
#include "semop/questions.hpp"
#include "semop/result.hpp"

namespace semop {

// Images: pictures as part of what a question is asked about. TypeSafe calls this
// "Dreamer-ready" (not production-ready): formats, limits and accuracy may change.
//
// An Image is checked when it's made, so a bad file fails with a clear invalid_argument error
// instead of failing the whole call. TypeSafe's current rules:
//   - PNG or JPEG stills only (no animated PNG), at most 16 MiB per file;
//   - each side 256-1568 px, total area 78,400-2,458,624 px;
//   - rejected, not resized; EXIF orientation is not applied (rotate the pixels yourself);
//   - at most 32 MiB of images per request, counting every use of an image.
//
// Put `image.placeholder()` anywhere text goes: as a state value (`{"photo", img.placeholder()}`),
// or inside instructions or option/level descriptions. Give the images to the provider (for
// TypeSafe, `TypeSafe::attach`); each request sends the attached images whose placeholders it uses.

namespace image_limits {
inline constexpr int min_side = 256;
inline constexpr int max_side = 1568;
inline constexpr std::int64_t min_area = 78'400;
inline constexpr std::int64_t max_area = 2'458'624;
inline constexpr std::size_t max_file_bytes = 16u * 1024 * 1024;
inline constexpr std::size_t max_request_bytes = 32u * 1024 * 1024;
}  // namespace image_limits

enum class ImageFormat { png, jpeg };

/// A checked PNG or JPEG image with a unique placeholder (`image-<uuid>`). Cheap to copy: copies
/// share the bytes and the placeholder.
class Image {
 public:
  /// Checks the bytes against the limits; an invalid_argument Error says what's wrong.
  [[nodiscard]] static Result<Image> from_bytes(std::vector<std::uint8_t> bytes);
  [[nodiscard]] static Result<Image> from_file(const std::string& path);
  /// Standard base64 (whitespace ignored, no `data:` prefix).
  [[nodiscard]] static Result<Image> from_base64(std::string_view text);

  [[nodiscard]] const std::string& placeholder() const noexcept { return data_->placeholder; }
  [[nodiscard]] ImageFormat format() const noexcept { return data_->format; }
  /// "png" or "jpeg".
  [[nodiscard]] std::string_view format_name() const noexcept;
  [[nodiscard]] int width() const noexcept { return data_->width; }
  [[nodiscard]] int height() const noexcept { return data_->height; }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return data_->bytes; }
  [[nodiscard]] std::size_t size() const noexcept { return data_->bytes.size(); }
  /// The bytes, shared with this image (no copy), e.g. for a multipart upload.
  [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> shared_bytes() const noexcept {
    return {data_, &data_->bytes};
  }
  /// Standard, padded base64, as TypeSafe's `data_base64` asset field wants it.
  [[nodiscard]] std::string base64() const;

  /// The JPEG's EXIF orientation, when it has one other than 1 (upright). TypeSafe ignores it,
  /// so the model sees the pixels unrotated: rotate them yourself.
  [[nodiscard]] std::optional<int> exif_orientation() const noexcept { return data_->exif_orientation; }
  /// A progressive (non-baseline) JPEG. TypeSafe recommends baseline.
  [[nodiscard]] bool progressive() const noexcept { return data_->progressive; }

 private:
  struct Data {
    std::vector<std::uint8_t> bytes;
    ImageFormat format = ImageFormat::png;
    int width = 0;
    int height = 0;
    std::string placeholder;
    std::optional<int> exif_orientation;
    bool progressive = false;
  };
  explicit Image(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
  std::shared_ptr<const Data> data_;
};

/// Still frames as a state value, oldest first: `[{"frame": "<placeholder>"}, ...]`. With
/// `seconds_ago` (one per frame, else invalid_argument) each item says how long ago it was taken.
/// Point at one frame in instructions with e.g. "`frames[2].frame`".
[[nodiscard]] Result<Json> frames(const std::vector<Image>& images, const std::vector<double>& seconds_ago = {});

/// Every image placeholder (`image-<uuid>`) in the JSON's strings and keys, once per use.
[[nodiscard]] std::vector<std::string> image_placeholders(const Json& value);
/// The same, in question texts (instructions, option descriptions, levels).
[[nodiscard]] std::vector<std::string> image_placeholders(const Questions& questions);

// ---------------------------------------------------------------------------
// locate: a Box with the presence check it needs
// ---------------------------------------------------------------------------

class Provider;

struct LocateOptions {
  /// How the questions name the image, e.g. "`photo`" when the state is `{"photo": ...}`.
  std::string where = "the image";
  /// The confidence the yes/no presence answer needs; below it `found` is nullopt.
  double min_presence = 0.5;
  /// With a low-confidence box, `found` is true but `region` is nullopt.
  double min_box_confidence = 0;
};

struct Located {
  /// true / false, or nullopt when the presence answer was undecided.
  std::optional<bool> found;
  /// Only when `found` is true.
  std::optional<Region> region;
  Answer presence;
  Answer box;
};

/// Is `target` (e.g. "a coffee mug") in the image, and where? Asks a presence Boolean and a Box
/// in one call, because Box has no "not found" answer. Use a model that supports Box (TypeSafe:
/// `jev-1.14.0`).
[[nodiscard]] Result<Located> locate(Provider& provider, const Json& state, const std::string& target,
                                     const LocateOptions& options = {});

}  // namespace semop

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include <libtypesafe/client.hpp>

#include "semop/images.hpp"
#include "semop/provider.hpp"

namespace semop {

/// TypeSafe's hosted API, which serves the Jev models. Target `libsemop::typesafe`.
///
/// Translation only: our questions become TypeSafe questions (sent exactly as the Python
/// library sends them), one `system_one` call, and each answer is built with `make_answer`.
/// You create and own the `libtypesafe::Client` (key, timeout, retries); this borrows it, so
/// the client must outlive the provider. Any client failure is a provider_error (`timeout` for
/// timeouts), prefixed "TypeSafe: ". Each answer's `call` records the model TypeSafe reports.
///
/// Retries belong to the client: libtypesafe retries by default, and
/// `ClientOptions::retry.max_retries = 0` turns that off.
///
/// Images: `attach` them, then use their placeholders in the state or any question text. Each
/// call sends the attached images it references in the request's `assets` (base64, since
/// libtypesafe sends JSON bodies), and every call with an image or a Box question carries
/// `TypeSafe-Danger-Zone-Images: true`. A placeholder that isn't attached, or images over 32 MiB
/// per request (counting each use), fail as invalid_argument before anything is sent. Box
/// questions need `jev-1.14.0` (or `jev-preview`).
class TypeSafe final : public Provider {
 public:
  explicit TypeSafe(libtypesafe::Client& client, std::string model = "jev-latest",
                    libtypesafe::CallOptions options = {});
  ~TypeSafe() override;

  /// libtypesafe's Client has no public wait, so this sleeps at most 1 ms.
  void wait(std::chrono::milliseconds max_wait) override;
  [[nodiscard]] const std::string& model() const noexcept { return model_; }

  /// Make `image` available to calls that use its placeholder. Attaching costs nothing per call:
  /// only referenced images are sent.
  void attach(const Image& image);
  void detach(const Image& image);
  void detach_all() noexcept { images_.clear(); }

 private:
  void start_call(Ticket ticket, const Json& state, const Questions& questions) override;
  void cancel_call(Ticket ticket) override;
  void on_pump() override;

  libtypesafe::Client& client_;
  std::string model_;
  libtypesafe::CallOptions options_;
  std::unordered_map<std::string, Image> images_;
  std::unordered_map<Ticket, libtypesafe::Pending<libtypesafe::SystemOneResult>> pending_;
};

/// The header every image or Box request must carry (with the value "true").
inline constexpr const char* typesafe_images_header = "TypeSafe-Danger-Zone-Images";

/// A question as TypeSafe's wire JSON, e.g. `{"type": "noul", "instructions": "..."}`.
[[nodiscard]] Json typesafe_question(const Question& question);

}  // namespace semop

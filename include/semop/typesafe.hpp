#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include <libtypesafe/client.hpp>

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
class TypeSafe final : public Provider {
 public:
  explicit TypeSafe(libtypesafe::Client& client, std::string model = "jev-latest",
                    libtypesafe::CallOptions options = {});
  ~TypeSafe() override;

  /// libtypesafe's Client has no public wait, so this sleeps at most 1 ms.
  void wait(std::chrono::milliseconds max_wait) override;
  [[nodiscard]] const std::string& model() const noexcept { return model_; }

 private:
  void start_call(Ticket ticket, const Json& state, const Questions& questions) override;
  void cancel_call(Ticket ticket) override;
  void on_pump() override;

  libtypesafe::Client& client_;
  std::string model_;
  libtypesafe::CallOptions options_;
  std::unordered_map<Ticket, libtypesafe::Pending<libtypesafe::SystemOneResult>> pending_;
};

/// A question as TypeSafe's wire JSON, e.g. `{"type": "noul", "instructions": "..."}`.
[[nodiscard]] Json typesafe_question(const Question& question);

}  // namespace semop

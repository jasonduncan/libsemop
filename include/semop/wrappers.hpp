#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "semop/provider.hpp"

namespace semop {

/// A provider with a time limit per call. A call that runs over is cancelled and comes back as
/// a `timeout` error. It is still a provider, so the limit carries through operators, flows,
/// filtering, and reranking (a timed-out item is simply unscored).
///
/// Stopping to wait isn't always stopping the work: a hosted call is cancelled locally, but the
/// server may still finish it, and bill it. Borrows `inner`, which must outlive this.
class TimeLimited final : public Provider {
 public:
  TimeLimited(Provider& inner, std::chrono::milliseconds limit);
  ~TimeLimited() override;
  void wait(std::chrono::milliseconds max_wait) override;

  /// Seam for tests.
  std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };

 private:
  struct Running {
    Ticket inner;
    std::chrono::steady_clock::time_point deadline;
  };
  void start_call(Ticket ticket, const Json& state, const Questions& questions) override;
  void cancel_call(Ticket ticket) override;
  void on_pump() override;

  Provider& inner_;
  std::chrono::milliseconds limit_;
  std::unordered_map<Ticket, Running> running_;
};

/// Escalation: ask the first provider everything, then re-ask only its unsure answers of the
/// next, all in one call, and so on down the list. The last provider's answer stands, and each
/// answer's `call` shows which provider gave it.
///
/// "Unsure" means undecided, or confidence under `escalate_below`. A question's
/// `min_confidence` still decides when the final answer is a "don't know". It escalates when a
/// model is unsure, never when it fails: an error from any provider is the result. Borrows the
/// providers, which must outlive this.
class Cascade final : public Provider {
 public:
  /// At least two providers, first (fastest or cheapest) to last; `escalate_below` in [0, 1].
  /// Invalid arguments make every call fail with `invalid_argument`.
  Cascade(std::vector<Provider*> providers, double escalate_below = 0.0);
  ~Cascade() override;
  void wait(std::chrono::milliseconds max_wait) override;

 private:
  struct Running {
    Json state;
    Questions questions;
    Answers answers;          // by name, merged across providers
    std::size_t provider = 0;  // index of the provider asked last
    Ticket inner = 0;
  };
  void start_call(Ticket ticket, const Json& state, const Questions& questions) override;
  void cancel_call(Ticket ticket) override;
  void on_pump() override;
  void ask_next(Ticket ticket, Running& running, const Questions& questions);
  void on_answers(Ticket ticket, Result<Answers> result);

  std::vector<Provider*> providers_;
  double escalate_below_;
  std::optional<Error> invalid_;
  std::unordered_map<Ticket, Running> running_;
};

}  // namespace semop

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "semop/answers.hpp"
#include "semop/questions.hpp"
#include "semop/result.hpp"

namespace semop {

using Ticket = std::uint64_t;
using Done = std::function<void(Result<Answers>)>;

/// Anything that answers named questions about a state: TypeSafe, a local model, a wrapper.
///
/// Non-blocking: `start` begins a call and returns at once; `pump` makes progress and delivers
/// results. Blocking helpers (`ask`, `apply`, `filter_items`, `rerank`, flows, bench) are built
/// on top. Use each provider from one thread at a time.
///
/// To write one, derive and implement `start_call` (and `on_pump` / `cancel_call` as needed),
/// build each answer with `make_answer`, and report with `finish`. Or derive from
/// `BlockingProvider` and implement `answer`.
class Provider {
 public:
  explicit Provider(std::string name);
  virtual ~Provider();
  Provider(const Provider&) = delete;
  Provider& operator=(const Provider&) = delete;

  /// The provider's name, used in errors and `Call` records, e.g. "TypeSafe".
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  /// Start answering every question about `state`. Never blocks and never calls `done` itself:
  /// `done` runs exactly once, inside a later `pump()` on this thread, unless the call is
  /// cancelled first. Invalid questions come back through `done` as `invalid_question`, and
  /// nothing is sent.
  Ticket start(Json state, Questions questions, Done done);

  /// Stop a call. Its `done` is not called.
  void cancel(Ticket ticket);

  /// Make progress and run the `done` callbacks of finished calls. Never blocks (except for
  /// a `BlockingProvider`, which does its work here). Does nothing if called from a callback.
  void pump();

  /// Used by blocking helpers: wait up to `max_wait` for progress. The default sleeps up to 1 ms.
  virtual void wait(std::chrono::milliseconds max_wait);

  /// Calls started and not yet delivered or cancelled.
  [[nodiscard]] std::size_t in_flight() const noexcept { return calls_.size(); }

  /// True while callbacks are running. Blocking helpers refuse to run then.
  [[nodiscard]] bool pumping() const noexcept { return in_pump_; }

 protected:
  /// Begin answering valid, uniquely named questions. Report the result with `finish`.
  virtual void start_call(Ticket ticket, const Json& state, const Questions& questions) = 0;
  /// Stop working on a call. No `finish` is needed (it would be ignored).
  virtual void cancel_call(Ticket ticket);
  /// Called at the start of every `pump()`.
  virtual void on_pump();
  /// Report a call's result. Safe from any thread; delivered during `pump()`. The base class
  /// turns answers that don't match the questions into a provider_error.
  void finish(Ticket ticket, Result<Answers> result);
  /// A provider_error from this provider: "<name>: <message>".
  [[nodiscard]] Error provider_error(std::string message) const;

 private:
  struct Pending {
    Done done;
    std::vector<std::string> names;
  };

  std::string name_;
  Ticket next_ticket_ = 0;
  std::unordered_map<Ticket, Pending> calls_;
  bool in_pump_ = false;

  std::mutex finished_mutex_;
  std::vector<std::pair<Ticket, Result<Answers>>> finished_;
  std::vector<std::pair<Ticket, Result<Answers>>> delivering_;  // reused by pump()
};

/// A provider whose work blocks, such as a local model: implement `answer`, and calls run one
/// after another inside `pump()`.
class BlockingProvider : public Provider {
 public:
  using Provider::Provider;
  void wait(std::chrono::milliseconds max_wait) override;

 protected:
  virtual Result<Answers> answer(const Json& state, const Questions& questions) = 0;

 private:
  struct Job {
    Ticket ticket;
    Json state;
    Questions questions;
  };
  void start_call(Ticket ticket, const Json& state, const Questions& questions) final;
  void cancel_call(Ticket ticket) final;
  void on_pump() final;

  std::vector<Job> queue_;
};

/// A BlockingProvider from a function; for tests, rules, or wrapping your own model.
class FunctionProvider final : public BlockingProvider {
 public:
  using Fn = std::function<Result<Answers>(const Json& state, const Questions& questions)>;
  FunctionProvider(std::string name, Fn fn);

 protected:
  Result<Answers> answer(const Json& state, const Questions& questions) override;

 private:
  Fn fn_;
};

// ---------------------------------------------------------------------------
// Blocking helpers
// ---------------------------------------------------------------------------

/// Ask every question about `state` in one call, and wait for the answers.
[[nodiscard]] Result<Answers> ask(Provider& provider, const Json& state, const Questions& questions);

/// Ask several operators in one call: `apply(provider, message, {department, urgency})`.
[[nodiscard]] inline Result<Answers> apply(Provider& provider, const Json& state,
                                           std::initializer_list<NamedQuestion> operators) {
  return ask(provider, state, Questions(operators));
}

/// Ask one operator: `ask(provider, message, urgent)`.
template <class Q>
[[nodiscard]] Result<Answer> ask(Provider& provider, const Json& state, const Operator<Q>& op) {
  auto answers = ask(provider, state, Questions{op.named()});
  if (!answers) return answers.error();
  return answers->at(op.name);
}

/// The questions of several operators, e.g. for `bench::run`.
[[nodiscard]] inline Questions questions(std::initializer_list<NamedQuestion> operators) {
  return Questions(operators);
}

}  // namespace semop

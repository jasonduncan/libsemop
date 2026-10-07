#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "semop/provider.hpp"

namespace semop {

// A flow is a decision built from operators, written as a plain C++ function. `if` is the flow
// language:
//
//   semop::Outcome<std::string> triage(semop::Ask& ask, const std::string& message) {
//     auto a = ask(message, department, urgency);   // one call for both operators
//     auto team = a[department];                    // semop::Value<std::string>
//     if (!team) return "human";                    // undecided: a person decides
//     if (*team == "technical") {
//       auto down = ask(message, outage)[outage];   // a second call, only when needed
//       if (!down) return down.stop();              // "don't know" ends the flow here
//       if (*down) return "page on-call";
//     }
//     return *team;
//   }
//   auto result = semop::run_flow(provider, triage, message);
//
// Reading an undecided answer gives an empty Value. Returning its `stop()` ends the flow with
// `stopped_at` naming the operator; handling it (returning something else) is fine too. A
// provider error ends the flow with `error` whatever the flow returns, and every later `ask`
// fails without calling the provider. Each `ask` is a provider call: ask everything you might
// need up front, and add a later step only when it depends on an earlier answer.

/// One `ask`: the state and the full answers.
struct Step {
  Json state;
  Answers answers;
};

/// Why a flow stopped. Only a `Value` can make one.
class Stop {
 public:
  /// The undecided operator; empty when the stop follows a provider error.
  [[nodiscard]] const std::string& undecided() const noexcept { return name_; }

 private:
  template <class>
  friend class Value;
  explicit Stop(std::string name) : name_(std::move(name)) {}
  std::string name_;
};

/// An operator's value inside a flow: the value, or why there isn't one.
template <class T>
class Value {
 public:
  explicit operator bool() const noexcept { return value_.has_value(); }
  [[nodiscard]] const T& operator*() const {
    SEMOP_ASSERT(value_.has_value(), "Value has no value; check it first");
    return *value_;
  }
  const T* operator->() const { return &**this; }
  /// The model wasn't sure (a tie, or below `min_confidence`).
  [[nodiscard]] bool undecided() const noexcept { return !value_ && !failed_; }
  /// The `ask` failed; the flow's result will carry the error.
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  /// End the flow here: `return value.stop();`.
  [[nodiscard]] Stop stop() const { return Stop(failed_ ? std::string() : name_); }

 private:
  friend class Asked;
  Value(std::optional<T> value, std::string name, bool failed)
      : value_(std::move(value)), name_(std::move(name)), failed_(failed) {}
  std::optional<T> value_;
  std::string name_;
  bool failed_;
};

/// What a flow function returns: a result, or a Stop.
template <class T>
class Outcome {
 public:
  Outcome(T value) : value_(std::move(value)) {}
  template <class U, std::enable_if_t<std::is_constructible_v<T, U&&> && !std::is_same_v<std::decay_t<U>, T> &&
                                          !std::is_same_v<std::decay_t<U>, Stop>,
                                      int> = 0>
  Outcome(U&& value) : value_(T(std::forward<U>(value))) {}
  Outcome(Stop stop) : stop_(std::move(stop)) {}

  [[nodiscard]] bool stopped() const noexcept { return stop_.has_value(); }
  [[nodiscard]] const Stop& stop() const { return *stop_; }
  [[nodiscard]] T take() && { return std::move(*value_); }

 private:
  std::optional<T> value_;
  std::optional<Stop> stop_;
};

/// The answers from one `ask`, read by operator.
class Asked {
 public:
  /// The operator's value. The operator must have been asked in this call.
  template <class Q>
  [[nodiscard]] Value<ValueOf<Q>> operator[](const Operator<Q>& op) const {
    if (!answers_) return Value<ValueOf<Q>>(std::nullopt, op.name, true);
    SEMOP_ASSERT(answers_->find(op.name) != nullptr, "operator was not asked in this call");
    return Value<ValueOf<Q>>(answers_->value(op), op.name, false);
  }
  /// The whole answer (probabilities, confidence), or null if the ask failed.
  [[nodiscard]] const Answer* full(std::string_view name) const {
    return answers_ ? answers_->find(name) : nullptr;
  }
  [[nodiscard]] bool failed() const noexcept { return !answers_.has_value(); }

 private:
  friend class Ask;
  Asked() = default;
  explicit Asked(Answers answers) : answers_(std::move(answers)) {}
  std::optional<Answers> answers_;
};

/// The handle a flow asks through.
class Ask {
 public:
  /// Ask the operators about `state` in one call.
  template <class... Qs>
  Asked operator()(const Json& state, const Operator<Qs>&... ops) {
    return (*this)(state, Questions{ops.named()...});
  }
  Asked operator()(const Json& state, const Questions& questions);

 private:
  template <class Fn, class... Args>
  friend auto run_flow(Provider& provider, Fn&& fn, Args&&... args);
  explicit Ask(Provider& provider) : provider_(provider) {}

  Provider& provider_;
  std::vector<Step> trace_;
  std::optional<Error> error_;
};

/// What a flow did.
template <class T>
struct FlowResult {
  /// What the flow returned; nullopt if it stopped or failed.
  std::optional<T> value;
  /// The undecided operator that stopped it, if any.
  std::optional<std::string> stopped_at;
  /// A provider error (or invalid question) that ended it, if any.
  std::optional<Error> error;
  /// Every successful call.
  std::vector<Step> trace;

  [[nodiscard]] bool decided() const noexcept { return value.has_value(); }
  [[nodiscard]] std::size_t calls() const noexcept { return trace.size(); }
};

template <class O>
struct outcome_value;
template <class T>
struct outcome_value<Outcome<T>> {
  using type = T;
};

/// Run `fn(ask, args...)` against `provider`. Blocking.
template <class Fn, class... Args>
auto run_flow(Provider& provider, Fn&& fn, Args&&... args) {
  using Out = std::decay_t<std::invoke_result_t<Fn&, Ask&, Args&&...>>;
  using T = typename outcome_value<Out>::type;
  Ask ask(provider);
  Out outcome = std::invoke(fn, ask, std::forward<Args>(args)...);
  FlowResult<T> result;
  result.trace = std::move(ask.trace_);
  if (ask.error_) {
    result.error = std::move(ask.error_);
  } else if (outcome.stopped()) {
    result.stopped_at = outcome.stop().undecided();
  } else {
    result.value = std::move(outcome).take();
  }
  return result;
}

}  // namespace semop

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "semop/questions.hpp"
#include "semop/result.hpp"

namespace semop {

/// What one provider call reported about itself. Every answer from that call shares it.
/// `model` is what the provider says answered (it can differ from an alias such as
/// `jev-latest`). Anything unreported is nullopt, never zero.
struct Call {
  std::string provider;
  std::optional<std::string> model;
  std::optional<std::int64_t> input_tokens;
  std::optional<std::int64_t> output_tokens;
};

/// An answer's value: a bool (Boolean), an option name (Choice), an expected level (Score), or
/// std::monostate when undecided.
using AnswerValue = std::variant<std::monostate, bool, std::string, double>;

/// The answer to one question.
struct Answer {
  /// Undecided (monostate) when the model was split evenly or below the question's
  /// `min_confidence`. Probabilities are kept either way.
  AnswerValue value;
  /// "true"/"false", option names, or level texts, in the question's order.
  std::vector<std::pair<std::string, double>> probabilities;
  /// The probability of its own answer: the chosen side, the chosen option, or the level
  /// nearest a Score's value. The model's view, not a guarantee.
  double confidence = 0;
  /// Which provider and model answered, and the tokens used.
  std::shared_ptr<const Call> call;
  /// The provider's own answer, for when you need more.
  Json raw;

  [[nodiscard]] bool decided() const noexcept { return value.index() != 0; }
  [[nodiscard]] std::optional<bool> boolean() const;
  [[nodiscard]] std::optional<std::string> choice() const;
  [[nodiscard]] std::optional<double> score() const;
  /// The probability of `key` ("true", an option, a level), or nullopt.
  [[nodiscard]] std::optional<double> probability(std::string_view key) const;
};

/// The answers from one call, in question order.
class Answers {
 public:
  Answers() = default;

  void add(std::string name, Answer answer) { entries_.emplace_back(std::move(name), std::move(answer)); }

  [[nodiscard]] const Answer* find(std::string_view name) const noexcept {
    for (const auto& [n, a] : entries_) {
      if (n == name) return &a;
    }
    return nullptr;
  }
  [[nodiscard]] const Answer& at(std::string_view name) const {
    const Answer* answer = find(name);
    SEMOP_ASSERT(answer != nullptr, "no answer with that name");
    return *answer;
  }

  template <class Q>
  [[nodiscard]] const Answer& operator[](const Operator<Q>& op) const {
    return at(op.name);
  }

  /// The operator's typed value, or nullopt when its answer is undecided or missing.
  template <class Q>
  [[nodiscard]] std::optional<ValueOf<Q>> value(const Operator<Q>& op) const {
    const Answer* answer = find(op.name);
    if (answer == nullptr) return std::nullopt;
    if constexpr (std::is_same_v<Q, Boolean>) {
      return answer->boolean();
    } else if constexpr (std::is_same_v<Q, Choice>) {
      return answer->choice();
    } else if constexpr (std::is_same_v<Q, Score>) {
      return answer->score();
    } else {
      const auto label = answer->choice();
      if (!label) return std::nullopt;
      for (const auto& [value, text] : choice_labels<typename enum_of<Q>::type>::values) {
        if (text == *label) return value;
      }
      return std::nullopt;
    }
  }

  [[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] auto end() const noexcept { return entries_.end(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

 private:
  template <class Q>
  struct enum_of;
  template <class E>
  struct enum_of<EnumChoice<E>> {
    using type = E;
  };

  std::vector<std::pair<std::string, Answer>> entries_;
};

/// Providers round what they return (TypeSafe to 2 decimals, Laya to 4), so each probability may
/// be off by up to half a hundredth. Totals and expected scores are checked with margins that
/// allow exactly that much rounding and no more.
inline constexpr double rounding = 0.005;

/// Build an Answer from a provider's value and probabilities (in any order). Every provider uses
/// this, so ties and `min_confidence` behave the same everywhere. It checks the output: the
/// probabilities cover exactly the question's outcomes, are finite, in [0, 1], and sum to 1,
/// and the value agrees with them. A malformed response is a provider_error, never a
/// confident-looking answer. The error's `provider` is empty; the provider fills it in.
[[nodiscard]] Result<Answer> make_answer(const Question& question, const AnswerValue& value,
                                         std::vector<std::pair<std::string, double>> probabilities,
                                         Json raw = nullptr, std::shared_ptr<const Call> call = nullptr);

/// `1 - p` without float noise, for providers that report only p(true): `1 - 0.07` is
/// 0.9299999999999999, which would fail a 0.93 threshold. Rounds to 10 places.
[[nodiscard]] double complement(double p) noexcept;

}  // namespace semop

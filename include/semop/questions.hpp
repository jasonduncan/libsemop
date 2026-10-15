#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "semop/result.hpp"

namespace semop {

// Every question has `min_confidence` (0 to 1). When the model's confidence in its answer is
// below it, the answer comes back undecided instead of as a guess. 0, the default, never
// withholds. Questions are plain structs; they are checked where they're used (see `check`).

/// A yes/no question. `yes` / `no` optionally describe each outcome (Python's `true` / `false`).
struct Boolean {
  std::string instructions;
  std::optional<std::string> yes;
  std::optional<std::string> no;
  double min_confidence = 0;

  [[nodiscard]] Boolean with_min_confidence(double c) const {
    Boolean q = *this;
    q.min_confidence = c;
    return q;
  }
};

/// One option of a Choice: a name, optionally described.
struct Option {
  Option(std::string option_name, std::optional<std::string> option_description = std::nullopt)
      : name(std::move(option_name)), description(std::move(option_description)) {}
  Option(const char* option_name) : name(option_name) {}

  std::string name;
  std::optional<std::string> description;
};

/// Pick one of at least two named options: `Choice{"Which team?", {"billing", "technical"}}`,
/// or described: `Choice{"Which team?", {{"billing", "Payments"}, {"technical", "Bugs"}}}`.
struct Choice {
  std::string instructions;
  std::vector<Option> options;
  double min_confidence = 0;

  [[nodiscard]] Choice with_min_confidence(double c) const {
    Choice q = *this;
    q.min_confidence = c;
    return q;
  }
};

/// Rate on an ordered rubric of at least two levels, lowest first: `levels[0]` is score 0.
struct Score {
  std::string instructions;
  std::vector<std::string> levels;
  double min_confidence = 0;

  [[nodiscard]] Score with_min_confidence(double c) const {
    Score q = *this;
    q.min_confidence = c;
    return q;
  }
};

/// Locate one thing in an image; the answer's value is a `Region`. Needs an image in the
/// request and a model that supports it (TypeSafe: `jev-1.14.0` / `jev-preview` only). There is
/// no "not found" answer (the model always returns a box), so pair it with a presence Boolean,
/// or use `semop::locate`, which does. One box per question.
struct Box {
  std::string instructions;
  double min_confidence = 0;

  [[nodiscard]] Box with_min_confidence(double c) const {
    Box q = *this;
    q.min_confidence = c;
    return q;
  }
};

/// A box's edges as fractions (0 to 1) of the image's height (top, bottom) and width (left, right).
struct Region {
  double top = 0;
  double bottom = 0;
  double left = 0;
  double right = 0;

  [[nodiscard]] double width() const noexcept { return right - left; }
  [[nodiscard]] double height() const noexcept { return bottom - top; }
  /// Grown by `fraction` of its own size on every side, clipped to the image. Boxes can be tight
  /// or cut a little off; padding before cropping is the cheap fix.
  [[nodiscard]] Region padded(double fraction) const noexcept;
  /// Pixel edges for an image of `width` x `height`.
  struct Pixels {
    int left, top, right, bottom;
  };
  [[nodiscard]] Pixels pixels(int image_width, int image_height) const noexcept;
  /// Edges finite, within [0, 1], and in order.
  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const Region& a, const Region& b) noexcept {
    return a.top == b.top && a.bottom == b.bottom && a.left == b.left && a.right == b.right;
  }
  friend bool operator!=(const Region& a, const Region& b) noexcept { return !(a == b); }
};

using Question = std::variant<Boolean, Choice, Score, Box>;

/// A question with the name its answer comes back under.
struct NamedQuestion {
  std::string name;
  Question question;
};

/// Questions for one call, in order. Names must be unique.
using Questions = std::vector<NamedQuestion>;

/// Why `question` is malformed, or nullopt. Messages match the Python library's.
[[nodiscard]] std::optional<Error> check(const Question& question);
/// Every question checked, plus unique, non-empty names and at least one question.
[[nodiscard]] std::optional<Error> check(const Questions& questions);

// ---------------------------------------------------------------------------
// Enum-typed choices
// ---------------------------------------------------------------------------

/// Specialize to use an enum as a Choice's options:
///
///     template <> struct semop::choice_labels<Team> {
///       static constexpr std::array values{
///           std::pair{Team::billing, std::string_view{"billing"}},
///           std::pair{Team::technical, std::string_view{"technical"}},
///       };
///     };
template <class E>
struct choice_labels;

template <class E, class = void>
struct has_choice_labels : std::false_type {};
template <class E>
struct has_choice_labels<E, std::void_t<decltype(choice_labels<E>::values)>> : std::true_type {};

/// A Choice whose options come from `choice_labels<E>`.
template <class E>
struct EnumChoice {
  static_assert(std::is_enum_v<E> && has_choice_labels<E>::value,
                "specialize semop::choice_labels<E> for this enum");
  std::string instructions;
  /// Optional descriptions; options without one are undescribed.
  std::vector<std::pair<E, std::string>> descriptions;
  double min_confidence = 0;

  [[nodiscard]] Choice choice() const {
    Choice q{instructions, {}, min_confidence};
    for (const auto& [value, label] : choice_labels<E>::values) {
      std::optional<std::string> description;
      for (const auto& [described, text] : descriptions) {
        if (described == value) description = text;
      }
      q.options.emplace_back(std::string(label), std::move(description));
    }
    return q;
  }

  [[nodiscard]] EnumChoice with_min_confidence(double c) const {
    EnumChoice q = *this;
    q.min_confidence = c;
    return q;
  }
};

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

/// The typed value an operator's answer carries.
template <class Q>
struct value_of;
template <>
struct value_of<Boolean> {
  using type = bool;
};
template <>
struct value_of<Choice> {
  using type = std::string;
};
template <>
struct value_of<Score> {
  using type = double;
};
template <>
struct value_of<Box> {
  using type = Region;
};
template <class E>
struct value_of<EnumChoice<E>> {
  using type = E;
};
template <class Q>
using ValueOf = typename value_of<Q>::type;

/// A semantic judgment defined once, with a name: `Operator{"urgent", Boolean{"Is it urgent?"}}`.
/// The name identifies its answer, so several operators can be asked in one call. The wording
/// is part of the operator: it changes the answers.
template <class Q>
struct Operator {
  std::string name;
  Q question;

  /// The question as sent to a provider.
  [[nodiscard]] NamedQuestion named() const {
    if constexpr (std::is_same_v<Q, Boolean> || std::is_same_v<Q, Choice> || std::is_same_v<Q, Score> ||
                  std::is_same_v<Q, Box>) {
      return {name, question};
    } else {
      return {name, question.choice()};
    }
  }
  operator NamedQuestion() const { return named(); }
};

template <class Q>
Operator(const char*, Q) -> Operator<Q>;
template <class Q>
Operator(std::string, Q) -> Operator<Q>;

}  // namespace semop

#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "semop/provider.hpp"

namespace semop {

// Filtering: ask one yes/no question of every item, and keep the ones the model says yes to.
// Each item gets its own call, so an item's answer can't depend on the others. Item ids are
// never sent. Every item ends in exactly one outcome, in input order.

/// One item's outcome.
struct Verdict {
  enum class Outcome {
    match,   ///< decided yes
    no,      ///< decided no
    unsure,  ///< undecided (below min_confidence, or a tie); never a match
    failed,  ///< the provider failed for this item (including a timeout)
  };

  std::string id;
  Outcome outcome = Outcome::failed;
  std::optional<Answer> answer;  ///< absent when failed
  std::optional<Error> error;    ///< set when failed
};

struct Filtered {
  Boolean question;
  std::vector<Verdict> verdicts;  ///< one per item, in input order

  [[nodiscard]] std::vector<const Verdict*> with(Verdict::Outcome outcome) const;
  [[nodiscard]] std::vector<const Verdict*> matched() const { return with(Verdict::Outcome::match); }
  [[nodiscard]] std::vector<const Verdict*> rejected() const { return with(Verdict::Outcome::no); }
  [[nodiscard]] std::vector<const Verdict*> unsure() const { return with(Verdict::Outcome::unsure); }
  [[nodiscard]] std::vector<const Verdict*> failed() const { return with(Verdict::Outcome::failed); }
  /// Every model the provider reported. More than one: answers may not compare.
  [[nodiscard]] std::set<std::string> models() const;
};

struct FilterOptions {
  /// Shared background sent with every item: the state becomes `{"context": ..., "item": ...}`.
  /// Null sends the item alone.
  Json context = nullptr;
  /// Calls in flight at once.
  std::size_t concurrency = 8;
};

/// Ask `question` of every item (id, state), one call each. Fails only for invalid arguments;
/// a provider failure is recorded for its own item and the rest carry on.
[[nodiscard]] Result<Filtered> filter_items(Provider& provider, const Boolean& question,
                                            const std::vector<std::pair<std::string, Json>>& items,
                                            const FilterOptions& options = {});

/// One question about each of many states, one call each, up to `concurrency` in flight;
/// results in input order. Used by filtering and reranking.
[[nodiscard]] Result<std::vector<Result<Answer>>> ask_each(Provider& provider, const Question& question,
                                                           const std::vector<Json>& states,
                                                           std::size_t concurrency);

}  // namespace semop

#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "semop/provider.hpp"

namespace semop {

// Reranking: score each candidate's relevance to a query against a rubric (a Score), one call
// per candidate, then sort. Retrieval stays in your code; pass candidates in retrieval order.
// Each call sees only `{"query", "document"}` (plus "context" if given): ids and positions are
// never sent, so a score doesn't depend on which other documents came with it.
//
// The ranking key is the expected rubric level, not confidence. Ties keep retrieval order.
// Scores aren't normalized: all candidates can be relevant, or none. Undecided or failed
// candidates go to `unscored`, never given a made-up score.

struct Ranked {
  std::string id;
  Answer answer;
  double score = 0;        ///< the ranking key: the expected level, or a reweighted value
  std::size_t position = 0;  ///< where retrieval put it (0 = first); breaks ties
};

/// A candidate without a score: undecided (`answer`) or failed (`error`).
struct Unscored {
  std::string id;
  std::optional<Answer> answer;
  std::optional<Error> error;
};

struct Ranking {
  Score question;
  std::vector<Ranked> ranked;      ///< best first
  std::vector<Unscored> unscored;  ///< in retrieval order

  [[nodiscard]] bool complete() const noexcept { return unscored.empty(); }
  /// Every model the provider reported. More than one: scores may not compare.
  [[nodiscard]] std::set<std::string> models() const;
  /// The `k` best. Fails if any candidate is unscored (unless `allow_partial`), or if answers
  /// came from more than one model (a moving alias such as `jev-latest` can change mid-run).
  [[nodiscard]] Result<std::vector<Ranked>> top(std::size_t k, bool allow_partial = false) const;
};

struct RerankOptions {
  Json context = nullptr;  ///< sent with every candidate as "context"; null sends none
  std::size_t concurrency = 8;
};

[[nodiscard]] Result<Ranking> rerank(Provider& provider, const Score& question, const std::string& query,
                                     const std::vector<std::pair<std::string, Json>>& candidates,
                                     const RerankOptions& options = {});

/// Re-rank by `sum(weight[level] * p[level])` from the probabilities already returned; no calls.
/// One finite weight per level, strictly increasing.
[[nodiscard]] Result<Ranking> reweighted(const Ranking& ranking, const std::vector<double>& weights);

}  // namespace semop

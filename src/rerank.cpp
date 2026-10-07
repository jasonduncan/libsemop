#include "semop/rerank.hpp"

#include <algorithm>
#include <cmath>

#include "semop/filtering.hpp"
#include "util.hpp"

namespace semop {

namespace {

void sort_ranked(std::vector<Ranked>& ranked) {
  // Highest score first; exact ties keep retrieval order. No rounding, no "close enough".
  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.position < b.position;
  });
}

}  // namespace

std::set<std::string> Ranking::models() const {
  std::set<std::string> models;
  const auto add = [&](const Answer& answer) {
    if (answer.call && answer.call->model) models.insert(*answer.call->model);
  };
  for (const auto& r : ranked) add(r.answer);
  for (const auto& u : unscored) {
    if (u.answer) add(*u.answer);
  }
  return models;
}

Result<std::vector<Ranked>> Ranking::top(std::size_t k, bool allow_partial) const {
  if (!complete() && !allow_partial) {
    std::string ids;
    for (const auto& u : unscored) ids += (ids.empty() ? "" : ", ") + u.id;
    return detail::invalid(ErrorKind::invalid_argument, std::to_string(unscored.size()) +
                                                            " candidate(s) unscored: " + ids +
                                                            "; pass allow_partial=true to accept");
  }
  const auto used = models();
  if (used.size() > 1) {
    std::string list;
    for (const auto& m : used) list += (list.empty() ? "" : ", ") + m;
    return detail::invalid(ErrorKind::invalid_argument, "scores came from different models: " + list);
  }
  return std::vector<Ranked>(ranked.begin(), ranked.begin() + std::ptrdiff_t(std::min(k, ranked.size())));
}

Result<Ranking> rerank(Provider& provider, const Score& question, const std::string& query,
                       const std::vector<std::pair<std::string, Json>>& candidates, const RerankOptions& options) {
  if (detail::blank(query)) return detail::invalid(ErrorKind::invalid_argument, "query must be a non-empty string");
  std::vector<Json> states;
  states.reserve(candidates.size());
  for (const auto& [id, document] : candidates) {
    if (id.empty()) return detail::invalid(ErrorKind::invalid_argument, "candidate ids must be non-empty strings");
    Json state = {{"query", query}, {"document", document}};
    if (!options.context.is_null()) state["context"] = options.context;
    states.push_back(std::move(state));
  }
  auto results = ask_each(provider, question, states, options.concurrency);
  if (!results) return results.error();

  Ranking ranking{question, {}, {}};
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    auto& result = (*results)[i];
    if (result && result->decided()) {
      const double score = *result->score();
      ranking.ranked.push_back(Ranked{candidates[i].first, std::move(result).value(), score, i});
    } else if (result) {
      ranking.unscored.push_back(Unscored{candidates[i].first, std::move(result).value(), std::nullopt});
    } else {
      ranking.unscored.push_back(Unscored{candidates[i].first, std::nullopt, result.error()});
    }
  }
  sort_ranked(ranking.ranked);
  return ranking;
}

Result<Ranking> reweighted(const Ranking& ranking, const std::vector<double>& weights) {
  const auto& levels = ranking.question.levels;
  bool ok = weights.size() == levels.size();
  for (std::size_t i = 0; ok && i < weights.size(); ++i) {
    ok = std::isfinite(weights[i]) && (i == 0 || weights[i - 1] < weights[i]);
  }
  if (!ok) {
    return detail::invalid(ErrorKind::invalid_argument, "need " + std::to_string(levels.size()) +
                                                            " finite, strictly increasing weights, one per level");
  }
  Ranking out{ranking.question, {}, ranking.unscored};
  for (const auto& r : ranking.ranked) {
    std::vector<double> ps;
    std::vector<double> weighted;
    for (std::size_t i = 0; i < levels.size(); ++i) {
      const double p = r.answer.probability(levels[i]).value_or(0);
      ps.push_back(p);
      weighted.push_back(weights[i] * p);
    }
    out.ranked.push_back(Ranked{r.id, r.answer, detail::fsum(weighted) / detail::fsum(ps), r.position});
  }
  sort_ranked(out.ranked);
  return out;
}

}  // namespace semop

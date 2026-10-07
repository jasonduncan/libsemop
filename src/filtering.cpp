#include "semop/filtering.hpp"

#include "util.hpp"

namespace semop {

Result<std::vector<Result<Answer>>> ask_each(Provider& provider, const Question& question,
                                             const std::vector<Json>& states, std::size_t concurrency) {
  if (concurrency < 1) return detail::invalid(ErrorKind::invalid_argument, "concurrency must be a positive integer");
  if (auto problem = check(question)) return *problem;
  if (provider.pumping()) {
    return detail::invalid(ErrorKind::invalid_argument,
                           "blocking calls can't run inside a provider callback; use Provider::start");
  }

  static const std::string name = "q";
  const Questions questions{{name, question}};
  std::vector<std::optional<Result<Answer>>> results(states.size());
  std::size_t next = 0;
  std::size_t running = 0;
  std::size_t finished = 0;
  while (finished < states.size()) {
    while (next < states.size() && running < concurrency) {
      const std::size_t i = next++;
      ++running;
      provider.start(states[i], questions, [&, i](Result<Answers> result) {
        if (result) {
          results[i].emplace(result->at(name));
        } else {
          results[i].emplace(result.error());
        }
        --running;
        ++finished;
      });
    }
    const std::size_t before = finished;
    provider.pump();
    if (finished == before && finished < states.size()) provider.wait(std::chrono::milliseconds(10));
  }

  std::vector<Result<Answer>> out;
  out.reserve(results.size());
  for (auto& result : results) out.push_back(std::move(*result));
  return out;
}

std::vector<const Verdict*> Filtered::with(Verdict::Outcome outcome) const {
  std::vector<const Verdict*> out;
  for (const auto& verdict : verdicts) {
    if (verdict.outcome == outcome) out.push_back(&verdict);
  }
  return out;
}

std::set<std::string> Filtered::models() const {
  std::set<std::string> models;
  for (const auto& verdict : verdicts) {
    if (verdict.answer && verdict.answer->call && verdict.answer->call->model) models.insert(*verdict.answer->call->model);
  }
  return models;
}

Result<Filtered> filter_items(Provider& provider, const Boolean& question,
                              const std::vector<std::pair<std::string, Json>>& items, const FilterOptions& options) {
  std::vector<Json> states;
  states.reserve(items.size());
  for (const auto& [id, item] : items) {
    if (id.empty()) return detail::invalid(ErrorKind::invalid_argument, "item ids must be non-empty strings");
    states.push_back(options.context.is_null() ? item : Json{{"context", options.context}, {"item", item}});
  }
  auto results = ask_each(provider, question, states, options.concurrency);
  if (!results) return results.error();

  Filtered filtered{question, {}};
  for (std::size_t i = 0; i < items.size(); ++i) {
    Verdict verdict;
    verdict.id = items[i].first;
    auto& result = (*results)[i];
    if (!result) {
      verdict.outcome = Verdict::Outcome::failed;
      verdict.error = result.error();
    } else {
      const auto yes = result->boolean();
      verdict.outcome = !yes ? Verdict::Outcome::unsure : (*yes ? Verdict::Outcome::match : Verdict::Outcome::no);
      verdict.answer = std::move(result).value();
    }
    filtered.verdicts.push_back(std::move(verdict));
  }
  return filtered;
}

}  // namespace semop

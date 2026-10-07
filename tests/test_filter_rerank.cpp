// Filtering and reranking, offline. Ports tests/test_filtering.py (library part) and
// tests/test_rerank.py.
#include <cmath>
#include <limits>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "scripted.hpp"

using namespace semop;
using Catch::Matchers::ContainsSubstring;

namespace {

const Boolean question = Boolean{"Is it a yes?"}.with_min_confidence(0.8);

std::string text_of(const Json& state) {
  if (state.is_string()) return state.get<std::string>();
  return text_of(state["item"]);
}

/// "yes" matches, "no" doesn't, "maybe" is unsure, "boom" fails; "other model" reports model-b.
Result<Answers> decide(const Json& state, const Questions& questions) {
  const std::string text = text_of(state);
  if (text.find("boom") != std::string::npos) {
    Error e;
    e.kind = ErrorKind::provider_error;
    e.message = "503 Service Unavailable";
    return e;
  }
  const double p = text.find("yes") != std::string::npos ? 0.95 : text.find("maybe") != std::string::npos ? 0.6 : 0.05;
  const std::string model = text.find("other model") != std::string::npos ? "model-b" : "model-a";
  auto call = std::make_shared<const Call>(Call{"Fake", model, std::nullopt, std::nullopt});
  Answers answers;
  answers.add(questions[0].name,
              make_answer(questions[0].question, p > 0.5, {{"true", p}, {"false", complement(p)}}, nullptr, call).value());
  return answers;
}

const std::vector<std::pair<std::string, Json>> items = {
    {"a", "yes please"}, {"b", "no thanks"}, {"c", "maybe"}, {"d", "boom"}, {"e", "yes again"}};

std::vector<std::string> ids(const std::vector<const Verdict*>& verdicts) {
  std::vector<std::string> out;
  for (const auto* v : verdicts) out.push_back(v->id);
  return out;
}

}  // namespace

TEST_CASE("filter: every item gets one outcome, in input order") {
  FunctionProvider direct("Fake", decide);
  auto result = filter_items(direct, question, items, {nullptr, 3});
  REQUIRE(result);
  using O = Verdict::Outcome;
  std::vector<std::pair<std::string, O>> got;
  for (const auto& v : result->verdicts) got.emplace_back(v.id, v.outcome);
  CHECK(got == std::vector<std::pair<std::string, O>>{
                   {"a", O::match}, {"b", O::no}, {"c", O::unsure}, {"d", O::failed}, {"e", O::match}});
  CHECK(ids(result->matched()) == std::vector<std::string>{"a", "e"});
  REQUIRE(result->failed().size() == 1);
  CHECK(result->failed()[0]->error->message == "Fake: 503 Service Unavailable");
  CHECK(result->models() == std::set<std::string>{"model-a"});
}

TEST_CASE("filter: concurrency is respected, out-of-order finishes keep input order, ids are never sent") {
  scripted::Provider provider("Fake", decide);
  provider.hold = true;
  // Release held calls, newest first, whenever the provider is pumped with work outstanding.
  class Releasing : public semop::Provider {
   public:
    explicit Releasing(scripted::Provider& inner) : semop::Provider("Fake"), inner_(inner) {}

   protected:
    void start_call(Ticket ticket, const Json& state, const Questions& questions) override {
      inner_.start(state, questions, [this, ticket](Result<Answers> r) { finish(ticket, std::move(r)); });
    }
    void on_pump() override {
      inner_.release();
      inner_.pump();
    }

   private:
    scripted::Provider& inner_;
  };
  Releasing releasing(provider);
  auto result = filter_items(releasing, question, items, {nullptr, 3});
  REQUIRE(result);
  CHECK(provider.peak == 3);
  CHECK(result->verdicts[0].id == "a");
  CHECK(result->verdicts[4].outcome == Verdict::Outcome::match);
  std::multiset<std::string> sent;
  for (const auto& c : provider.calls) sent.insert(c.state.get<std::string>());
  CHECK(sent == std::multiset<std::string>{"yes please", "no thanks", "maybe", "boom", "yes again"});
}

TEST_CASE("filter: context wraps each item; bad arguments are refused") {
  scripted::Provider provider("Fake", decide);
  REQUIRE(filter_items(provider, question, {{"a", "yes"}}, {"the rules", 8}));
  CHECK(provider.calls[0].state == Json{{"context", "the rules"}, {"item", "yes"}});
  CHECK(filter_items(provider, question, {{"", "yes"}}).error().message == "item ids must be non-empty strings");
  CHECK(filter_items(provider, question, items, {nullptr, 0}).error().message == "concurrency must be a positive integer");
  CHECK(filter_items(provider, Boolean{""}, items).error().kind == ErrorKind::invalid_question);
}

// ---------------------------------------------------------------------------
// Reranking
// ---------------------------------------------------------------------------

namespace {

const Score relevance{"How useful is the document for the query?", {"none", "some", "full"}};

struct Dist {
  double low, mid, high;
};

/// Answers from script[document]: a distribution, "error", or "undecided".
scripted::Provider rerank_provider(std::map<std::string, Json> script, std::map<std::string, std::string> models = {}) {
  return scripted::Provider("Scripted", [script, models](const Json& state, const Questions& questions) -> Result<Answers> {
    const std::string doc = state["document"].get<std::string>();
    const Json& entry = script.at(doc);
    if (entry == "error") {
      Error e;
      e.kind = ErrorKind::provider_error;
      e.message = "failed on " + doc;
      return e;
    }
    Score q = std::get<Score>(questions[0].question);
    std::vector<double> p = {0.2, 0.5, 0.3};
    if (entry == "undecided") {
      q.min_confidence = 0.9;
    } else {
      p = entry.get<std::vector<double>>();
    }
    const double value = 0 * p[0] + 1 * p[1] + 2 * p[2];
    const auto m = models.find(doc);
    auto call = std::make_shared<const Call>(Call{"Scripted", m == models.end() ? "model-a" : m->second, {}, {}});
    Answers answers;
    answers.add(questions[0].name,
                make_answer(q, value, {{"none", p[0]}, {"some", p[1]}, {"full", p[2]}}, nullptr, call).value());
    return answers;
  });
}

std::vector<std::string> ranked_ids(const std::vector<Ranked>& items) {
  std::vector<std::string> out;
  for (const auto& r : items) out.push_back(r.id);
  return out;
}

}  // namespace

TEST_CASE("rerank: sorts by expected level; ties keep retrieval order") {
  auto provider = rerank_provider({{"a", {0.6, 0.4, 0}}, {"b", {0, 0.2, 0.8}}, {"c", {0.6, 0.4, 0}}, {"d", {0, 0.5, 0.5}}});
  auto ranking = rerank(provider, relevance, "q", {{"A", "a"}, {"B", "b"}, {"C", "c"}, {"D", "d"}});
  REQUIRE(ranking);
  CHECK(ranked_ids(ranking->ranked) == std::vector<std::string>{"B", "D", "A", "C"});
  CHECK(ranking->complete());
  CHECK(ranked_ids(ranking->top(2).value()) == std::vector<std::string>{"B", "D"});
  CHECK(ranking->ranked[0].score == Catch::Approx(1.8));
}

TEST_CASE("rerank: each call sees only query, document, and context") {
  auto provider = rerank_provider({{"a", {1, 0, 0}}});
  REQUIRE(rerank(provider, relevance, "q", {{"doc-1", "a"}}));
  REQUIRE(rerank(provider, relevance, "q", {{"doc-1", "a"}}, {Json{{"user", "admin"}}, 8}));
  CHECK(provider.calls[0].state == Json{{"query", "q"}, {"document", "a"}});
  CHECK(provider.calls[1].state == Json{{"query", "q"}, {"document", "a"}, {"context", {{"user", "admin"}}}});
}

TEST_CASE("rerank: unscored candidates are kept; a partial top needs consent") {
  auto provider = rerank_provider({{"a", {0, 0, 1}}, {"b", "error"}, {"c", "undecided"}});
  auto ranking = rerank(provider, relevance, "q", {{"A", "a"}, {"B", "b"}, {"C", "c"}});
  REQUIRE(ranking);
  CHECK(ranked_ids(ranking->ranked) == std::vector<std::string>{"A"});
  CHECK_FALSE(ranking->complete());
  REQUIRE(ranking->unscored.size() == 2);
  CHECK(ranking->unscored[0].id == "B");
  CHECK(ranking->unscored[0].error->is_provider_error());
  CHECK(ranking->unscored[1].id == "C");
  CHECK_FALSE(ranking->unscored[1].answer->decided());
  CHECK_THAT(ranking->top(10).error().message, ContainsSubstring("unscored"));
  CHECK(ranked_ids(ranking->top(10, true).value()) == std::vector<std::string>{"A"});
}

TEST_CASE("rerank: scores from different models are refused") {
  auto provider = rerank_provider({{"a", {0, 0, 1}}, {"b", {1, 0, 0}}}, {{"b", "model-b"}});
  auto ranking = rerank(provider, relevance, "q", {{"A", "a"}, {"B", "b"}});
  REQUIRE(ranking);
  CHECK(ranking->models() == std::set<std::string>{"model-a", "model-b"});
  CHECK_THAT(ranking->top(10).error().message, ContainsSubstring("different models"));
}

TEST_CASE("rerank: reweighting can change the order without calling again") {
  auto provider = rerank_provider({{"a", {0.5, 0, 0.5}}, {"b", {0, 1, 0}}});
  auto ranking = rerank(provider, relevance, "q", {{"A", "a"}, {"B", "b"}});
  REQUIRE(ranking);
  const auto calls = provider.calls.size();
  auto rewarding = reweighted(*ranking, {0, 10, 100});
  REQUIRE(rewarding);
  CHECK(ranked_ids(rewarding->ranked) == std::vector<std::string>{"A", "B"});
  CHECK(rewarding->ranked[0].score == 50);
  CHECK(provider.calls.size() == calls);
  CHECK(ranking->ranked[0].answer.score() == 1.0);
  for (const std::vector<double>& bad : {std::vector<double>{0, 1}, std::vector<double>{0, 0, 1},
                                         std::vector<double>{0, 1, std::numeric_limits<double>::infinity()}}) {
    CHECK_FALSE(reweighted(*ranking, bad));
  }
}

TEST_CASE("rerank: input checks") {
  auto provider = rerank_provider({});
  CHECK(rerank(provider, relevance, "  ", {}).error().message == "query must be a non-empty string");
  CHECK(rerank(provider, relevance, "q", {{"", "a"}}).error().message == "candidate ids must be non-empty strings");
  auto empty = rerank(provider, relevance, "q", {});
  REQUIRE(empty);
  CHECK(empty->complete());
  CHECK(empty->ranked.empty());
  CHECK(provider.calls.empty());
}

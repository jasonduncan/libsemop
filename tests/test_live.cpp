// Calls the real TypeSafe API. Skipped unless TYPESAFE_API_KEY and LIBSEMOP_LIVE=1 are set.
// Each test prints what happened, prefixed "live:". Only clear-cut judgments are asserted.
#include <cstdlib>
#include <iostream>

#include <catch2/catch_test_macros.hpp>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

using namespace semop;
namespace ts = libtypesafe;

namespace {

bool live() {
  const char* flag = std::getenv("LIBSEMOP_LIVE");
  return std::getenv("TYPESAFE_API_KEY") != nullptr && flag != nullptr && std::string(flag) == "1";
}

#define REQUIRE_LIVE() \
  if (!live()) SKIP("set TYPESAFE_API_KEY and LIBSEMOP_LIVE=1 to run")

ts::Client client() {
  ts::ClientOptions options;
  options.log_level = ts::LogLevel::warn;
  auto c = ts::Client::create(std::move(options));
  REQUIRE(c);
  return std::move(c).value();
}

const Operator complaint{"is_complaint", Boolean{"Is the customer complaining?"}};
const Operator department{"department",
                          Choice{"Which team should handle this?", {{"billing", "Payments, refunds"}, {"technical", "Bugs"}}}
                              .with_min_confidence(0.8)};
const Operator urgency{"urgency", Score{"How urgent is this?", {"low", "medium", "high"}}};
const Operator outage{"outage", Boolean{"Is a service down for the customer?"}};

Outcome<std::string> triage(Ask& ask, const std::string& message) {
  auto a = ask(message, department, urgency);
  auto team = a[department];
  if (!team) return "human";
  if (*team == "technical") {
    auto down = ask(message, outage)[outage];
    if (!down) return down.stop();
    if (*down) return "page on-call";
  }
  return *team;
}

}  // namespace

TEST_CASE("live: every question type in one call") {
  REQUIRE_LIVE();
  auto c = client();
  TypeSafe provider(c);
  auto answers = apply(provider, "I was charged twice for my subscription and I'm furious.", {complaint, department, urgency});
  if (!answers) FAIL(answers.error().message);
  const Call& call = *answers->at("department").call;
  std::cout << "live: model=" << call.model.value_or("?") << " tokens=" << call.input_tokens.value_or(-1)
            << " complaint=" << answers->at("is_complaint").probability("true").value_or(-1)
            << " department=" << answers->value(department).value_or("(undecided)")
            << " urgency=" << answers->value(urgency).value_or(-1) << '\n';
  CHECK(answers->value(complaint) == true);
  CHECK(answers->value(department) == "billing");
  CHECK(call.model.value_or("").rfind("jev-", 0) == 0);
}

TEST_CASE("live: a flow takes a second step only when needed") {
  REQUIRE_LIVE();
  auto c = client();
  TypeSafe provider(c);
  auto billing = run_flow(provider, triage, std::string("Please refund the duplicate charge on my card."));
  auto outage_case = run_flow(provider, triage, std::string("Your whole site returns 500 errors, nobody can log in."));
  std::cout << "live: flow billing=" << billing.value.value_or("(stopped)") << " calls=" << billing.calls()
            << "; outage=" << outage_case.value.value_or("(stopped)") << " calls=" << outage_case.calls() << '\n';
  CHECK(billing.value == "billing");
  CHECK(billing.calls() == 1);
  CHECK(outage_case.value == "page on-call");
  CHECK(outage_case.calls() == 2);
}

TEST_CASE("live: filter keeps the yes items, several calls in flight") {
  REQUIRE_LIVE();
  auto c = client();
  TypeSafe provider(c);
  const Boolean question = Boolean{"Is this message about money: a charge, invoice, refund, or price?"}.with_min_confidence(0.8);
  auto result = filter_items(provider, question,
                             {{"1", "I was charged twice this month."},
                              {"2", "The export button does nothing on Safari."},
                              {"3", "Can I get a refund for last year?"},
                              {"4", "How do I change my profile photo?"},
                              {"5", "Your invoice has the wrong VAT number."},
                              {"6", "Dark mode would be nice."}},
                             {nullptr, 4});
  REQUIRE(result);
  std::string matched;
  for (const auto* v : result->matched()) matched += v->id;
  std::cout << "live: filter matched=" << matched << " unsure=" << result->unsure().size()
            << " failed=" << result->failed().size() << '\n';
  CHECK(result->failed().empty());
  CHECK(matched == "135");
}

TEST_CASE("live: rerank puts the answering document first") {
  REQUIRE_LIVE();
  auto c = client();
  TypeSafe provider(c);
  const Score relevance{"How useful is the document for answering the query?",
                        {"no useful information", "on topic but doesn't answer", "partly answers", "fully answers"}};
  auto ranking = rerank(provider, relevance, "How do I reset my password?",
                        {{"pricing", "Our plans start at $9 per month; annual billing saves 20%."},
                         {"reset", "To reset your password, click 'Forgot password' on the sign-in page and follow the email link."},
                         {"login", "You can sign in with Google or with your email address."}});
  REQUIRE(ranking);
  for (const auto& r : ranking->ranked) std::cout << "live: rerank " << r.id << " " << r.score << '\n';
  REQUIRE_FALSE(ranking->ranked.empty());
  CHECK(ranking->ranked[0].id == "reset");
}

TEST_CASE("live: cascade from one model to another; time limits") {
  REQUIRE_LIVE();
  auto c = client();
  TypeSafe preview(c, "jev-preview");
  TypeSafe latest(c, "jev-latest");
  Cascade cascade({&preview, &latest}, 0.99);  // escalate almost everything
  auto answers = apply(cascade, "My card was charged twice.", {complaint, department});
  if (!answers) FAIL(answers.error().message);
  for (const auto& [name, a] : *answers) {
    std::cout << "live: cascade " << name << " answered by " << a.call->model.value_or("?") << " at "
              << a.confidence << '\n';
  }

  TimeLimited hurried(latest, std::chrono::milliseconds(1));
  auto r = ask(hurried, "My card was charged twice.", complaint);
  REQUIRE_FALSE(r);
  CHECK(r.error().kind == ErrorKind::timeout);
  std::cout << "live: " << r.error().message << '\n';
}

TEST_CASE("live: a semop wire request round trip") {
  REQUIRE_LIVE();
  auto c = client();
  TypeSafe provider(c);
  auto request = wire::decode(Json::parse(R"({"request_id": "r1", "state": "The payment failed and now I cannot sign in.",
    "questions": [{"name": "department", "type": "choice", "instructions": "Which team?",
                   "options": ["billing", "technical"], "min_confidence": 0.8}]})"));
  REQUIRE(request);
  auto answers = ask(provider, request->state, request->questions);
  REQUIRE(answers);
  const Json response = wire::encode(*answers, request->questions, request->request_id);
  std::cout << "live: wire " << response.dump() << '\n';
  CHECK(response["request_id"] == "r1");
  CHECK(response["call"]["provider"] == "TypeSafe");
}

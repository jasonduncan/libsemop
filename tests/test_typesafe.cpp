// The TypeSafe provider over a real libtypesafe::Client with a scripted transport. Offline.
#include <deque>
#include <map>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

using namespace semop;
using Catch::Matchers::ContainsSubstring;
namespace ts = libtypesafe;

namespace {

/// Records requests; answers each with the next scripted outcome (or holds when there is none).
struct ScriptedTransport : ts::Transport {
  struct Sent {
    ts::TransferId id;
    ts::HttpRequest request;
    std::function<void(ts::TransportOutcome)> done;
    bool finished = false;
    bool cancelled = false;
  };
  std::vector<Sent> sent;
  std::deque<ts::TransportOutcome> script;

  ts::TransferId start(ts::HttpRequest request, std::function<void(ts::TransportOutcome)> done) override {
    sent.push_back({static_cast<ts::TransferId>(sent.size() + 1), std::move(request), std::move(done)});
    return sent.back().id;
  }
  void cancel(ts::TransferId id) override {
    for (auto& s : sent) {
      if (s.id == id && !s.finished) {
        s.finished = s.cancelled = true;
        s.done(ts::TransportError{ts::TransportError::Kind::cancelled, "cancelled", true});
      }
    }
  }
  void poll() override {
    for (auto& s : sent) {
      if (s.finished || script.empty()) continue;
      s.finished = true;
      auto outcome = std::move(script.front());
      script.pop_front();
      s.done(std::move(outcome));
    }
  }
};

ts::HttpResponse ok(const Json& body) {
  return {200, {{"Content-Type", "application/json"}, {"x-typesafe-request-id", "req_1"}}, body.dump()};
}

struct Fixture {
  std::shared_ptr<ScriptedTransport> transport = std::make_shared<ScriptedTransport>();
  ts::Client client = make();

  ts::Client make() {
    ts::ClientOptions options;
    options.api_key = "test-key-123456";
    options.transport = transport;
    options.log_level = ts::LogLevel::off;
    options.retry.max_retries = 0;
    return ts::Client::create(std::move(options)).value();
  }
};

const Operator complaint{"is_complaint", Boolean{"Is the customer complaining?", std::string("They report a problem."), std::nullopt}};
const Operator department{"department", Choice{"Which team?", {{"billing", "Payments, refunds"}, {"technical"}}}};
const Operator urgency{"urgency", Score{"How urgent?", {"low", "medium", "high"}}};

const Json good_body = Json::parse(R"({
  "model": "jev-1.13.0",
  "answers": {
    "is_complaint": {"type": "noul", "noul": 0.07},
    "department": {"type": "choice", "choice": "billing", "confidence": 0.97,
                   "probabilities": {"technical": 0.03, "billing": 0.97}},
    "urgency": {"type": "score", "score": 1.26, "confidence": 0.67,
                "legend": {"0": "low", "1": "medium", "2": "high"},
                "probabilities": {"0": 0.04, "1": 0.66, "2": 0.3}}
  },
  "usage": {"input_tokens": 291, "output_tokens": 20}
})");

}  // namespace

TEST_CASE("questions are sent exactly as the Python provider sends them") {
  CHECK(typesafe_question(complaint.question).dump() ==
        R"({"type":"noul","instructions":"Is the customer complaining?","criteria":{"true":"They report a problem.","false":null}})");
  CHECK(typesafe_question(Boolean{"Is it?"}).dump() == R"({"type":"noul","instructions":"Is it?"})");
  CHECK(typesafe_question(department.question).dump() ==
        R"({"type":"choice","instructions":"Which team?","criteria":{"billing":"Payments, refunds","technical":null}})");
  CHECK(typesafe_question(urgency.question).dump() ==
        R"({"type":"score","instructions":"How urgent?","criteria":["low","medium","high"]})");
}

TEST_CASE("one call, typed answers, and the model that answered") {
  Fixture f;
  TypeSafe provider(f.client, "jev-1.13.0");
  f.transport->script.push_back(ok(good_body));
  auto answers = apply(provider, "I was charged twice and I'm furious.", {complaint, department, urgency});
  REQUIRE(answers);
  REQUIRE(f.transport->sent.size() == 1);
  const Json body = Json::parse(f.transport->sent[0].request.body);
  CHECK(body["model"] == "jev-1.13.0");
  CHECK(body["state"] == "I was charged twice and I'm furious.");
  CHECK(body["questions"]["urgency"]["criteria"] == Json{"low", "medium", "high"});

  // p(true) 0.07: false, with p(false) exactly 0.93.
  CHECK(answers->value(complaint) == false);
  CHECK(answers->at("is_complaint").probability("false") == 0.93);
  CHECK(answers->value(department) == "billing");
  CHECK(answers->at("department").probabilities[0].first == "billing");  // question order
  CHECK(answers->value(urgency) == 1.26);
  CHECK(answers->at("urgency").probability("medium") == 0.66);
  const auto& call = *answers->at("urgency").call;
  CHECK(call.provider == "TypeSafe");
  CHECK(call.model == "jev-1.13.0");
  CHECK(call.input_tokens == 291);
  CHECK(answers->at("department").raw["confidence"] == 0.97);
}

TEST_CASE("the model defaults to jev-latest") {
  Fixture f;
  TypeSafe provider(f.client);
  f.transport->script.push_back(ok(good_body));
  REQUIRE(ask(provider, "hi", urgency));
  CHECK(Json::parse(f.transport->sent[0].request.body)["model"] == "jev-latest");
}

TEST_CASE("failures are provider errors named TypeSafe") {
  SECTION("HTTP 401") {
    Fixture f;
    TypeSafe provider(f.client);
    f.transport->script.push_back(ts::HttpResponse{401, {}, R"({"detail": "Cannot authenticate"})"});
    auto r = ask(provider, "hi", urgency);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ErrorKind::provider_error);
    CHECK(r.error().provider == "TypeSafe");
    CHECK_THAT(r.error().message, ContainsSubstring("TypeSafe: GET") || ContainsSubstring("TypeSafe: POST"));
    CHECK_THAT(r.error().message, ContainsSubstring("401 Cannot authenticate"));
  }
  SECTION("timeout") {
    Fixture f;
    TypeSafe provider(f.client);
    f.transport->script.push_back(ts::TransportError{ts::TransportError::Kind::timeout, "slow", true});
    auto r = ask(provider, "hi", urgency);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ErrorKind::timeout);
    CHECK(r.error().is_provider_error());
  }
  SECTION("a malformed answer never looks confident") {
    Fixture f;
    TypeSafe provider(f.client);
    Json body = good_body;
    body["answers"]["urgency"]["score"] = 2.9;  // disagrees with its probabilities
    f.transport->script.push_back(ok(body));
    auto r = ask(provider, "hi", urgency);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "TypeSafe: unexpected response: Score value 2.9 is outside 0..2");
  }
}

TEST_CASE("cancel and destruction cancel the HTTP call") {
  Fixture f;
  {
    TypeSafe provider(f.client);
    bool called = false;
    const Ticket t = provider.start("hi", Questions{urgency}, [&](Result<Answers>) { called = true; });
    provider.cancel(t);
    f.client.pump();
    CHECK(f.transport->sent[0].cancelled);
    provider.start("again", Questions{urgency}, [&](Result<Answers>) { called = true; });
    CHECK_FALSE(called);
  }  // destroyed with a call in flight
  f.client.pump();
  CHECK(f.transport->sent[1].cancelled);
}

// ---------------------------------------------------------------------------
// Images and Box
// ---------------------------------------------------------------------------

#include "imagekit.hpp"

TEST_CASE("images go in assets with the images header; Box answers are decoded") {
  Fixture f;
  TypeSafe provider(f.client, "jev-1.14.0");
  auto photo = Image::from_bytes(imagekit::png()).value();
  auto logo = Image::from_bytes(imagekit::png(300, 300)).value();
  provider.attach(photo);
  provider.attach(logo);
  auto unused = Image::from_bytes(imagekit::png()).value();
  provider.attach(unused);

  f.transport->script.push_back(ok(Json::parse(R"({
    "model": "jev-1.14.0",
    "answers": {
      "mug": {"type": "box", "box": {"top": 0.25, "bottom": 0.82, "left": 0.44, "right": 0.84}, "confidence": 0.92},
      "brand": {"type": "choice", "choice": "a", "confidence": 0.8, "probabilities": {"a": 0.8, "b": 0.2}}
    },
    "usage": {"input_tokens": 500, "output_tokens": 30}
  })")));
  auto answers = ask(provider, Json{{"photo", photo.placeholder()}},
                     Questions{{"mug", Box{"the coffee mug in `photo`"}},
                               {"brand", Choice{"Which brand?", {{"a", "this logo: " + logo.placeholder()}, {"b", "other"}}}}});
  REQUIRE(answers);
  CHECK(answers->at("mug").region() == Region{0.25, 0.82, 0.44, 0.84});
  CHECK(answers->at("mug").confidence == 0.92);
  CHECK(answers->value(Operator{"mug", Box{"x"}}) == Region{0.25, 0.82, 0.44, 0.84});

  const auto& request = f.transport->sent.at(0).request;
  CHECK(request.headers.get("TypeSafe-Danger-Zone-Images") == "true");
  const std::string type(*request.headers.get("Content-Type"));
  REQUIRE(type.rfind("multipart/form-data; boundary=", 0) == 0);
  const std::string boundary = "--" + type.substr(type.find('=') + 1);
  // Parts by name: the JSON request, then the two images (the unattached-but-unused one isn't sent).
  std::map<std::string, std::string> parts;
  for (std::size_t at = request.body.find(boundary); request.body.compare(at + boundary.size(), 2, "--") != 0;) {
    const std::size_t next = request.body.find(boundary, at + 1);
    const std::string part = request.body.substr(at + boundary.size() + 2, next - at - boundary.size() - 4);
    const std::size_t n = part.find("name=\"") + 6;
    parts[part.substr(n, part.find('"', n) - n)] = part.substr(part.find("\r\n\r\n") + 4);
    at = next;
  }
  REQUIRE(parts.size() == 3);
  // Byte-identical to the Python provider's request part (typesafe-sdk 0.7.5) for the same call.
  std::string expected =
      R"({"state":{"photo":"P"},"model":"jev-1.14.0","questions":{"mug":{"type":"box","instructions":"the coffee mug in `photo`"},"brand":{"type":"choice","instructions":"Which brand?","criteria":{"a":"this logo: L","b":"other"}}},"assets":{"P":{"type":"image","part":"asset-0"},"L":{"type":"image","part":"asset-1"}}})";
  for (const auto& [from, to] : {std::pair{std::string("\"P\""), "\"" + photo.placeholder() + "\""},
                                 std::pair{std::string(": L\""), ": " + logo.placeholder() + "\""},
                                 std::pair{std::string("\"L\""), "\"" + logo.placeholder() + "\""}}) {
    for (std::size_t at; (at = expected.find(from)) != std::string::npos;) expected.replace(at, from.size(), to);
  }
  CHECK(parts["request"] == expected);
  CHECK(parts["asset-0"] == std::string(photo.bytes().begin(), photo.bytes().end()));
  CHECK(parts["asset-1"] == std::string(logo.bytes().begin(), logo.bytes().end()));
}

TEST_CASE("text-only calls are unchanged") {
  Fixture f;
  TypeSafe provider(f.client);
  f.transport->script.push_back(ok(good_body));
  REQUIRE(apply(provider, "hello", {complaint, department, urgency}));
  const auto& request = f.transport->sent.at(0).request;
  CHECK_FALSE(request.headers.contains("TypeSafe-Danger-Zone-Images"));
  CHECK_FALSE(Json::parse(request.body).contains("assets"));
}

TEST_CASE("an unattached placeholder fails before sending") {
  Fixture f;
  TypeSafe provider(f.client, "jev-1.14.0");
  auto photo = Image::from_bytes(imagekit::png()).value();
  auto answers = ask(provider, Json{{"photo", photo.placeholder()}}, Questions{{"q", Boolean{"Is it red?"}}});
  REQUIRE_FALSE(answers);
  CHECK(answers.error().kind == ErrorKind::invalid_argument);
  CHECK_THAT(answers.error().message, ContainsSubstring("attach"));
  CHECK(f.transport->sent.empty());
}

TEST_CASE("a malformed box answer is a provider error") {
  Fixture f;
  TypeSafe provider(f.client, "jev-1.14.0");
  f.transport->script.push_back(ok(Json::parse(R"({"model": "jev-1.14.0",
    "answers": {"mug": {"type": "box", "box": {"top": 0.9, "bottom": 0.1, "left": 0, "right": 1}, "confidence": 0.9}}})")));
  auto answers = ask(provider, "x", Questions{{"mug", Box{"the mug"}}});
  REQUIRE_FALSE(answers);
  CHECK(answers.error().kind == ErrorKind::provider_error);
}

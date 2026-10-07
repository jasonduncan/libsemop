// The semop JSON request/response, offline. Ports the wire tests of tests/test_interfaces.py.
#include <functional>

#include <catch2/catch_test_macros.hpp>

#include "scripted.hpp"

using namespace semop;

namespace {

const Json request = Json::parse(R"({
  "schema_version": "1",
  "request_id": "ticket-42",
  "state": "The payment failed and now I cannot sign in.",
  "questions": [
    {"name": "is_complaint", "type": "boolean", "instructions": "Is it a complaint?", "min_confidence": 0.8},
    {"name": "department", "type": "choice", "instructions": "Which team?",
     "options": [{"name": "billing", "description": "Charges"}, "technical"], "min_confidence": 0.8},
    {"name": "urgency", "type": "score", "instructions": "How urgent?", "levels": ["low", "medium", "high"]}
  ]
})");

/// Sure about Booleans and Scores, split 55/45 on Choices (as the Python test's Fake).
Answers fake_answers(const Questions& questions) {
  auto call = std::make_shared<const Call>(Call{"Fake", "fake-1", 12, std::nullopt});
  Answers answers;
  for (const auto& [name, q] : questions) {
    if (std::holds_alternative<Boolean>(q)) {
      answers.add(name, make_answer(q, true, {{"true", 0.9}, {"false", 0.1}}, nullptr, call).value());
    } else if (const auto* c = std::get_if<Choice>(&q)) {
      std::vector<std::pair<std::string, double>> ps{{c->options[0].name, 0.55}};
      for (std::size_t i = 1; i < c->options.size(); ++i) ps.emplace_back(c->options[i].name, 0.45 / double(c->options.size() - 1));
      answers.add(name, make_answer(q, c->options[0].name, ps, nullptr, call).value());
    } else {
      const auto& levels = std::get<Score>(q).levels;
      answers.add(name, make_answer(q, 1.6, {{levels[0], 0.1}, {levels[1], 0.2}, {levels[2], 0.7}}, nullptr, call).value());
    }
  }
  return answers;
}

}  // namespace

TEST_CASE("decode builds our question types in order") {
  auto decoded = wire::decode(request);
  REQUIRE(decoded);
  CHECK(decoded->request_id == "ticket-42");
  CHECK(decoded->state == "The payment failed and now I cannot sign in.");
  REQUIRE(decoded->questions.size() == 3);
  CHECK(decoded->questions[0].name == "is_complaint");
  const auto& department = std::get<Choice>(decoded->questions[1].question);
  CHECK(department.min_confidence == 0.8);
  CHECK(department.options[0].name == "billing");
  CHECK(department.options[0].description == "Charges");
  CHECK_FALSE(department.options[1].description);
  CHECK(std::get<Score>(decoded->questions[2].question).levels == std::vector<std::string>{"low", "medium", "high"});
}

TEST_CASE("decode rejects malformed requests with a path") {
  const std::vector<std::pair<std::function<void(Json&)>, std::string>> cases = {
      {[](Json& r) { r["schema_version"] = "2"; }, "schema_version"},
      {[](Json& r) { r.erase("state"); }, "state"},
      {[](Json& r) { r["state"] = 5; }, "state"},
      {[](Json& r) { r["questions"] = Json::array(); }, "questions"},
      {[](Json& r) { r["extra"] = 1; }, "extra"},
      {[](Json& r) { r["questions"][0]["type"] = "rank"; }, "questions[0].type"},
      {[](Json& r) { r["questions"][0]["options"] = {"a", "b"}; }, "questions[0].options"},
      {[](Json& r) { r["questions"][1]["options"] = {"only"}; }, "questions[1]"},
      {[](Json& r) { r["questions"][1]["options"] = {"a", "a"}; }, "questions[1].options[1]"},
      {[](Json& r) { r["questions"][2]["name"] = "is_complaint"; }, "questions[2].name"},
      {[](Json& r) { r["questions"][2]["levels"] = {"x", "x"}; }, "questions[2]"},
      {[](Json& r) { r["questions"][0]["min_confidence"] = 2; }, "questions[0]"},
  };
  for (const auto& [change, path] : cases) {
    Json r = request;
    change(r);
    auto decoded = wire::decode(r);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().kind == ErrorKind::invalid_request);
    CHECK(decoded.error().path == path);
  }
}

TEST_CASE("decode messages match the Python implementation") {
  const auto message = [](const Json& r) { return wire::decode(r).error().message; };
  CHECK(message(Json::array()) == "the request must be a JSON object");
  CHECK(message({{"state", "x"}, {"questions", {{{"name", "q"}, {"type", "boolean"}, {"instructions", "?"}, {"min_confidence", "0.8"}}}}}) ==
        "questions[0]: min_confidence must be a number from 0 to 1, got '0.8'");
  CHECK(message({{"state", "x"}, {"questions", {{{"name", "q"}, {"type", "choice"}, {"instructions", "?"}, {"options", {"a", "a"}}}}}}) ==
        "questions[0].options[1]: option 'a' is used twice");
  CHECK(message({{"state", "x"}, {"questions", {{{"name", "q"}, {"type", "boolean"}, {"instructions", "?"}, {"typo", 1}, {"zzz", 2}}}}}) ==
        "questions[0].typo: unknown field");
}

TEST_CASE("encode keeps undecided as null, rounds numbers, and names the call") {
  auto decoded = wire::decode(request);
  REQUIRE(decoded);
  const Json response = wire::encode(fake_answers(decoded->questions), decoded->questions, decoded->request_id);
  CHECK(response["request_id"] == "ticket-42");
  CHECK(response["answers"]["department"] == Json::parse(R"({"type": "choice", "value": null, "decided": false,
      "confidence": 0.55, "probabilities": {"billing": 0.55, "technical": 0.45}})"));
  CHECK(response["answers"]["urgency"]["value"] == 1.6);
  CHECK(response["call"] == Json::parse(R"({"provider": "Fake", "model": "fake-1", "input_tokens": 12, "output_tokens": null})"));
  CHECK(response.dump().find("raw") == std::string::npos);
}

TEST_CASE("numbers are rounded to 4 significant digits, like Python") {
  CHECK(wire::significant(0.123456) == 0.1235);
  CHECK(wire::significant(1.0 / 3) == 0.3333);
  CHECK(wire::significant(1 - 0.07) == 0.93);
  CHECK(wire::significant(0.0) == 0.0);
  CHECK(wire::significant(12345.6) == 12350.0);
}

TEST_CASE("errors and codes") {
  CHECK(wire::error("timeout", "slow", std::string("r1")) ==
        Json::parse(R"({"request_id": "r1", "error": {"code": "timeout", "message": "slow"}})"));
  Error e;
  e.kind = ErrorKind::invalid_question;
  CHECK(wire::code(e) == "invalid_request");
  e.kind = ErrorKind::provider_error;
  CHECK(wire::code(e) == "provider_error");
  e.kind = ErrorKind::timeout;
  CHECK(wire::code(e) == "timeout");
}

// A minimal `semop ask`: read the semop JSON request on stdin, print the response.
//   echo '{"state": "...", "questions": [...]}' | example_ask
// Exit codes as semop's: 0 answered (including "don't know"), 1 provider failed, 2 invalid request.
// Needs TYPESAFE_API_KEY.
#include <iostream>
#include <iterator>
#include <string>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

int main() {
  const std::string text((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const semop::Json parsed = semop::Json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    std::cout << semop::wire::error("invalid_request", "the request is not valid JSON").dump() << '\n';
    return 2;
  }
  auto request = semop::wire::decode(parsed);
  if (!request) {
    std::cout << semop::wire::error("invalid_request", request.error().message).dump() << '\n';
    return 2;
  }
  auto client = libtypesafe::Client::create();
  if (!client) {
    std::cout << semop::wire::error("invalid_request", client.error().message, request->request_id).dump() << '\n';
    return 2;
  }
  semop::TypeSafe provider(*client);
  auto answers = semop::ask(provider, request->state, request->questions);
  if (!answers) {
    const auto& e = answers.error();
    std::cout << semop::wire::error(semop::wire::code(e), e.message, request->request_id).dump() << '\n';
    return e.is_provider_error() ? 1 : 2;
  }
  std::cout << semop::wire::encode(*answers, request->questions, request->request_id).dump(2) << '\n';
}

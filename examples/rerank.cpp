// Rerank retrieved documents by relevance, then re-sort by custom level weights without new calls.
// Needs TYPESAFE_API_KEY.
#include <iostream>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

int main() {
  auto client = libtypesafe::Client::create();
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 1;
  }
  semop::TypeSafe provider(*client);
  const semop::Score relevance{"How useful is the document for answering the query?",
                               {"no useful information", "on topic but doesn't answer", "partly answers",
                                "answers with minor gaps", "fully answers"}};

  auto ranking = semop::rerank(provider, relevance, "How do I reset my password?",
                               {{"pricing", "Plans start at $9 per month; annual billing saves 20%."},
                                {"login", "You can sign in with Google or with your email address."},
                                {"reset", "Click 'Forgot password' on the sign-in page and follow the emailed link."},
                                {"2fa", "Two-factor codes can be reset by support after identity checks."}});
  if (!ranking) {
    std::cerr << ranking.error().message << '\n';
    return 1;
  }
  auto top = ranking->top(3, /*allow_partial=*/true);
  if (!top) {
    std::cerr << top.error().message << '\n';
    return 1;
  }
  for (const auto& r : *top) std::cout << r.id << "  " << r.score << '\n';
  for (const auto& u : ranking->unscored) std::cout << u.id << "  (unscored)\n";

  auto strict = semop::reweighted(*ranking, {0, 1, 5, 20, 100});  // reward only real answers
  if (strict) std::cout << "reweighted first: " << strict->ranked.front().id << '\n';
}

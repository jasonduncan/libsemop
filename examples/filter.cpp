// A tiny semfilter: print the stdin lines the model says yes to, 8 calls in flight.
//   ls ~/Downloads | example_filter "Is this an installer or other throwaway download?"
// Needs TYPESAFE_API_KEY.
#include <iostream>
#include <string>

#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: example_filter \"yes/no question\" < items\n";
    return 2;
  }
  std::vector<std::pair<std::string, semop::Json>> items;
  for (std::string line; std::getline(std::cin, line);) {
    if (!line.empty()) items.emplace_back(std::to_string(items.size()), line);
  }
  auto client = libtypesafe::Client::create();
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 2;
  }
  semop::TypeSafe provider(*client);
  auto result = semop::filter_items(provider, semop::Boolean{argv[1]}.with_min_confidence(0.8), items);
  if (!result) {
    std::cerr << result.error().message << '\n';
    return 2;
  }
  for (const auto& v : result->verdicts) {
    const std::string& text = items[std::stoul(v.id)].second.get_ref<const std::string&>();
    if (v.outcome == semop::Verdict::Outcome::match) std::cout << text << '\n';
    if (v.outcome == semop::Verdict::Outcome::unsure) std::cerr << "? " << text << '\n';
    if (v.outcome == semop::Verdict::Outcome::failed) std::cerr << "! " << text << ": " << v.error->message << '\n';
  }
  return result->matched().empty() ? 1 : result->failed().empty() ? 0 : 3;
}

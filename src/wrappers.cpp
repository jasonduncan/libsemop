#include "semop/wrappers.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "util.hpp"

namespace semop {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// TimeLimited
// ---------------------------------------------------------------------------

TimeLimited::TimeLimited(Provider& inner, std::chrono::milliseconds limit)
    : Provider(inner.name()), inner_(inner), limit_(limit) {}

TimeLimited::~TimeLimited() {
  for (const auto& [ticket, running] : running_) inner_.cancel(running.inner);
}

void TimeLimited::start_call(Ticket ticket, const Json& state, const Questions& questions) {
  if (limit_.count() <= 0) {
    finish(ticket, detail::invalid(ErrorKind::invalid_argument, "the time limit must be positive"));
    return;
  }
  const Ticket inner = inner_.start(state, questions, [this, ticket](Result<Answers> result) {
    running_.erase(ticket);
    finish(ticket, std::move(result));
  });
  running_[ticket] = Running{inner, now() + limit_};
}

void TimeLimited::cancel_call(Ticket ticket) {
  const auto it = running_.find(ticket);
  if (it == running_.end()) return;
  inner_.cancel(it->second.inner);
  running_.erase(it);
}

void TimeLimited::on_pump() {
  inner_.pump();
  const auto t = now();
  for (auto it = running_.begin(); it != running_.end();) {
    if (t < it->second.deadline) {
      ++it;
      continue;
    }
    inner_.cancel(it->second.inner);
    Error error = provider_error("no answer within " + std::to_string(limit_.count()) + "ms");
    error.kind = ErrorKind::timeout;
    finish(it->first, std::move(error));
    it = running_.erase(it);
  }
}

void TimeLimited::wait(std::chrono::milliseconds max_wait) {
  auto wait = max_wait;
  const auto t = now();
  for (const auto& [ticket, running] : running_) {
    wait = std::min(wait, std::max(std::chrono::milliseconds(0),
                                   std::chrono::duration_cast<std::chrono::milliseconds>(running.deadline - t)));
  }
  inner_.wait(wait);
}

// ---------------------------------------------------------------------------
// Cascade
// ---------------------------------------------------------------------------

Cascade::Cascade(std::vector<Provider*> providers, double escalate_below)
    : Provider("Cascade"), providers_(std::move(providers)), escalate_below_(escalate_below) {
  if (providers_.size() < 2) {
    invalid_ = detail::invalid(ErrorKind::invalid_argument, "cascade needs at least two providers");
  } else if (std::any_of(providers_.begin(), providers_.end(), [](Provider* p) { return p == nullptr; })) {
    invalid_ = detail::invalid(ErrorKind::invalid_argument, "cascade providers must not be null");
  } else if (!std::isfinite(escalate_below) || escalate_below < 0 || escalate_below > 1) {
    invalid_ = detail::invalid(ErrorKind::invalid_argument,
                               "escalate_below must be a number from 0 to 1, got " + detail::repr(escalate_below));
  }
}

Cascade::~Cascade() {
  for (const auto& [ticket, running] : running_) providers_[running.provider]->cancel(running.inner);
}

void Cascade::start_call(Ticket ticket, const Json& state, const Questions& questions) {
  if (invalid_) {
    finish(ticket, *invalid_);
    return;
  }
  Running& running = running_[ticket];
  running.state = state;
  running.questions = questions;
  ask_next(ticket, running, questions);
}

void Cascade::ask_next(Ticket ticket, Running& running, const Questions& questions) {
  running.inner = providers_[running.provider]->start(
      running.state, questions, [this, ticket](Result<Answers> result) { on_answers(ticket, std::move(result)); });
}

void Cascade::on_answers(Ticket ticket, Result<Answers> result) {
  const auto it = running_.find(ticket);
  if (it == running_.end()) return;
  Running& running = it->second;
  if (!result) {  // a failure is the result: escalating around it would hide an outage
    finish(ticket, result.error());
    running_.erase(it);
    return;
  }

  // Merge, then find what this provider was unsure of among the questions it was asked.
  Answers merged;
  for (const auto& [name, question] : running.questions) {
    if (const Answer* fresh = result->find(name)) {
      merged.add(name, *fresh);
    } else if (const Answer* earlier = running.answers.find(name)) {
      merged.add(name, *earlier);
    }
  }
  running.answers = std::move(merged);

  Questions unsure;
  for (const auto& nq : running.questions) {
    if (result->find(nq.name) == nullptr) continue;
    const Answer& answer = running.answers.at(nq.name);
    if (!answer.decided() || answer.confidence < escalate_below_) unsure.push_back(nq);
  }
  if (unsure.empty() || running.provider + 1 == providers_.size()) {
    finish(ticket, std::move(running.answers));
    running_.erase(it);
    return;
  }
  ++running.provider;
  ask_next(ticket, running, unsure);
}

void Cascade::cancel_call(Ticket ticket) {
  const auto it = running_.find(ticket);
  if (it == running_.end()) return;
  providers_[it->second.provider]->cancel(it->second.inner);
  running_.erase(it);
}

void Cascade::on_pump() {
  std::set<Provider*> pumped;
  for (Provider* provider : providers_) {
    if (provider != nullptr && pumped.insert(provider).second) provider->pump();
  }
}

void Cascade::wait(std::chrono::milliseconds max_wait) {
  for (Provider* provider : providers_) {
    if (provider != nullptr && provider->in_flight() > 0) {
      provider->wait(max_wait);
      return;
    }
  }
  Provider::wait(max_wait);
}

}  // namespace semop

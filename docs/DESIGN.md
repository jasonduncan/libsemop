# libsemop: design

A C++17 port of [semantic-operators-lib](https://github.com/jasonduncan/semantic-operators)
0.11.0 (Python): write a semantic judgment once, run it on any System One model, and get a
"don't know" instead of a guess when the model isn't sure. It is built on
[libtypesafe](https://github.com/jasonduncan/libtypesafe) for the TypeSafe provider.

Status: **0.1.0 built and verified, 2026-09-25; pushed to the private repo `jasonduncan/libsemop` 2026-10-07; not released.** Built
autonomously while Jason was away. Every judgment call is listed in §1 for review; what was
verified, and how, is in §6.

## 1. Decisions to verify

| # | Decision | Why | Alternative |
|---|---|---|---|
| 1 | Port `semantic-operators-lib` 0.11.0, not the legacy `semantic-operators` 1.0.0 | You said the lib is current and the 1.0.0 repo is legacy | — |
| 2 | Project `libsemop`, namespace `semop`, headers `<semop/...>`, CMake targets `libsemop::semop` (core) and `libsemop::typesafe` | Matches the libtypesafe naming; `semop` is your own name, so a short namespace is safe | Namespace `libsemop` for symmetry with libtypesafe |
| 3 | A separate repo at `~/Development/Projects/libsemop`, next to libtypesafe. **Resolved 2026-10-07:** Jason created the private repo `jasonduncan/libsemop` | Pushing to a new public repo is yours to decide | — |
| 4 | The core depends only on nlohmann_json; the TypeSafe provider is a separate target that needs libtypesafe ≥ 0.1 (`find_package`, else the v0.1.0 GitHub release is fetched) | Mirrors Python: the core has no provider dependencies, and each provider is optional | One library that always links libtypesafe |
| 5 | Same conventions as libtypesafe: C++17, never throws, `Result<T>` / `Error`, built without exceptions or RTTI | Same users, same hosts | Exceptions, mirroring Python's `ProviderError` |
| 6 | Providers are non-blocking: `start()` / `pump()` / `cancel()`, with blocking helpers (`ask`, `apply`, `filter_items`, `rerank`, flows, bench) on top | Works from a game or UI loop like libtypesafe, and gives concurrency without threads | Blocking-only providers with a thread pool for concurrency |
| 7 | Questions are plain structs validated when used (every entry point returns `invalid_question`), plus `semop::check()` for eager validation | Without exceptions, constructors can't fail; plain structs keep designated initializers | Factory functions returning `Result<Question>` |
| 8 | Typed operators: `Operator<Boolean>` → `bool`, `Operator<Choice>` → `std::string`, `Operator<Score>` → `double`, `Operator<EnumChoice<E>>` → `E` | The C++ value-add, like libtypesafe's typed keys; the dynamic `Answer` is still there | Dynamic values only, like Python |
| 9 | Flows are exception-free: reading an operator gives a `Value<T>`; `return v.stop();` ends the flow as "don't know"; a provider error ends it with `error` whatever the flow returns | Reproduces Python's `Undecided` and propagation semantics without exceptions | A `SEMOP_TRY` macro, or requiring exceptions |
| 10 | Flows are blocking only in 0.1 | Non-blocking flows need C++20 coroutines or hand-written state machines | Coroutine flows (C++20) later |
| 11 | `filter_items` and `rerank` keep up to 8 calls in flight by default (`concurrency`); `bench::run` is sequential by default | Python's async versions default to 8; C++ gets concurrency without an async twin | Sequential by default, like Python's sync API |
| 12 | `make_answer` rules are identical to Python's: rounding margin 0.005 per outcome, ties undecided, `min_confidence`, `complement()` rounded to 10 places | Answers must mean the same thing in both languages | — |
| 13 | The `semop` JSON request/response format (schema_version "1") lives in the library (`semop/wire.hpp`) | The C++ `semop` CLI (next) and the MCP server need it, and it enables cross-language parity tests | Keep it in the CLI project |
| 14 | The TypeSafe provider sends questions exactly as the Python provider does (it uses `add_raw`, so a half-described Boolean still sends `"false": null`) | Byte-identical requests across languages, checked against Python | libtypesafe's typed builders, which omit the null side |
| 15 | `TypeSafe::wait()` sleeps at most 1 ms per loop, because `libtypesafe::Client` has no public `wait()` | A blocking helper waits at most 1 ms extra per poll; no change to libtypesafe needed | Add `Client::wait()` to libtypesafe 0.2 (a follow-up) |
| 16 | Not ported: Laya (Python/torch), the CLI and MCP server (next project), async twins, call-time choices (not in Python yet), `Rank` | Out of scope for a library port, or not in Python either | — |
| 17 | Version 0.1.0; MIT, same copyright as libtypesafe | Matches libtypesafe | — |
| 18 | The library builds **without exceptions but with RTTI**, unlike libtypesafe | RTTI in a library never hurts callers built without it, and avoids libtypesafe's two workarounds (`transport.cpp` built with RTTI; `-fno-sanitize=vptr` in UBSan) | Match libtypesafe exactly |
| 19 | Examples use forced choices only with an escape option (`other`) | A live run routed "Hi, just saying hello." to `technical` when the only options were billing/technical: the model must pick one | — |
| 20 | `ask_each` (one question, many states, concurrent) is public | Filtering and reranking use it, and a CLI's per-item commands need it; Python keeps it private | Keep it internal |

## 2. What maps to what

| Python (`semantic_operators`) | C++ (`semop`) |
|---|---|
| `Boolean`, `Choice`, `Score` (validate on construction) | same structs; `check()` or validation at use |
| `Answer`, `Call`, `make_answer`, `complement` | same; `Answer::value` is an `AnswerValue`: `std::variant<std::monostate, bool, std::string, double>` |
| `Provider` / `AsyncProvider` protocols | `Provider` base class: `start` / `pump` / `cancel` / `wait`; `BlockingProvider`, `FunctionProvider` |
| `provider.ask(state, {name: q})` | `semop::ask(provider, state, questions)` |
| `Operator`, `op(provider, state)`, `apply` | `Operator<Q>`, `semop::ask(provider, state, op)`, `semop::apply(provider, state, {ops})` |
| `ProviderError`, `ProviderTimeout`, `ValueError` | `Error` with `ErrorKind::provider_error`, `timeout`, `invalid_question`, `invalid_argument`, `invalid_request` |
| `with_timeout(provider, s)` | `semop::TimeLimited(provider, ms)` |
| `cascade(p1, p2, escalate_below=)` | `semop::Cascade({&p1, &p2}, escalate_below)` |
| `flows.flow`, `Undecided`, `FlowResult` | `semop::run_flow(provider, fn, args...)`, `Value<T>::stop()`, `FlowResult<T>` |
| `filtering.filter_items` | `semop::filter_items` |
| `rerank`, `reweighted`, `Ranking.top` | same names |
| `bench.score/run/at_min_confidence/ndcg/stability/run_flow` | `semop::bench::` same names |
| `interfaces/wire.py` | `semop::wire::decode/encode/error` |
| `providers/typesafe.py` | `semop::TypeSafe` (target `libsemop::typesafe`) |

## 3. The provider contract

```cpp
class Provider {
 public:
  Ticket start(Json state, Questions questions, Done done);  // never blocks, never calls done itself
  void cancel(Ticket);                                        // done is not called
  void pump();                                                // progress + callbacks; never blocks*
  virtual void wait(std::chrono::milliseconds max_wait);      // for blocking helpers
 protected:
  virtual void start_call(Ticket, const Json& state, const Questions&) = 0;
  virtual void cancel_call(Ticket);
  virtual void on_pump();
  void finish(Ticket, Result<Answers>);                       // any thread; delivered in pump()
};
```

- **The base class** validates the questions (so `start_call` only sees valid ones), numbers
  the calls, and delivers each result once, inside `pump()`, on the calling thread. It also
  checks that a provider answered exactly the questions asked, and turns a gap into a
  `provider_error`.
- \* `BlockingProvider` (e.g. `FunctionProvider`, or a local model) runs its calls inside
  `pump()`, so its `pump()` blocks for as long as a call takes.
- **Blocking helpers** refuse to run inside a callback (`invalid_argument`) rather than
  deadlock.
- **Wrappers:** `TimeLimited` and `Cascade` are providers themselves, so everything above
  works with them unchanged, as in Python.

## 4. Flows without exceptions

```cpp
semop::Outcome<std::string> triage(semop::Ask& ask, const std::string& message) {
  auto a = ask(message, department, urgency);    // one call, two operators
  auto team = a[department];                     // Value<std::string>
  if (!team) return "human";                     // undecided: a person decides (handled)
  if (*team == "technical") {
    auto down = ask(message, outage)[outage];
    if (!down) return down.stop();               // "don't know" ends the flow here
    if (*down) return "page on-call";
  }
  return *team;
}

auto result = semop::run_flow(provider, triage, message);
result.value, result.decided(), result.stopped_at, result.error, result.trace
```

A provider error makes every later `ask` fail without calling the provider, and the
`FlowResult` carries the error whatever the flow returned. That is how Python behaves when
the exception propagates.

## 5. Build

- **Targets:** `libsemop::semop` (core; nlohmann_json only) and `libsemop::typesafe`
  (`LIBSEMOP_WITH_TYPESAFE`, on by default).
- **libtypesafe** comes from `find_package(libtypesafe 0.1)`, or else the v0.1.0 release is
  fetched from GitHub. To develop against a local checkout, pass
  `-DFETCHCONTENT_SOURCE_DIR_LIBTYPESAFE=<path>`.
- **Static library:** built without exceptions or RTTI, like libtypesafe (`provider.cpp`
  keeps RTTI so callers can subclass `Provider`).

## 6. What was verified (2026-09-25)

| Check | Result |
|---|---|
| Offline tests (`ctest -LE live`), macOS Apple clang 21 | 63 test cases + 2 package checks pass; 0 warnings |
| Same, Linux (Docker `gcc:14`), GCC 14.4 | 65/65 pass; 0 warnings |
| ASan + UBSan, and TSan (macOS) | both test binaries clean (383 assertions each) |
| Strict build: examples compiled with `-fno-exceptions -fno-rtti` | compiles |
| Package: `find_package(libsemop)` from an install, and `add_subdirectory` | both pass; the install includes libtypesafe and nlohmann_json when they were fetched |
| libtypesafe v0.1.0 fetched from the public GitHub release | builds and links |
| Live suite (6 cases, jev-1.13.0) | pass: all question types, a two-step flow, filter (3 of 6 matched correctly), rerank, cascade, a time limit, a wire round trip |
| Examples, live | all 5 run; `ask` returns exit codes 0 and 2 as `semop` does |
| **Parity: TypeSafe request body** vs the Python provider, same `semop` request | byte-identical (including `"false": null` on a half-described Boolean and non-ASCII state) |
| **Parity: answers** | same decisions, options, and order; numbers within the server's run-to-run noise (urgency 1.61 vs 1.60) |
| **Parity: wire validation**, 32 malformed requests through both decoders | 32/32 identical paths and messages |

Not verified: Windows (MSVC) and Linux Clang. The CI workflow covers both, but it only runs once
the repo is pushed.

## 7. Follow-ups

- **libtypesafe 0.2:** add a public `Client::wait()`, so blocking helpers wake on network activity
  instead of polling every 1 ms (decision 15). Consider building with RTTI on (decision 18).
- **Async flows** (C++20 coroutines) for calling flows from a frame loop (decision 10).
- **The C++ `semop` CLI** on top of `wire` and `ask_each`: `ask`, `filter`, and maybe `mcp`.
- Observation: `jev-preview` currently reports itself as `jev-1.13.0`, the same as `jev-latest`, so
  a preview → latest cascade escalates to the same model today.


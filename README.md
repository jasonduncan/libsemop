# libsemop

**Write a semantic judgment once, run it on any System One model, and get a "don't know"
instead of a guess when the model isn't sure.** A C++17 port of
[semantic-operators](https://github.com/jasonduncan/semantic-operators) (Python), built on
[libtypesafe](https://github.com/jasonduncan/libtypesafe).

System One models are small, fast models that classify instead of generating text. You ask them
typed questions (yes/no, pick one, rate on a scale) about text or JSON, and they answer with
probabilities. [Jev](https://typesafe.ai) is the first; `semop::TypeSafe` talks to it.

> **Status:** 0.1.0, not yet released. The offline tests pass (clean under ASan, UBSan, and TSan),
> the live suite passes against jev-1.13.0, and requests are byte-identical to the Python
> library's. See [docs/DESIGN.md](docs/DESIGN.md), including the decisions to review.

```cpp
#include <semop/semop.hpp>
#include <semop/typesafe.hpp>

auto client = libtypesafe::Client::create();        // reads TYPESAFE_API_KEY; you own it
semop::TypeSafe provider(*client);                   // model defaults to "jev-latest"

const semop::Operator department{"department",
    semop::Choice{"Which team should handle this?", {{"billing", "Payments, refunds"}, {"other"}}}
        .with_min_confidence(0.8)};
const semop::Operator urgency{"urgency", semop::Score{"How urgent is this?", {"low", "medium", "high"}}};

auto answers = semop::apply(provider, "I was charged twice and I'm furious.", {department, urgency});
if (!answers) return fail(answers.error().message);

std::optional<std::string> team = answers->value(department);   // nullopt: "don't know"
std::optional<double> level = answers->value(urgency);          // expected level, e.g. 1.7 of 0..2
(*answers)[department].probabilities;                           // [("billing", 0.97), ("other", 0.03)]
(*answers)[department].call->model;                             // "jev-1.13.0"
```

## What's in it

| | |
|---|---|
| **Questions and answers** | `Boolean`, `Choice`, `Score`, `EnumChoice<E>`; `Answer` with value, probabilities, confidence, and the `Call` that produced it. A tie, or confidence under `min_confidence`, is "don't know". |
| **Operators** | `Operator{name, question}`: typed values (`bool`, `std::string`, `double`, your enum). `apply` asks several in one call. |
| **Flows** | Plain functions: `if` is the flow language. Reading an undecided answer gives an empty `Value`; `return v.stop();` ends the flow as "don't know". |
| **Filtering** | `filter_items`: one yes/no question per item, 8 calls in flight; match, no, unsure, or failed per item. |
| **Reranking** | `rerank`: score candidates against a relevance rubric, then sort; `reweighted` re-sorts without new calls. |
| **Escalation and timeouts** | `Cascade` re-asks only unsure answers of a stronger provider; `TimeLimited` bounds each call. Both are providers themselves. |
| **Benchmarks** | `bench::run`, `score`, `at_min_confidence`, `stability`, `ndcg`, and `run_flow` for whole flows. |
| **The `semop` JSON format** | `wire::decode` / `encode`: the request and response of Python's `semop` CLI and MCP server, with identical validation messages. |
| **Providers** | Non-blocking `Provider` (`start` / `pump`), `BlockingProvider`, and `FunctionProvider` for your own models; `TypeSafe` for Jev. |

Like libtypesafe, it's C++17, never throws (`semop::Result<T>`), and works in programs built
without exceptions. Providers are non-blocking, so `start()` + `pump()` works from any loop; the
blocking helpers (`ask`, `apply`, `filter_items`, `rerank`, flows) are built on top.

## Examples

`examples/`: `hello.cpp` (quick start), `triage.cpp` (a flow), `rerank.cpp`, `filter.cpp` (a
tiny semfilter), `ask.cpp` (a minimal `semop ask` over the JSON format).

## Build and use

```sh
cmake -S . -B build && cmake --build build     # fetches nlohmann_json and libtypesafe 0.1.0 if needed
ctest --test-dir build -LE live                 # offline tests and package checks
TYPESAFE_API_KEY=... LIBSEMOP_LIVE=1 ctest --test-dir build -L live
```

```cmake
find_package(libsemop 0.1 REQUIRED)              # or add_subdirectory(libsemop)
target_link_libraries(app PRIVATE libsemop::typesafe)   # or libsemop::semop for the core alone
```

Requires CMake ≥ 3.24, a C++17 compiler, and, for the TypeSafe provider, libtypesafe ≥ 0.1 and
libcurl.

## License

MIT. Not affiliated with TypeSafe AI.

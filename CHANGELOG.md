# Changelog

## 0.1.0 (unreleased)

First version: a C++17 port of semantic-operators-lib 0.11.0 (Python).

- Questions (`Boolean`, `Choice`, `Score`, `EnumChoice<E>`), `Answer`, `Call`, and `make_answer`
  with the Python library's rules for ties, `min_confidence`, rounding, and validation
- Typed `Operator<Q>`; `ask`, `apply`
- Non-blocking `Provider` (`start` / `pump` / `cancel`), `BlockingProvider`, `FunctionProvider`
- Flows without exceptions: `run_flow`, `Ask`, `Value<T>`, `Outcome<T>`, `FlowResult<T>`
- `filter_items`, `rerank`, `reweighted`, `ask_each` (concurrent, one call per item)
- `Cascade` (escalation) and `TimeLimited`
- `bench`: `run`, `score`, `at_min_confidence`, `stability`, `ndcg`, `run_flow`
- `wire`: the `semop` JSON request and response (schema_version "1")
- `semop::TypeSafe` provider (target `libsemop::typesafe`) on libtypesafe 0.1; requests are
  byte-identical to the Python provider's
- CMake package `libsemop` with targets `libsemop::semop` and `libsemop::typesafe`

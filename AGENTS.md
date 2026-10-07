# libsemop — agent instructions

A C++17 port of the Python `semantic-operators` library (semantic-operators-lib 0.11.0): typed
questions, operators, flows, filtering, reranking, escalation, benchmarks, and the `semop` JSON
format, with a TypeSafe provider built on libtypesafe.

## Status

0.1.0 built on 2026-09-25, autonomously while Jason was away. Read `docs/DESIGN.md` first:
§1 lists every decision made then, which Jason still needs to review. First pushed 2026-10-07 to
the private GitHub repo `jasonduncan/libsemop`; not released or tagged yet.

## Hard rules

- C++17. Never throws: failures are `semop::Result<T>` / `semop::Error`. The library builds
  with `-fno-exceptions` (RTTI stays on; see DESIGN §1 #18).
- The core (`libsemop::semop`) depends only on nlohmann_json. Only `src/typesafe.cpp` and
  `include/semop/typesafe.hpp` may use libtypesafe.
- Match the Python library's behavior: `make_answer` rules, wire messages, and TypeSafe request
  bodies are checked for parity. Port behavior from
  `~/PDLabsShare/Projects/semantic-operators-lib`, never from the legacy
  `~/PDLabsShare/Projects/semantic-operators`.
- Providers never block in `start()` and deliver results only inside `pump()`.

## Layout

- `include/semop/`: public API. `semop.hpp` is the core umbrella; `typesafe.hpp` is separate.
- `src/`: implementation. `util.hpp` holds the Python-style `repr` used in error messages.
- `tests/`: Catch2. `scripted.hpp` is a controllable fake provider. `test_typesafe.cpp` drives a
  real libtypesafe Client over a scripted transport. `consumer/` checks the installed and
  vendored package. `test_live.cpp` calls the real API.
- `examples/`: hello, triage (flow), rerank, filter, ask (the JSON format).

## Build / check

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build -LE live
LIBSEMOP_LIVE=1 ctest --test-dir build -L live   # needs TYPESAFE_API_KEY
```

Develop against a local libtypesafe with `-DFETCHCONTENT_SOURCE_DIR_LIBTYPESAFE=<path>`.

## Boundaries

- Live tests call the real API with `TYPESAFE_API_KEY` from the environment. Never send private
  data in them, and never print or commit keys.

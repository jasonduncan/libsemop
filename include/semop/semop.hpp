#pragma once

// The provider-neutral core (target libsemop::semop). The TypeSafe provider is separate:
// <semop/typesafe.hpp>, target libsemop::typesafe.

#include <string_view>

#include "semop/answers.hpp"
#include "semop/bench.hpp"
#include "semop/filtering.hpp"
#include "semop/flows.hpp"
#include "semop/provider.hpp"
#include "semop/questions.hpp"
#include "semop/rerank.hpp"
#include "semop/result.hpp"
#include "semop/wire.hpp"
#include "semop/wrappers.hpp"

namespace semop {

inline constexpr std::string_view version = "0.1.0";

}  // namespace semop

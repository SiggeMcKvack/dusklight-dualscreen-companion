#pragma once

// Fork's DuskLog surface routed to the mod LogService.
#include "mods/svc/log.hpp"

struct DuskLogShim {
    template <typename... A>
    void info(fmt::format_string<A...> f, A&&... a) const { mods::log::info(f, std::forward<A>(a)...); }
    template <typename... A>
    void warn(fmt::format_string<A...> f, A&&... a) const { mods::log::warn(f, std::forward<A>(a)...); }
    template <typename... A>
    void error(fmt::format_string<A...> f, A&&... a) const { mods::log::error(f, std::forward<A>(a)...); }
    template <typename... A>
    void debug(fmt::format_string<A...> f, A&&... a) const { mods::log::debug(f, std::forward<A>(a)...); }
};
inline constexpr DuskLogShim DuskLog{};

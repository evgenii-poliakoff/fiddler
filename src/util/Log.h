// Logging facade.
//
// Use the FLOG_* macros from any non-realtime thread:
//
//     FLOG_INFO("decoder", "opened {}, {} ms, {} Hz",
//               path, duration.count(), sampleRate);
//
// The first argument is a category string. Categories are hierarchical
// with '.' separators ("player", "player.thread", "player.callback").
// At runtime the user can filter with --log-filter, e.g.
//
//     fiddler --log-level=trace --log-filter='player.*'
//
// Three rules to keep the design intact:
//
//   1. Never include <spdlog/...> outside Log.cpp — this header is the
//      only seam, so we can swap libraries later without touching call
//      sites.
//   2. Never log from the PortAudio callback (or any realtime thread).
//      Logging takes locks. Use atomics + a stats() accessor and let
//      the GUI timer log periodically instead.
//   3. Format strings use fmt/std::format syntax: "{}" not "%s".

#pragma once

#include <fmt/core.h>          // spdlog uses fmt; we only borrow FMT_STRING

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fiddler::log {

enum class Level { Trace, Debug, Info, Warn, Error, Off };

struct Config {
    Level                                  level    = Level::Warn;
    std::string                            filter   = "*";    // glob, e.g. "player.*"
    std::optional<std::filesystem::path>   logFile;           // unset → no file sink
};

// Parse "trace" | "debug" | "info" | "warn" | "error" | "off"
// (case-insensitive). Returns std::nullopt on unknown input.
std::optional<Level> parseLevel(std::string_view s);

// Initialise the logging system. Idempotent: a second call replaces the
// previous configuration. Must be called before any FLOG_* macro fires.
void init(const Config& cfg);

// Effective threshold (after init). Used by the macros to short-circuit.
Level threshold() noexcept;

// Test capture hook. While set, every log call that passes the level
// and category filters is *also* delivered to this callback (in addition
// to the regular sinks). Pass nullptr to remove. Tests use this to assert
// against emitted messages without including spdlog headers; production
// code never touches it.
using CaptureFn = std::function<void(Level                 lvl,
                                     std::string_view      category,
                                     std::string_view      message)>;
void setCapture(CaptureFn fn);

// Internal — do not call directly; use the FLOG_* macros below.
void logImpl(Level lvl, std::string_view category, std::string msg);

// Whether `category` matches the current filter glob.
bool categoryEnabled(std::string_view category) noexcept;

} // namespace fiddler::log

// The macro guards prevent argument evaluation when the call is filtered
// out — important for TRACE in tight loops where formatting itself costs.
#define FLOG_AT(LEVEL_ENUM, CATEGORY, ...)                                  \
    do {                                                                    \
        if (::fiddler::log::threshold() <= (LEVEL_ENUM)                     \
            && ::fiddler::log::categoryEnabled(CATEGORY)) {                 \
            ::fiddler::log::logImpl((LEVEL_ENUM), (CATEGORY),               \
                                    ::fmt::format(__VA_ARGS__));            \
        }                                                                   \
    } while (false)

#define FLOG_TRACE(category, ...) FLOG_AT(::fiddler::log::Level::Trace, category, __VA_ARGS__)
#define FLOG_DEBUG(category, ...) FLOG_AT(::fiddler::log::Level::Debug, category, __VA_ARGS__)
#define FLOG_INFO(category, ...)  FLOG_AT(::fiddler::log::Level::Info,  category, __VA_ARGS__)
#define FLOG_WARN(category, ...)  FLOG_AT(::fiddler::log::Level::Warn,  category, __VA_ARGS__)
#define FLOG_ERROR(category, ...) FLOG_AT(::fiddler::log::Level::Error, category, __VA_ARGS__)

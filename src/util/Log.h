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
    // MEMO: default level is Info (not Warn) so the startup banner
    // and lifecycle events ("fiddler X.Y starting up", "open: path=…")
    // are visible without any flags. Tap-place / select / wrap-around
    // are still Debug-level — pass --log-level=debug or set a
    // narrower --log-filter (which auto-promotes to debug — see
    // resolveLogConfig) to surface them.
    Level                                  level    = Level::Info;
    // Comma-separated list of category globs. Each glob is either a
    // bare "*" (matches everything), a trailing ".*" pattern
    // (subtree match — "player.*" matches "player" and
    // "player.thread"), or a literal category name (exact match).
    // Whitespace around each entry is trimmed. Examples:
    //   "*"
    //   "player.*"
    //   "ui.*,player,waveform,score"
    std::string                            filter   = "*";
    std::optional<std::filesystem::path>   logFile;           // unset → no file sink
};

// Inputs collected from the CLI + environment, kept as plain
// strings + booleans so the resolver is independent of QCommandLineParser
// (and therefore unit-testable without a QApplication).
struct CliInputs {
    // --log-level value. levelStr is ignored unless levelExplicit is
    // true, so callers can pass an empty string when the flag wasn't
    // set.
    bool        levelExplicit  = false;
    std::string levelStr;

    // --log-filter value. filterStr is ignored unless filterExplicit
    // is true; an unset flag falls back to the documented "*" default.
    bool        filterExplicit = false;
    std::string filterStr;

    // --log-file. Empty string when unset.
    std::string logFile;

    // FIDDLER_LOG_LEVEL env var value. Empty string when unset.
    std::string envLevel;
};

// Resolution outcome — either a Config or an error message (for
// malformed --log-level / FIDDLER_LOG_LEVEL values).
struct ResolveResult {
    Config                       config;
    std::optional<std::string>   error;
};

// Resolve CLI + environment into a Config.
//
// Precedence for level: CLI > env > default.
// Filter: CLI value if set, else "*".
//
// MEMO: load-bearing UX rule — when --log-filter is explicitly set
// and no level is otherwise specified, level is promoted to Debug.
// The user's intent in passing a filter is "I want to see X"; if we
// left the level at Info they'd get the filter-matching INFO+ lines
// but miss every Debug call (which is where almost all gestural
// logging lives), and quietly conclude the filter is broken. A past
// debugging session burned exactly that hour. See docs/debugging.md.
ResolveResult resolveLogConfig(const CliInputs& in);

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

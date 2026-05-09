#include "util/Log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace fiddler::log {

namespace {

// One spdlog logger backs everything. We store the category in the log
// message rather than spinning up a logger per category, because category
// names are user-supplied at call sites and we don't want to manage a
// hash map of loggers keyed on arbitrary strings.
std::shared_ptr<spdlog::logger> g_logger;
std::atomic<Level>              g_threshold{Level::Warn};

// MEMO: g_filterPatterns is the parsed form of Config::filter — a
// comma-separated list of globs split into individual entries. We
// keep both (g_filterRaw retains the original string for any tooling
// that wants it) but the matcher only consults the parsed vector.
// Updates are guarded by g_initMutex; reads from categoryEnabled are
// unlocked, on the same accept-the-data-race basis the rest of this
// file already operates on (init is a one-shot at startup).
std::string                     g_filterRaw = "*";
std::vector<std::string>        g_filterPatterns{"*"};
std::mutex                      g_initMutex;

// Test capture: a function that receives every emitted log line.
// Read-mostly; a separate mutex keeps lookup off the init path.
CaptureFn                       g_capture;
std::mutex                      g_captureMutex;

spdlog::level::level_enum toSpdlog(Level l) noexcept {
    switch (l) {
        case Level::Trace: return spdlog::level::trace;
        case Level::Debug: return spdlog::level::debug;
        case Level::Info:  return spdlog::level::info;
        case Level::Warn:  return spdlog::level::warn;
        case Level::Error: return spdlog::level::err;
        case Level::Off:   return spdlog::level::off;
    }
    return spdlog::level::warn;
}

// Glob match supporting only one wildcard form: a trailing ".*" or a bare
// "*". Sufficient for our hierarchical category names; avoids pulling in
// a regex engine.
bool globMatch(std::string_view pattern, std::string_view text) noexcept {
    if (pattern == "*") return true;
    if (pattern.size() >= 2 && pattern.substr(pattern.size() - 2) == ".*") {
        const auto prefix = pattern.substr(0, pattern.size() - 2);
        return text == prefix
            || (text.size() > prefix.size()
                && text.starts_with(prefix)
                && text[prefix.size()] == '.');
    }
    return pattern == text;
}

std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// Split a comma-separated filter into individual glob patterns.
// Whitespace around each entry is trimmed; empty entries are
// dropped; if the result is empty the filter falls back to "*".
//
// Examples:
//   "*"                       → ["*"]
//   "player.*"                → ["player.*"]
//   "ui.*,player,waveform"    → ["ui.*", "player", "waveform"]
//   "  ui.*  ,  player  "     → ["ui.*", "player"]
//   ""                        → ["*"]
std::vector<std::string> parseFilter(std::string_view filter) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (start <= filter.size()) {
        const auto commaPos = filter.find(',', start);
        const auto end =
            (commaPos == std::string_view::npos) ? filter.size() : commaPos;

        auto begin = start;
        while (begin < end
               && std::isspace(static_cast<unsigned char>(filter[begin]))) {
            ++begin;
        }
        auto last = end;
        while (last > begin
               && std::isspace(static_cast<unsigned char>(filter[last - 1]))) {
            --last;
        }
        if (last > begin) {
            out.emplace_back(filter.substr(begin, last - begin));
        }
        if (commaPos == std::string_view::npos) break;
        start = commaPos + 1;
    }
    if (out.empty()) out.emplace_back("*");
    return out;
}

} // namespace

ResolveResult resolveLogConfig(const CliInputs& in) {
    ResolveResult out;
    Config& cfg = out.config;

    // Step 1: env (lower precedence than the CLI flag).
    bool levelFromEnvOrCli = false;
    if (!in.envLevel.empty()) {
        if (auto lvl = parseLevel(in.envLevel)) {
            cfg.level = *lvl;
            levelFromEnvOrCli = true;
        } else {
            out.error = "Unknown FIDDLER_LOG_LEVEL: " + in.envLevel;
            return out;
        }
    }

    // Step 2: --log-level (overrides env if set).
    if (in.levelExplicit) {
        if (auto lvl = parseLevel(in.levelStr)) {
            cfg.level = *lvl;
            levelFromEnvOrCli = true;
        } else {
            out.error = "Unknown log level: " + in.levelStr;
            return out;
        }
    }

    // Step 3: filter — explicit value wins; unset falls back to "*".
    if (in.filterExplicit) {
        cfg.filter = in.filterStr.empty() ? std::string{"*"}
                                          : in.filterStr;
    }
    // else cfg.filter keeps its default ("*")

    // Step 4: load-bearing UX rule (see Log.h). Filter without level
    // promotes verbosity. Without this, every gestural FLOG_DEBUG
    // call is silently filtered out and the user sees nothing.
    if (in.filterExplicit && !levelFromEnvOrCli) {
        cfg.level = Level::Debug;
    }

    // Step 5: log file passthrough.
    if (!in.logFile.empty()) {
        cfg.logFile = std::filesystem::path(in.logFile);
    }

    return out;
}

std::optional<Level> parseLevel(std::string_view s) {
    const auto lo = toLower(s);
    if (lo == "trace") return Level::Trace;
    if (lo == "debug") return Level::Debug;
    if (lo == "info")  return Level::Info;
    if (lo == "warn" || lo == "warning") return Level::Warn;
    if (lo == "error" || lo == "err")    return Level::Error;
    if (lo == "off"   || lo == "none")   return Level::Off;
    return std::nullopt;
}

void init(const Config& cfg) {
    std::lock_guard<std::mutex> lock(g_initMutex);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    if (cfg.logFile) {
        // Rotate at 5 MiB, keep 3 historical files. Plenty of headroom
        // for a debugging session, bounded so logs can't fill the disk.
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            cfg.logFile->string(), 5 * 1024 * 1024, 3));
    }

    auto logger = std::make_shared<spdlog::logger>("fiddler",
                                                    sinks.begin(), sinks.end());
    logger->set_level(toSpdlog(cfg.level));
    logger->set_pattern("%H:%M:%S.%e %^%-5l%$ [%t] %v");
    // MEMO: flush_on(debug) is deliberately aggressive — Fiddler is a
    // debugging tool, used interactively at human time scales. Per-
    // line flushing means the log file is always up to date when the
    // user reproduces a bug, even if the app is force-killed (SIGTERM
    // or crash). The throughput cost (one fsync-equivalent per line)
    // is irrelevant at gestural rates. See tests/test_app_subprocess.cpp:
    // the --log-file functional test depends on this flush policy.
    logger->flush_on(spdlog::level::debug);

    spdlog::set_default_logger(logger);
    g_logger = std::move(logger);
    g_threshold.store(cfg.level, std::memory_order_release);
    g_filterRaw      = cfg.filter.empty() ? std::string{"*"} : cfg.filter;
    g_filterPatterns = parseFilter(g_filterRaw);
}

Level threshold() noexcept {
    return g_threshold.load(std::memory_order_acquire);
}

bool categoryEnabled(std::string_view category) noexcept {
    // Any-match across the parsed pattern list. The common single-
    // pattern case ("*", "player.*", etc.) just checks one entry;
    // a comma-separated filter checks each pattern in turn until
    // one matches.
    for (const auto& pattern : g_filterPatterns) {
        if (globMatch(pattern, category)) return true;
    }
    return false;
}

void setCapture(CaptureFn fn) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    g_capture = std::move(fn);
}

void logImpl(Level lvl, std::string_view category, std::string msg) {
    if (g_logger) {
        g_logger->log(toSpdlog(lvl), "[{}] {}", category, msg);
    }
    // Capture hook (test-only in practice). Take a copy under the mutex
    // so the call itself doesn't run with the lock held.
    CaptureFn fn;
    {
        std::lock_guard<std::mutex> lock(g_captureMutex);
        fn = g_capture;
    }
    if (fn) fn(lvl, category, msg);
}

} // namespace fiddler::log

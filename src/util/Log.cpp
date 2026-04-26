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
std::string                     g_filter   = "*";
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

} // namespace

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
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(logger);
    g_logger = std::move(logger);
    g_threshold.store(cfg.level, std::memory_order_release);
    g_filter = cfg.filter.empty() ? std::string{"*"} : cfg.filter;
}

Level threshold() noexcept {
    return g_threshold.load(std::memory_order_acquire);
}

bool categoryEnabled(std::string_view category) noexcept {
    // Cheap-path: no filter set means everything's on.
    if (g_filter == "*") return true;
    return globMatch(g_filter, category);
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

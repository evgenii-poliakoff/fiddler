// Functional tests that launch the built fiddler binary as a
// subprocess and verify it produces the expected log output. These
// tests exist because two debugging sessions in this project's
// history were burned by a default log level (Warn) that silently
// dropped every gestural FLOG_DEBUG call when the user passed only
// --log-filter. The unit tests for resolveLogConfig pin the
// resolution rules; these tests pin the end-to-end CLI experience —
// "does the app actually emit logs when the user expects to see
// them?" — by talking to the real binary, not a test fixture.
//
// MEMO[refactor]: each TEST_CASE drives the binary with a specific
// flag combination and greps the captured stderr for an expected
// substring. The binary path is baked in at build time
// (FIDDLER_BINARY_PATH); the tests don't depend on any working
// directory or PATH lookup. Running with QT_QPA_PLATFORM=offscreen
// keeps the GUI from trying to open a real X / Wayland display on
// CI, the same way the rest of the suite does.

#include "qt_test_app.h"

#include <QByteArray>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using fiddler::test::qtApp;

namespace {

// Boot the app with the given args, give it a moment to start +
// emit the startup banner, then terminate it. Returns (stderr,
// stdout) as a pair of strings.
struct ProcOutput {
    QString stderrText;
    QString stdoutText;
    int     exitCode = -1;
    bool    timedOut = false;
};

ProcOutput runApp(const QStringList& args, int waitMs = 1500) {
    qtApp();   // make sure a QApplication exists for QProcess to use

    QProcess proc;
    // Force offscreen so we don't need a display server. Inherit
    // the rest of the env so the user's locale / library paths
    // still work.
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("QT_QPA_PLATFORM", "offscreen");
    proc.setProcessEnvironment(env);

    // Buffer stderr + stdout separately — log lines go to stderr
    // (spdlog's stderr_color_sink) and that's what we want to grep.
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(QString::fromUtf8(FIDDLER_BINARY_PATH), args);
    REQUIRE(proc.waitForStarted(5000));

    // Let the app run long enough to:
    //   (1) parse args,
    //   (2) call init() and emit the startup banner,
    //   (3) build the central widget + dock + initial waveform.
    // 1500 ms is generous — on a developer laptop the banner lands
    // within a few hundred ms — but cheap insurance against CI
    // hosts that are slow on first-process launch. We proactively
    // terminate after the wait window because the app would
    // otherwise sit in QApplication::exec() forever.
    proc.waitForReadyRead(waitMs);
    proc.terminate();
    if (!proc.waitForFinished(2000)) {
        proc.kill();
        proc.waitForFinished(1000);
    }

    ProcOutput out;
    out.stderrText = QString::fromUtf8(proc.readAllStandardError());
    out.stdoutText = QString::fromUtf8(proc.readAllStandardOutput());
    out.exitCode   = proc.exitCode();
    out.timedOut   = (proc.exitStatus() != QProcess::NormalExit);
    return out;
}

} // namespace

TEST_CASE("fiddler binary: startup banner is always visible on stderr",
          "[app][subprocess]") {
    // MEMO: load-bearing — the banner is the user's "what config is
    // actually loaded?" answer. It must not be filterable away,
    // because that's exactly when the user needs to see it.
    auto out = runApp({});
    REQUIRE(out.stderrText.contains("fiddler"));
    REQUIRE(out.stderrText.contains("log level="));
    REQUIRE(out.stderrText.contains("filter="));
}

TEST_CASE("fiddler binary: --log-filter alone produces gestural log lines",
          "[app][subprocess]") {
    // MEMO: the bug this test guards. Before the fix, --log-filter
    // alone left the level at Warn and the user saw NOTHING — every
    // FLOG_DEBUG was below threshold. The fix promotes level to
    // Debug whenever a filter is set without a level. This test
    // pins that promotion by asserting the banner reports level=debug
    // when only --log-filter is passed.
    auto out = runApp({"--log-filter=ui.*"});
    REQUIRE(out.stderrText.contains("level=debug"));
    REQUIRE(out.stderrText.contains("filter='ui.*'"));
}

TEST_CASE("fiddler binary: explicit --log-level overrides the auto-promote",
          "[app][subprocess]") {
    auto out = runApp({"--log-filter=ui.*", "--log-level=warn"});
    REQUIRE(out.stderrText.contains("level=warn"));
}

TEST_CASE("fiddler binary: no flags → default level info",
          "[app][subprocess]") {
    auto out = runApp({});
    REQUIRE(out.stderrText.contains("level=info"));
}

TEST_CASE("fiddler binary: --log-file writes log lines to the file",
          "[app][subprocess]") {
    // MEMO: the bug-report flow. Users attach the log file when
    // reporting issues; if the file sink doesn't actually write,
    // the bug-report channel is silently broken.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString logPath = dir.filePath("fiddler.log");

    auto out = runApp({
        QStringLiteral("--log-file=%1").arg(logPath),
        "--log-level=info",
    });

    QFile f(logPath);
    REQUIRE(f.exists());
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString contents = QString::fromUtf8(f.readAll());
    REQUIRE(contents.contains("starting up"));
}

TEST_CASE("fiddler binary: bad --log-level exits non-zero with an error",
          "[app][subprocess]") {
    auto out = runApp({"--log-level=bogus"}, /*waitMs=*/300);
    // The binary should fail fast with our error message; the
    // exact wording is "Unknown log level: bogus".
    REQUIRE(out.stderrText.contains("Unknown log level"));
    // exitCode should be 2 from main().
    REQUIRE(out.exitCode == 2);
}

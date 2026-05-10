#include "scene_runner.h"

#include "scene_actions.h"
#include "screenshot_fixture.h"
#include "ui/MainWindow.h"
#include "ui/WaveformWidget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFontMetrics>
#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QStringList>
#include <QString>
#include <QStyle>
#include <QThread>
#include <QWidget>

#include <iostream>

namespace fiddler::screenshots {

namespace {

constexpr double kDefaultDpiScale = 2.0;

// Margin defaults derived from the style: PM_LayoutLeftMargin gives
// the same value Qt's QFormLayout / QHBoxLayout would use as their
// own padding, so a screenshot's "breathing room" matches what the
// rest of the UI uses elsewhere. Falls back to font metrics if the
// style returns 0.
int defaultMargin(QWidget& w) {
    const int byStyle =
        w.style()->pixelMetric(QStyle::PM_LayoutLeftMargin, nullptr, &w);
    if (byStyle > 0) return byStyle;
    return std::max(4, QFontMetrics(w.font()).height() / 2);
}

// Apply the margin to a QRect, clamping to the bounds of the
// containing widget. Vertical / horizontal can be customised
// independently — useful for region scenes where the row's natural
// height already gives some vertical padding via layout spacing.
QRect padRect(const QRect& r, int marginH, int marginV,
              const QRect& bounds)
{
    return r.adjusted(-marginH, -marginV, +marginH, +marginV)
            .intersected(bounds);
}

// Render a widget's rect into a higher-density QPixmap. Output's
// DPR makes painter coords logical, so painter.scale() must NOT
// also be called (double-scaling truncates the source to its
// top-left quadrant).
QPixmap grabAtScale(QWidget& w, const QRect& rect, double scale) {
    const int wPx = static_cast<int>(rect.width()  * scale);
    const int hPx = static_cast<int>(rect.height() * scale);
    QPixmap out(wPx, hPx);
    out.setDevicePixelRatio(scale);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    w.render(&painter, QPoint{0, 0}, QRegion(rect));
    return out;
}

// Union the geometries of named widgets, mapped into the main
// window's coordinate space. Names that don't resolve to a widget
// are appended to `outMissing` so the caller can fail loudly;
// silently dropping a missing name produced wrong-but-plausible
// PNGs (a partial union of just the names that did resolve).
QRect unionInWindow(const ui::MainWindow& window,
                    const QStringList& names,
                    QStringList& outMissing)
{
    QRect out;
    for (const QString& name : names) {
        auto* widget = window.findChild<QWidget*>(name);
        if (!widget) {
            outMissing.append(name);
            continue;
        }
        const QRect r(widget->mapTo(&window, QPoint{0, 0}),
                      widget->size());
        out = out.isEmpty() ? r : out.united(r);
    }
    return out;
}

// Sample the parent widget's background colour from one pixel just
// outside `target`'s top-left, so a composited canvas around the
// widget blends with the surrounding chrome instead of sitting on
// transparent void. Falls back to a neutral light grey if the
// parent isn't available or the sample point lies outside it.
QColor sampleParentBg(QWidget& target) {
    QColor bg(0xee, 0xee, 0xee);
    auto* parent = target.parentWidget();
    if (!parent) return bg;
    const QPixmap parentShot = parent->grab(parent->rect());
    const QPoint sampleAt = target.mapTo(parent, QPoint{-1, -1});
    if (parent->rect().contains(sampleAt)) {
        bg = parentShot.toImage().pixelColor(sampleAt);
    }
    return bg;
}

// Composite an element's grab onto a margined canvas, sampling
// the parent's background colour from one pixel just outside the
// widget's top-left so the margin doesn't sit on transparent void.
QPixmap composeElement(QWidget& target, int margin, double scale) {
    const QPixmap inner = target.grab();
    const QColor bg = sampleParentBg(target);

    const QSize natSize = target.size();
    const int logicalW = natSize.width()  + 2 * margin;
    const int logicalH = natSize.height() + 2 * margin;
    QPixmap composed(static_cast<int>(logicalW * scale),
                     static_cast<int>(logicalH * scale));
    composed.setDevicePixelRatio(scale);
    composed.fill(bg);
    QPainter p(&composed);
    p.drawPixmap(QPoint{margin, margin}, inner);
    p.end();
    return composed;
}

// Composite a closed combo cell with its open popup container,
// stacked vertically with a small gap, on the combo's parent bg.
// MEMO: the popup is a top-level QFrame, not a child of the main
// window, so it's grabbed independently and pasted below the combo
// at the canvas's local coordinates rather than via a region union.
QPixmap composePopup(QComboBox& combo, QWidget& popup,
                     int marginH, int marginV, double scale)
{
    const QPixmap comboShot = combo.grab();
    const QPixmap popupShot = popup.grab();
    const QColor bg = sampleParentBg(combo);

    constexpr int kGap = 1;
    const int innerW = std::max(comboShot.width(), popupShot.width());
    const int innerH = comboShot.height() + kGap + popupShot.height();
    const int logicalW = innerW + 2 * marginH;
    const int logicalH = innerH + 2 * marginV;

    QPixmap composed(static_cast<int>(logicalW * scale),
                     static_cast<int>(logicalH * scale));
    composed.setDevicePixelRatio(scale);
    composed.fill(bg);
    QPainter p(&composed);
    p.drawPixmap(QPoint{marginH, marginV}, comboShot);
    p.drawPixmap(QPoint{marginH,
                        marginV + comboShot.height() + kGap},
                 popupShot);
    p.end();
    return composed;
}

} // namespace

void flushEvents() {
    for (int i = 0; i < 4; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 16);
    }
}

void resetWindow(ui::MainWindow& window) {
    // sizeHint() asks the layout system for the window's natural
    // size given its current children + their sizeHints — which
    // ends up large enough for transport row + waveform + staff +
    // sliders + dock when those are all instantiated. We don't
    // need a hardcoded canonical size; Qt computes it for us.
    window.show();
    flushEvents();
    QSize natural = window.sizeHint();
    if (auto* dock = window.findChild<QDockWidget*>("projectViewerDock")) {
        // The dock contributes its width to the window's natural
        // size, but Qt doesn't always include the right-area dock
        // in sizeHint until the layout has settled. Add it
        // explicitly so the dock has visible area to render into.
        natural.rwidth() += dock->sizeHint().width();
        dock->setFloating(false);
        dock->show();
    }
    window.resize(natural);
    flushEvents();
}

void loadFixture(ui::MainWindow& window) {
    const auto path = generateFixtureWav();
    if (!window.loadFile(QString::fromStdString(path.string()))) {
        std::cerr << "screenshot: loadFile failed for "
                  << path.string() << "\n";
        return;
    }
    auto* waveform = window.findChild<ui::WaveformWidget*>("waveformWidget");
    if (!waveform) return;
    // Bounded wait for the worker-built overview to land on the
    // GUI thread.
    for (int i = 0; i < 200; ++i) {
        if (waveform->overview() != nullptr) break;
        flushEvents();
        QThread::msleep(15);
    }
    flushEvents();
}

bool runScene(ui::MainWindow& window,
              const QDir& outputDir,
              const SceneSpec& spec)
{
    runActions(window, spec.setup);
    flushEvents();

    const double scale = spec.dpiScale.value_or(kDefaultDpiScale);
    const int marginDefault = defaultMargin(window);
    const int marginH = spec.margin.horizontal.value_or(marginDefault);
    const int marginV = spec.margin.vertical.value_or(marginDefault);

    QPixmap shot;
    switch (spec.capture.kind) {
    case CaptureKind::Window: {
        shot = grabAtScale(window, window.rect(), scale);
        break;
    }
    case CaptureKind::Region: {
        QStringList missing;
        QRect region = unionInWindow(
            window, spec.capture.objectNames, missing);
        if (!missing.isEmpty()) {
            std::cerr << "screenshot: '"
                      << spec.id.toStdString()
                      << "' — unresolved object name(s): "
                      << missing.join(", ").toStdString()
                      << "\n";
            return false;
        }
        if (region.isEmpty()) {
            std::cerr << "screenshot: '"
                      << spec.id.toStdString()
                      << "' — region is empty (no object names "
                         "given?)\n";
            return false;
        }
        region = padRect(region, marginH, marginV,
                         QRect{QPoint{0, 0}, window.size()});
        shot = grabAtScale(window, region, scale);
        break;
    }
    case CaptureKind::Element: {
        if (spec.capture.objectNames.isEmpty()) {
            std::cerr << "screenshot: '"
                      << spec.id.toStdString()
                      << "' — element capture missing target name\n";
            return false;
        }
        const QString& name = spec.capture.objectNames.first();
        auto* target = window.findChild<QWidget*>(name);
        if (!target) {
            std::cerr << "screenshot: '"
                      << spec.id.toStdString()
                      << "' — target widget '"
                      << name.toStdString() << "' not found\n";
            return false;
        }
        // Element scenes default to the symmetric margin; if the
        // YAML specifies asymmetric H / V, the larger wins (the
        // canvas is square-ish around the small widget).
        const int m = std::max(marginH, marginV);
        shot = composeElement(*target, m, scale);
        break;
    }
    case CaptureKind::Popup: {
        if (spec.capture.objectNames.isEmpty()) {
            std::cerr << "screenshot: '"
                      << spec.id.toStdString()
                      << "' — popup capture missing combo name\n";
            return false;
        }
        const QString& name = spec.capture.objectNames.first();
        auto* combo = window.findChild<QComboBox*>(name);
        if (!combo) {
            std::cerr << "screenshot: '"
                      << spec.id.toStdString()
                      << "' — no QComboBox named '"
                      << name.toStdString() << "'\n";
            return false;
        }
        // The popup container is the QFrame that wraps the combo's
        // view; it only exists once showPopup() has been called via
        // the `show-popup` setup action.
        QWidget* popup = combo->view()
            ? combo->view()->parentWidget() : nullptr;
        if (!popup || !popup->isVisible()) {
            std::cerr << "screenshot: '"
                      << spec.id.toStdString()
                      << "' — combo '" << name.toStdString()
                      << "' popup is not visible; did you forget "
                         "`show-popup` in setup?\n";
            return false;
        }
        shot = composePopup(*combo, *popup, marginH, marginV, scale);
        // Defensive close so any later scene that touches the same
        // combo isn't surprised by a still-open popup.
        combo->hidePopup();
        flushEvents();
        break;
    }
    }

    if (shot.isNull()) {
        std::cerr << "screenshot: '" << spec.id.toStdString()
                  << "' captured nothing\n";
        return false;
    }

    const QString fullPath = outputDir.filePath(spec.output);
    if (!shot.save(fullPath, "PNG")) {
        std::cerr << "screenshot: failed to save '"
                  << fullPath.toStdString() << "'\n";
        return false;
    }

    std::cout << "  " << spec.id.toStdString()
              << " → " << spec.output.toStdString()
              << "  (" << shot.width() << "×" << shot.height()
              << " px @ " << scale << "×)\n";
    return true;
}

} // namespace fiddler::screenshots

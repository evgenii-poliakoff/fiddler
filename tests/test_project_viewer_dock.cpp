// Tests for ProjectViewerDock — the right-side artifact viewer.
//
// MEMO[refactor]: each TEST_CASE has a one-line comment naming the
// rule it pins. When the dock grows new categories (loops in PR B,
// notes / sections later), don't be afraid to refactor these tests
// — but read each comment first so you preserve the *property*
// being checked rather than the literal value.

#include "qt_test_app.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "score/NoteModel.h"
#include "ui/LoopCountdownWidget.h"
#include "ui/ProjectViewerDock.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using fiddler::score::LoopModel;
using fiddler::score::MarkerModel;
using fiddler::score::NoteModel;
using fiddler::test::qtApp;
using fiddler::ui::ProjectViewerDock;

namespace {

constexpr int kMarkerIdRole = Qt::UserRole + 1;   // mirrors dock impl
constexpr int kLoopIdRole   = Qt::UserRole + 2;

std::shared_ptr<MarkerModel>
makeModelWith(std::span<const std::int64_t> stamps) {
    auto m = std::make_shared<MarkerModel>();
    for (auto ms : stamps) (void)m->add(ms);
    return m;
}

std::shared_ptr<LoopModel>
makeLoopModelWith(std::span<const std::pair<std::int64_t, std::int64_t>> ranges) {
    auto m = std::make_shared<LoopModel>();
    for (const auto& r : ranges) (void)m->add(r.first, r.second);
    return m;
}

// Find the QTreeWidget child by objectName ("projectViewerTree") so
// the tests don't have to know the dock's internal layout.
QTreeWidget* treeOf(ProjectViewerDock& dock) {
    auto* tree = dock.findChild<QTreeWidget*>("projectViewerTree");
    REQUIRE(tree != nullptr);
    return tree;
}

QStackedWidget* stackOf(ProjectViewerDock& dock) {
    auto* stack =
        dock.findChild<QStackedWidget*>("projectViewerPropertyStack");
    REQUIRE(stack != nullptr);
    return stack;
}

// Locate the marker entry in the tree whose kMarkerIdRole == id.
// Returns nullptr if not found. The Markers category is at index 0.
QTreeWidgetItem*
findMarkerRow(QTreeWidget& tree, std::int64_t id) {
    REQUIRE(tree.topLevelItemCount() >= 1);
    auto* category = tree.topLevelItem(0);
    for (int i = 0; i < category->childCount(); ++i) {
        auto* child = category->child(i);
        if (child->data(0, kMarkerIdRole).toLongLong() == id) {
            return child;
        }
    }
    return nullptr;
}

// Wire the dock's edit-request signals back into the model. In
// production MainWindow does this AND captures undo snapshots; the
// dock's unit tests don't care about undo, so a thin pass-through
// preserves the old "edit a field, model updates" assertion shape.
// MEMO: keep this in sync with the equivalent connect block in
// MainWindow (src/ui/MainWindow.cpp's "Property-page edits" comment)
// so the dock's contract is exercised the same way both places.
void wireDockMarkerWritesToModel(ProjectViewerDock& dock,
                                 std::shared_ptr<MarkerModel> model) {
    QObject::connect(&dock, &ProjectViewerDock::markerRenameRequested,
                     model.get(), [m = model](std::int64_t id, QString name) {
                         m->rename(id, std::move(name));
                     });
    QObject::connect(&dock, &ProjectViewerDock::markerPositionEditRequested,
                     model.get(), [m = model](std::int64_t id, std::int64_t newMs) {
                         m->setPosition(id, newMs);
                     });
}

void wireDockLoopWritesToModel(ProjectViewerDock& dock,
                               std::shared_ptr<LoopModel> model) {
    QObject::connect(&dock, &ProjectViewerDock::loopRenameRequested,
                     model.get(), [m = model](std::int64_t id, QString name) {
                         m->rename(id, std::move(name));
                     });
    QObject::connect(&dock, &ProjectViewerDock::loopRangeEditRequested,
                     model.get(), [m = model](std::int64_t id,
                                               std::int64_t s,
                                               std::int64_t e) {
                         m->setRange(id, s, e);
                     });
}

// Same for loops — Loops category is at top-level index 1.
QTreeWidgetItem*
findLoopRow(QTreeWidget& tree, std::int64_t id) {
    REQUIRE(tree.topLevelItemCount() >= 2);
    auto* category = tree.topLevelItem(1);
    for (int i = 0; i < category->childCount(); ++i) {
        auto* child = category->child(i);
        if (child->data(0, kLoopIdRole).toLongLong() == id) {
            return child;
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Empty / model-less state
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: with no model, tree is empty and 'no selection' shows",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    auto* tree = treeOf(dock);

    // Two top-level category headers: Markers + Loops. Notes have
    // no tree row (step 6.2) — selection happens via the staff.
    REQUIRE(tree->topLevelItemCount() == 2);
    REQUIRE(tree->topLevelItem(0)->childCount() == 0);
    REQUIRE(tree->topLevelItem(1)->childCount() == 0);

    // The "no selection" page is index 0 in the property stack.
    REQUIRE(stackOf(dock)->currentIndex() == 0);
}

TEST_CASE("ProjectViewerDock: setMarkerModel populates the tree with entries",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    dock.setMarkerModel(makeModelWith(std::span<const std::int64_t>{stamps}));

    auto* tree = treeOf(dock);
    auto* category = tree->topLevelItem(0);
    REQUIRE(category->childCount() == 3);
    // Each child carries an int64 marker ID in the user-data role —
    // load-bearing because the dock looks up markers by ID, not text.
    for (int i = 0; i < category->childCount(); ++i) {
        const auto id = category->child(i)
                              ->data(0, kMarkerIdRole).toLongLong();
        REQUIRE(id != 0);
    }
}

// ---------------------------------------------------------------------------
// Selection round-trip
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: clicking a tree entry emits markerSelectionChanged",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    const auto markerId = *model->idAt(0);

    auto* tree = treeOf(dock);
    auto* row  = findMarkerRow(*tree, markerId);
    REQUIRE(row != nullptr);

    QSignalSpy spy(&dock, &ProjectViewerDock::markerSelectionChanged);
    tree->setCurrentItem(row);     // simulates user click in the tree

    REQUIRE(spy.count() == 1);
    REQUIRE(spy.takeFirst().at(0)
              .value<std::optional<std::int64_t>>() == markerId);
    REQUIRE(dock.selectedMarkerId() == markerId);
}

TEST_CASE("ProjectViewerDock: setSelectedMarkerId(id) shows the marker page",
          "[project-viewer-dock][gui]") {
    // MEMO: this is the inbound path — MainWindow forwards a
    // selection from the score widgets via this slot. Pinning the
    // page-switch is what proves the dock reacts visually.
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);

    dock.setSelectedMarkerId(*model->idAt(0));

    REQUIRE(stackOf(dock)->currentIndex() == 1);   // marker page
    REQUIRE(dock.selectedMarkerId() == *model->idAt(0));
}

TEST_CASE("ProjectViewerDock: setSelectedMarkerId(nullopt) returns to 'no selection'",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    dock.setSelectedMarkerId(*model->idAt(0));
    REQUIRE(stackOf(dock)->currentIndex() == 1);

    dock.setSelectedMarkerId(std::nullopt);
    REQUIRE(stackOf(dock)->currentIndex() == 0);
    REQUIRE_FALSE(dock.selectedMarkerId().has_value());
}

TEST_CASE("ProjectViewerDock: setSelectedMarkerId silently coerces a stale ID",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    dock.setMarkerModel(makeModelWith(std::span<const std::int64_t>{}));

    dock.setSelectedMarkerId(static_cast<std::int64_t>(999));   // never existed
    REQUIRE_FALSE(dock.selectedMarkerId().has_value());
    REQUIRE(stackOf(dock)->currentIndex() == 0);
}

// ---------------------------------------------------------------------------
// Property-page editing — round-trip into model
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: editing the Name field renames the marker",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    wireDockMarkerWritesToModel(dock, model);
    dock.setSelectedMarkerId(*model->idAt(0));

    auto* nameEdit =
        dock.findChild<QLineEdit*>("markerNameEdit");
    REQUIRE(nameEdit != nullptr);
    REQUIRE(nameEdit->text() == "Mark 1");           // initial auto-name

    nameEdit->setText("Hard bit");
    emit nameEdit->editingFinished();

    REQUIRE(model->markers()[0].name == "Hard bit");
}

TEST_CASE("ProjectViewerDock: editing the Position spinbox moves the marker",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    wireDockMarkerWritesToModel(dock, model);
    const auto markerId = *model->idAt(0);
    dock.setSelectedMarkerId(markerId);

    auto* posBox = dock.findChild<QSpinBox*>("markerPositionBox");
    REQUIRE(posBox != nullptr);
    REQUIRE(posBox->value() == 1000);

    // MEMO: simulate "user typed 1500 and pressed Enter" — the dock
    // commits position edits on editingFinished (not on every
    // valueChanged) so leading zeros aren't stripped mid-edit. The
    // test mimics the commit step explicitly.
    posBox->setValue(1500);
    emit posBox->editingFinished();

    REQUIRE(model->markers()[0].sourceMs == 1500);
    // Selection survives the re-position because IDs are stable.
    REQUIRE(dock.selectedMarkerId() == markerId);
}

TEST_CASE("ProjectViewerDock: in-progress spinbox edits don't strip leading zeros",
          "[project-viewer-dock][gui]") {
    // MEMO: regression for the smoke-test bug — at marker position
    // 30004 ms, placing the caret after the 3 and pressing Del to
    // remove it left the user with "4 ms" instead of "0004 ms"
    // because every keystroke round-tripped through the model and
    // re-formatted the text. The fix is editingFinished commits;
    // this test pins that property by mutating the model only when
    // editingFinished fires, not on every value change.
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 30004 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    wireDockMarkerWritesToModel(dock, model);
    dock.setSelectedMarkerId(*model->idAt(0));

    QSignalSpy modelSpy(model.get(), &MarkerModel::changed);
    auto* posBox = dock.findChild<QSpinBox*>("markerPositionBox");
    REQUIRE(posBox != nullptr);

    // Simulate the in-progress text edit by setting the spinbox's
    // value to 4 (what live-parsing would do mid-edit). Without
    // editingFinished firing, the model must NOT mutate.
    posBox->setValue(4);
    REQUIRE(modelSpy.count() == 0);
    REQUIRE(model->markers()[0].sourceMs == 30004);   // unchanged

    // After the user presses Enter / Tab / clicks elsewhere,
    // editingFinished fires and the value is committed. Use a final
    // value of 40004 here to mirror the user's intended end state
    // ("delete '3' from 30004, type '4' in front, end with 40004").
    posBox->setValue(40004);
    emit posBox->editingFinished();
    REQUIRE(model->markers()[0].sourceMs == 40004);
}

TEST_CASE("ProjectViewerDock: setting the spinbox doesn't loop back into rewriting itself",
          "[project-viewer-dock][gui]") {
    // MEMO: regression for the model→widget→model loop. When we
    // populate the property page from the model we set the
    // spinbox value — without the updatingPropertyPage_ guard,
    // that programmatic setValue would also trigger
    // model->setPosition, re-emit changed, and recurse. The
    // guard prevents that.
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);

    QSignalSpy modelSpy(model.get(), &MarkerModel::changed);
    // Selecting the marker triggers refreshPropertyPage which calls
    // QSpinBox::setValue. If the guard were broken, the spinbox's
    // valueChanged would call model->setPosition and emit changed.
    dock.setSelectedMarkerId(*model->idAt(0));

    // No model mutation should have happened — only the dock's
    // internal state changed.
    REQUIRE(modelSpy.count() == 0);
}

// ---------------------------------------------------------------------------
// Model-driven repaints
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: adding a marker grows the tree",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    auto model = makeModelWith(std::span<const std::int64_t>{});
    dock.setMarkerModel(model);
    auto* tree = treeOf(dock);
    REQUIRE(tree->topLevelItem(0)->childCount() == 0);

    (void)model->add(1234);
    REQUIRE(tree->topLevelItem(0)->childCount() == 1);
}

TEST_CASE("ProjectViewerDock: removing the selected marker drops selection",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000, 2000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    const auto secondId = *model->idAt(1);

    dock.setSelectedMarkerId(secondId);
    REQUIRE(dock.selectedMarkerId() == secondId);

    QSignalSpy spy(&dock, &ProjectViewerDock::markerSelectionChanged);
    REQUIRE(model->remove(secondId));

    REQUIRE_FALSE(dock.selectedMarkerId().has_value());
    REQUIRE(stackOf(dock)->currentIndex() == 0);
    REQUIRE(spy.count() >= 1);
}

TEST_CASE("ProjectViewerDock: setPosition redraws the row but keeps selection",
          "[project-viewer-dock][gui]") {
    // MEMO: the row's text contains the timestamp; after
    // setPosition the display must reflect the new ms. Selection
    // (by ID) survives the re-sort.
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000, 2000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    const auto firstId = *model->idAt(0);

    dock.setSelectedMarkerId(firstId);
    REQUIRE(model->setPosition(firstId, 3000));      // moves past second

    auto* tree = treeOf(dock);
    auto* row  = findMarkerRow(*tree, firstId);
    REQUIRE(row != nullptr);
    REQUIRE(row->text(0).contains("3000 ms"));
    REQUIRE(dock.selectedMarkerId() == firstId);
}

TEST_CASE("ProjectViewerDock: setMarkerModel(nullptr) clears the tree",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    dock.setMarkerModel(makeModelWith(std::span<const std::int64_t>{stamps}));
    REQUIRE(treeOf(dock)->topLevelItem(0)->childCount() == 1);

    dock.setMarkerModel(nullptr);
    REQUIRE(treeOf(dock)->topLevelItem(0)->childCount() == 0);
    REQUIRE(stackOf(dock)->currentIndex() == 0);
}

// ---------------------------------------------------------------------------
// Double-click → markerActivated (jump-and-play)
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: double-clicking a marker row emits markerActivated",
          "[project-viewer-dock][gui]") {
    // MEMO: the dock fires a *separate* signal for double-click
    // (markerActivated) on top of the usual selection signal —
    // markerSelectionChanged is passive ("I now show this"),
    // markerActivated is an action request ("seek + play").
    // MainWindow distinguishes them: the first updates mirrors,
    // the second additionally calls Player::seek + Player::play.
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000, 2500 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    const auto secondId = *model->idAt(1);

    auto* tree = treeOf(dock);
    auto* row  = findMarkerRow(*tree, secondId);
    REQUIRE(row != nullptr);

    QSignalSpy spy(&dock, &ProjectViewerDock::markerActivated);
    // QTreeWidget doesn't have an emitDoubleClick helper; we emit
    // its signal directly to simulate the gesture. (Real Qt would
    // also fire currentItemChanged on the first click of the
    // double-click pair, which onTreeCurrentItemChanged handles
    // separately.)
    emit tree->itemDoubleClicked(row, 0);

    REQUIRE(spy.count() == 1);
    REQUIRE(spy.takeFirst().at(0).value<std::int64_t>() == secondId);
}

TEST_CASE("ProjectViewerDock: double-clicking the category header does nothing",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    dock.setMarkerModel(makeModelWith(std::span<const std::int64_t>{}));

    auto* tree     = treeOf(dock);
    auto* category = tree->topLevelItem(0);

    QSignalSpy spy(&dock, &ProjectViewerDock::markerActivated);
    emit tree->itemDoubleClicked(category, 0);
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// Del key on a focused marker entry
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: Del on a focused marker entry fires markerDeleteRequested",
          "[project-viewer-dock][gui][keys]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);
    const auto markerId = *model->idAt(0);

    auto* tree = treeOf(dock);
    auto* row  = findMarkerRow(*tree, markerId);
    tree->setCurrentItem(row);
    dock.show();
    tree->setFocus();
    (void)QTest::qWaitForWindowExposed(&dock);

    QSignalSpy spy(&dock, &ProjectViewerDock::markerDeleteRequested);
    QTest::keyClick(tree, Qt::Key_Delete);

    REQUIRE(spy.count() == 1);
    REQUIRE(spy.takeFirst().at(0).value<std::int64_t>() == markerId);
}

// ---------------------------------------------------------------------------
// Loops category
//
// MEMO[refactor]: each TEST_CASE in this block names one rule the
// loop-aware dock must satisfy. Cross-kind mutual exclusion, the
// armed-glyph row prefix, the editingFinished discipline on the
// range spinboxes, and the Arm-checkbox round-trip are all
// load-bearing — break any one and you'll surface UI regressions
// in the integration tests for commit 5.
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: setLoopModel populates the Loops category",
          "[project-viewer-dock][gui][loops]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1500}, {2000, 3000}
    };
    dock.setLoopModel(makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges}));

    auto* tree = treeOf(dock);
    auto* loopsCategory = tree->topLevelItem(1);
    REQUIRE(loopsCategory->childCount() == 2);
    // Rows store the loop ID in kLoopIdRole; verify the data plumbing.
    REQUIRE(loopsCategory->child(0)->data(0, kLoopIdRole).toLongLong() != 0);
}

TEST_CASE("ProjectViewerDock: clicking a loop row emits loopSelectionChanged + shows loop page",
          "[project-viewer-dock][gui][loops]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    const auto loopId = *model->idAt(0);

    QSignalSpy selSpy(&dock, &ProjectViewerDock::loopSelectionChanged);
    auto* tree = treeOf(dock);
    auto* row  = findLoopRow(*tree, loopId);
    REQUIRE(row);
    tree->setCurrentItem(row);

    REQUIRE(selSpy.count() == 1);
    REQUIRE(*dock.selectedLoopId() == loopId);
    // Property stack: 0 = no-selection, 1 = marker, 2 = loop.
    REQUIRE(stackOf(dock)->currentIndex() == 2);
}

TEST_CASE("ProjectViewerDock: clicking a loop clears any active marker selection",
          "[project-viewer-dock][gui][loops]") {
    // MEMO: cross-kind mutual exclusion in the dock — this is what
    // keeps the property page focused on a single artifact at a time.
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t markerStamps[] = { 500 };
    auto markerModel = makeModelWith(std::span<const std::int64_t>{markerStamps});
    dock.setMarkerModel(markerModel);

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loopModel = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(loopModel);

    // Seed marker selection.
    dock.setSelectedMarkerId(*markerModel->idAt(0));
    REQUIRE(dock.selectedMarkerId().has_value());

    QSignalSpy markerSpy(&dock, &ProjectViewerDock::markerSelectionChanged);
    QSignalSpy loopSpy  (&dock, &ProjectViewerDock::loopSelectionChanged);

    auto* tree = treeOf(dock);
    tree->setCurrentItem(findLoopRow(*tree, *loopModel->idAt(0)));

    REQUIRE(dock.selectedLoopId().has_value());
    REQUIRE_FALSE(dock.selectedMarkerId().has_value());
    REQUIRE(markerSpy.count() == 1);
    REQUIRE(loopSpy.count()   == 1);
}

TEST_CASE("ProjectViewerDock: setSelectedLoopId(nullopt) returns to 'no selection'",
          "[project-viewer-dock][gui][loops]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    dock.setSelectedLoopId(*model->idAt(0));
    REQUIRE(stackOf(dock)->currentIndex() == 2);

    dock.setSelectedLoopId(std::nullopt);
    REQUIRE_FALSE(dock.selectedLoopId().has_value());
    REQUIRE(stackOf(dock)->currentIndex() == 0);
}

TEST_CASE("ProjectViewerDock: double-clicking a loop row emits loopActivated",
          "[project-viewer-dock][gui][loops]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    const auto loopId = *model->idAt(0);

    QSignalSpy spy(&dock, &ProjectViewerDock::loopActivated);
    auto* tree = treeOf(dock);
    emit tree->itemDoubleClicked(findLoopRow(*tree, loopId), 0);
    // Note: emitting the signal directly is the dock-test idiom in
    // this file (used elsewhere) — drives Qt's slot wiring without
    // needing a real mouse double-click on a hidden widget.

    REQUIRE(spy.count() == 1);
    REQUIRE(spy.takeFirst().at(0).value<std::int64_t>() == loopId);
}

TEST_CASE("ProjectViewerDock: Del on a focused loop entry fires loopDeleteRequested",
          "[project-viewer-dock][gui][loops][keys]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    const auto loopId = *model->idAt(0);

    auto* tree = treeOf(dock);
    auto* row  = findLoopRow(*tree, loopId);
    tree->setCurrentItem(row);
    dock.show();
    tree->setFocus();
    (void)QTest::qWaitForWindowExposed(&dock);

    QSignalSpy spy(&dock, &ProjectViewerDock::loopDeleteRequested);
    QTest::keyClick(tree, Qt::Key_Delete);

    REQUIRE(spy.count() == 1);
    REQUIRE(spy.takeFirst().at(0).value<std::int64_t>() == loopId);
}

TEST_CASE("ProjectViewerDock: editing loop Start round-trips through setRange",
          "[project-viewer-dock][gui][loops]") {
    // MEMO: pins the editingFinished → setRange path. Mirrors the
    // marker Position round-trip test (same leading-zero rationale).
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    wireDockLoopWritesToModel(dock, model);
    const auto loopId = *model->idAt(0);
    dock.setSelectedLoopId(loopId);

    auto* startBox = dock.findChild<QSpinBox*>("loopStartBox");
    REQUIRE(startBox);
    startBox->setValue(1500);
    emit startBox->editingFinished();   // simulate Enter / focus-loss

    REQUIRE(model->loops()[0].startMs == 1500);
    REQUIRE(model->loops()[0].endMs   == 2000);
}

TEST_CASE("ProjectViewerDock: editing loop End round-trips through setRange",
          "[project-viewer-dock][gui][loops]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    wireDockLoopWritesToModel(dock, model);
    dock.setSelectedLoopId(*model->idAt(0));

    auto* endBox = dock.findChild<QSpinBox*>("loopEndBox");
    REQUIRE(endBox);
    endBox->setValue(2500);
    emit endBox->editingFinished();

    REQUIRE(model->loops()[0].endMs == 2500);
}

// MEMO: per-loop "Pause" spinbox was removed in issue #16 — the
// pause-between-repeats is now a global MainWindow setting.
// The previous round-trip test for setPauseMs lived here; the new
// global-pre-roll tests live in test_main_window.cpp.

TEST_CASE("ProjectViewerDock: editing the loop Name round-trips through rename",
          "[project-viewer-dock][gui][loops]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    wireDockLoopWritesToModel(dock, model);
    dock.setSelectedLoopId(*model->idAt(0));

    auto* nameEdit = dock.findChild<QLineEdit*>("loopNameEdit");
    REQUIRE(nameEdit);
    nameEdit->setText("Hard turn");
    emit nameEdit->editingFinished();
    REQUIRE(model->loops()[0].name == "Hard turn");
}

TEST_CASE("ProjectViewerDock: Arm checkbox toggle emits loopArmToggleRequested",
          "[project-viewer-dock][gui][loops]") {
    // MEMO: the dock relays the toggle to MainWindow rather than
    // flipping armedLoopId_ itself — transport state is the
    // canonical source of truth, and MainWindow pushes it back via
    // setArmedLoopId() once the transport has actually changed.
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    const auto loopId = *model->idAt(0);
    dock.setSelectedLoopId(loopId);

    auto* armCheck = dock.findChild<QCheckBox*>("loopArmedCheck");
    REQUIRE(armCheck);
    REQUIRE_FALSE(armCheck->isChecked());

    QSignalSpy spy(&dock, &ProjectViewerDock::loopArmToggleRequested);
    armCheck->setChecked(true);

    REQUIRE(spy.count() == 1);
    const auto args = spy.takeFirst();
    REQUIRE(args.at(0).value<std::int64_t>() == loopId);
    REQUIRE(args.at(1).toBool() == true);
}

TEST_CASE("ProjectViewerDock: setArmedLoopId updates Arm checkbox without re-emitting",
          "[project-viewer-dock][gui][loops]") {
    // MEMO: load-bearing — when MainWindow pushes the armed state
    // back to the dock (after acting on a double-click or external
    // arm), the checkbox sync must NOT re-emit
    // loopArmToggleRequested. Otherwise we'd loop forever between
    // dock→MainWindow→dock.
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    const auto loopId = *model->idAt(0);
    dock.setSelectedLoopId(loopId);

    QSignalSpy spy(&dock, &ProjectViewerDock::loopArmToggleRequested);
    dock.setArmedLoopId(loopId);

    auto* armCheck = dock.findChild<QCheckBox*>("loopArmedCheck");
    REQUIRE(armCheck->isChecked());
    REQUIRE(spy.count() == 0);            // no re-emit during sync
    REQUIRE(*dock.armedLoopId() == loopId);
}

TEST_CASE("ProjectViewerDock: armed loop row gets a glyph prefix",
          "[project-viewer-dock][gui][loops]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    const auto loopId = *model->idAt(0);

    auto* tree = treeOf(dock);
    auto* row  = findLoopRow(*tree, loopId);
    const QString unarmedText = row->text(0);

    dock.setArmedLoopId(loopId);
    auto* armedRow = findLoopRow(*tree, loopId);
    const QString armedText = armedRow->text(0);

    REQUIRE(armedText != unarmedText);
    REQUIRE(armedText.startsWith(QString::fromUtf8("▶")));

    dock.setArmedLoopId(std::nullopt);
    auto* disarmedRow = findLoopRow(*tree, loopId);
    REQUIRE(disarmedRow->text(0) == unarmedText);
}

TEST_CASE("ProjectViewerDock: loop setRange keeps selection alive across re-sort",
          "[project-viewer-dock][gui][loops]") {
    // MEMO: stable-ID survival, mirror of the marker setPosition test.
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1000}, {2000, 2500}
    };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    const auto firstId = *model->idAt(0);
    dock.setSelectedLoopId(firstId);

    REQUIRE(model->setRange(firstId, 3000, 3500));   // moves past second
    REQUIRE(*dock.selectedLoopId() == firstId);
    REQUIRE(*model->indexOf(firstId) == 1);
}

TEST_CASE("ProjectViewerDock: Ctrl+click on a marker row emits loopAnchorAddRequested",
          "[project-viewer-dock][gui][loops][secondary-anchor]") {
    // MEMO: load-bearing — the dock's Ctrl+click gesture is the
    // mirror of the score widgets' Ctrl+click. It fires a signal
    // *before* the click changes selection, so MainWindow's handler
    // can capture the prior primary's ms.
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 500, 1500 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);

    dock.show();
    (void)QTest::qWaitForWindowExposed(&dock);
    auto* tree = treeOf(dock);
    auto* secondRow = findMarkerRow(*tree, *model->idAt(1));
    REQUIRE(secondRow);

    // Select first marker so there's a "prior primary" to capture.
    dock.setSelectedMarkerId(*model->idAt(0));

    QSignalSpy addSpy(&dock, &ProjectViewerDock::loopAnchorAddRequested);
    QSignalSpy clearSpy(&dock,
                        &ProjectViewerDock::loopAnchorClearRequested);

    // Drive a Ctrl+left-click via QTest. The viewport is the
    // QTreeWidget's actual mouse target.
    const QRect rowRect = tree->visualItemRect(secondRow);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::ControlModifier, rowRect.center());

    REQUIRE(addSpy.count()   == 1);
    REQUIRE(clearSpy.count() == 0);
}

TEST_CASE("ProjectViewerDock: plain click on a marker emits loopAnchorClearRequested",
          "[project-viewer-dock][gui][loops][secondary-anchor]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[] = { 1000 };
    auto model = makeModelWith(std::span<const std::int64_t>{stamps});
    dock.setMarkerModel(model);

    dock.show();
    (void)QTest::qWaitForWindowExposed(&dock);
    auto* tree = treeOf(dock);
    auto* row  = findMarkerRow(*tree, *model->idAt(0));

    QSignalSpy addSpy(&dock, &ProjectViewerDock::loopAnchorAddRequested);
    QSignalSpy clearSpy(&dock,
                        &ProjectViewerDock::loopAnchorClearRequested);

    const QRect rowRect = tree->visualItemRect(row);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::NoModifier, rowRect.center());

    REQUIRE(addSpy.count()   == 0);
    REQUIRE(clearSpy.count() >= 1);
}

TEST_CASE("ProjectViewerDock: Ctrl+click on a loop row emits clear, not add",
          "[project-viewer-dock][gui][loops][secondary-anchor]") {
    // MEMO: loops are regions, not anchors — Ctrl+click on a loop
    // row is treated as "exit loop-creation mode", same as a plain
    // click. Pinning this rule keeps a future visual-only refactor
    // from accidentally promoting loops to anchor candidates.
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);

    dock.show();
    (void)QTest::qWaitForWindowExposed(&dock);
    auto* tree = treeOf(dock);
    auto* row  = findLoopRow(*tree, *model->idAt(0));

    QSignalSpy addSpy(&dock, &ProjectViewerDock::loopAnchorAddRequested);
    QSignalSpy clearSpy(&dock,
                        &ProjectViewerDock::loopAnchorClearRequested);

    const QRect rowRect = tree->visualItemRect(row);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::ControlModifier, rowRect.center());

    REQUIRE(addSpy.count()   == 0);
    REQUIRE(clearSpy.count() >= 1);
}

TEST_CASE("ProjectViewerDock: removing the selected loop clears selection",
          "[project-viewer-dock][gui][loops]") {
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    const auto loopId = *model->idAt(0);
    dock.setSelectedLoopId(loopId);

    QSignalSpy spy(&dock, &ProjectViewerDock::loopSelectionChanged);
    REQUIRE(model->remove(loopId));
    REQUIRE_FALSE(dock.selectedLoopId().has_value());
    REQUIRE(spy.count() >= 1);
}

TEST_CASE("ProjectViewerDock: countdown widget is global, visibility tracks prerollEnabled",
          "[project-viewer-dock][gui][countdown][preroll]") {
    // MEMO[issue #16]: the countdown widget used to live in the
    // loop property page and reflect that loop's armed state. With
    // global pre-roll, it now lives at the bottom of the dock and
    // its visibility is driven by setPrerollEnabled. Loop selection
    // / arming no longer affects it.
    qtApp();
    ProjectViewerDock dock;
    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1500}, {2000, 3000}
    };
    auto model = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    dock.setLoopModel(model);
    dock.show();
    (void)QTest::qWaitForWindowExposed(&dock);

    auto* countdown =
        dock.findChild<fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    REQUIRE(countdown);

    // Default: pre-roll is disabled, widget is hidden.
    REQUIRE_FALSE(dock.prerollEnabled());
    REQUIRE_FALSE(countdown->isVisible());

    // Enable: widget becomes visible.
    dock.setPrerollEnabled(true);
    REQUIRE(dock.prerollEnabled());
    REQUIRE(countdown->isVisible());

    // Selecting / arming loops while pre-roll is on doesn't
    // change visibility (the global widget isn't tied to a
    // specific loop anymore).
    const auto firstId = *model->idAt(0);
    dock.setSelectedLoopId(firstId);
    REQUIRE(countdown->isVisible());
    dock.setArmedLoopId(firstId);
    REQUIRE(countdown->isVisible());
    dock.setArmedLoopId(std::nullopt);
    REQUIRE(countdown->isVisible());

    // Disable: widget hidden again.
    dock.setPrerollEnabled(false);
    REQUIRE_FALSE(countdown->isVisible());
}

TEST_CASE("ProjectViewerDock: disabling pre-roll mid-countdown cancels the widget",
          "[project-viewer-dock][gui][countdown][preroll]") {
    // MEMO: leaving a frozen ring visible after the user toggles
    // off would be confusing. setPrerollEnabled(false) cancels any
    // active countdown before hiding.
    qtApp();
    ProjectViewerDock dock;
    auto model = std::make_shared<LoopModel>();
    dock.setLoopModel(model);

    auto* countdown =
        dock.findChild<fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    REQUIRE(countdown);

    dock.setPrerollEnabled(true);
    dock.startCountdown(5000);
    REQUIRE(countdown->isCountingDown());

    dock.setPrerollEnabled(false);
    REQUIRE_FALSE(countdown->isCountingDown());
}

// ---------------------------------------------------------------------------
// Notes section (Step 6.1)
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: setNoteModel does NOT add a tree row "
          "(notes live on the staff in step 6.2)",
          "[project-viewer-dock][gui][notes]") {
    // MEMO[#step6.2]: notes are anonymous (pitch + interval, no
    // name), and the staff already paints every one as a bar. A
    // tabular list would just duplicate that. The dock keeps only
    // the Note property page (driven by the staff selection) and
    // the keyboard-fallback "New Note ..." button.
    qtApp();
    ProjectViewerDock dock;
    auto* tree = treeOf(dock);

    auto notes = std::make_shared<NoteModel>();
    notes->add(1000, 1400, 64, "First");          // E4
    notes->add(2000, 2400, 69, "Second");         // A4

    dock.setNoteModel(notes);

    // Markers + Loops only. No "Notes" category.
    REQUIRE(tree->topLevelItemCount() == 2);
    REQUIRE(tree->topLevelItem(0)->text(0) == "Markers");
    REQUIRE(tree->topLevelItem(1)->text(0) == "Loops");
}

TEST_CASE("ProjectViewerDock: Add Note button is disabled without a NoteModel",
          "[project-viewer-dock][gui][notes]") {
    qtApp();
    ProjectViewerDock dock;
    auto* button = dock.findChild<QPushButton*>("addNoteButton");
    REQUIRE(button != nullptr);
    REQUIRE_FALSE(button->isEnabled());

    dock.setNoteModel(std::make_shared<NoteModel>());
    REQUIRE(button->isEnabled());
}

TEST_CASE("ProjectViewerDock: Add Note button emits noteAddRequested",
          "[project-viewer-dock][gui][notes]") {
    qtApp();
    ProjectViewerDock dock;
    dock.setNoteModel(std::make_shared<NoteModel>());
    auto* button = dock.findChild<QPushButton*>("addNoteButton");

    QSignalSpy spy(&dock, &ProjectViewerDock::noteAddRequested);
    button->click();
    REQUIRE(spy.count() == 1);
}

TEST_CASE("ProjectViewerDock: selecting a note enters Editing mode "
          "and shows note property page",
          "[project-viewer-dock][gui][notes]") {
    // MEMO[#step6.2]: clicking a note row puts the dock into
    // Editing mode. Property page reflects the model's current
    // values; edits commit live (per-field). The Add-Note button
    // is hidden in Editing mode — the property page is the whole
    // UI, same as markers / loops. No per-note Name field exists.
    qtApp();
    ProjectViewerDock dock;
    auto* stack = stackOf(dock);
    auto* pitchEdit = dock.findChild<QLineEdit*>("notePitchEdit");
    auto* midiLabel = dock.findChild<QLabel*>("notePitchMidiLabel");
    auto* startBox  = dock.findChild<QSpinBox*>("noteStartBox");
    auto* endBox    = dock.findChild<QSpinBox*>("noteEndBox");
    auto* durLabel  = dock.findChild<QLabel*>("noteDurationLabel");
    auto* button    = dock.findChild<QPushButton*>("addNoteButton");
    REQUIRE(pitchEdit != nullptr);
    REQUIRE(midiLabel != nullptr);
    REQUIRE(startBox  != nullptr);
    REQUIRE(endBox    != nullptr);
    REQUIRE(durLabel  != nullptr);
    REQUIRE(button    != nullptr);
    dock.show();    // needed so isVisible() reflects setVisible()

    auto notes = std::make_shared<NoteModel>();
    const auto id = notes->add(1000, 1400, 64);   // E4, 400 ms
    dock.setNoteModel(notes);

    dock.setSelectedNoteId(id);

    // Page index 3 is the Note page (kPageNote in the dock impl).
    REQUIRE(stack->currentIndex() == 3);
    REQUIRE(pitchEdit->text() == "E4");
    REQUIRE(midiLabel->text().contains("64"));
    REQUIRE(startBox->value() == 1000);
    REQUIRE(endBox->value() == 1400);
    REQUIRE(durLabel->text().startsWith("400"));
    REQUIRE_FALSE(button->isVisible());   // Editing → button hidden
}

TEST_CASE("ProjectViewerDock: invalid pitch input reverts; valid input commits live",
          "[project-viewer-dock][gui][notes]") {
    // MEMO[#step6.2]: range + parse validation in
    // NoteModel::isAcceptedPitch is what the dock defers to. In the
    // live-commit Editing mode, an out-of-range or unparseable
    // entry reverts the line edit to the buffer's current value
    // and the model stays unchanged. A valid entry commits live
    // via noteCommitChangesRequested → MainWindow → setPitch.
    qtApp();
    ProjectViewerDock dock;
    auto notes = std::make_shared<NoteModel>();
    const auto id = notes->add(1000, 1400, 64);
    dock.setNoteModel(notes);

    // Mirror the dock's commit-changes signal through to the model
    // so the test sees real-world live-commit behaviour (this
    // wiring lives in MainWindow in production).
    QObject::connect(&dock,
        &ProjectViewerDock::noteCommitChangesRequested,
        &dock, [&notes](std::int64_t cid,
                        std::int64_t startMs,
                        std::int64_t endMs,
                        int          midi) {
            (void)startMs; (void)endMs;
            notes->setPitch(cid, midi);
        });

    dock.setSelectedNoteId(id);

    auto* pitchEdit = dock.findChild<QLineEdit*>("notePitchEdit");

    pitchEdit->setText("D2");                       // below violin range
    emit pitchEdit->editingFinished();
    REQUIRE(pitchEdit->text() == "E4");             // reverted to buffer
    REQUIRE(notes->notes()[0].midi == 64);          // model untouched

    pitchEdit->setText("F7");                       // above violin range
    emit pitchEdit->editingFinished();
    REQUIRE(pitchEdit->text() == "E4");
    REQUIRE(notes->notes()[0].midi == 64);

    pitchEdit->setText("Z9");                       // garbage
    emit pitchEdit->editingFinished();
    REQUIRE(pitchEdit->text() == "E4");
    REQUIRE(notes->notes()[0].midi == 64);

    pitchEdit->setText("C#5");                      // accidental — accepted
    emit pitchEdit->editingFinished();
    REQUIRE(pitchEdit->text() == "C#5");
    REQUIRE(notes->notes()[0].midi == 73);          // live-commit reached model

    pitchEdit->setText("A4");                       // valid natural
    emit pitchEdit->editingFinished();
    REQUIRE(pitchEdit->text() == "A4");
    REQUIRE(notes->notes()[0].midi == 69);
}

TEST_CASE("ProjectViewerDock: state machine — button label cycles Empty → "
          "NewDraft → Empty",
          "[project-viewer-dock][gui][notes][state-machine]") {
    qtApp();
    ProjectViewerDock dock;
    auto notes = std::make_shared<NoteModel>();
    dock.setNoteModel(notes);
    auto* button = dock.findChild<QPushButton*>("addNoteButton");
    REQUIRE(button->text() == "New Note ...");

    // Enter NewDraft. Button label transitions; model untouched.
    dock.enterNoteDraftMode(1000, 1400, 64);
    REQUIRE(button->text() == "Add Note");
    REQUIRE(notes->empty());

    // Exit (via exitNoteMode — same path the user takes by clicking
    // outside or a different row). Button returns to Empty.
    dock.exitNoteMode();
    REQUIRE(button->text() == "New Note ...");
    REQUIRE(notes->empty());
}

TEST_CASE("ProjectViewerDock: state machine — selecting a note hides the "
          "Add-Note button (Editing has no button)",
          "[project-viewer-dock][gui][notes][state-machine]") {
    qtApp();
    ProjectViewerDock dock;
    auto notes = std::make_shared<NoteModel>();
    const auto id = notes->add(1000, 1400, 64);
    dock.setNoteModel(notes);
    auto* button = dock.findChild<QPushButton*>("addNoteButton");
    dock.show();

    dock.setSelectedNoteId(id);
    REQUIRE_FALSE(button->isVisible());

    dock.setSelectedNoteId(std::nullopt);
    REQUIRE(button->isVisible());
    REQUIRE(button->text() == "New Note ...");
}

TEST_CASE("ProjectViewerDock: draft-mode field edits update buffer, "
          "click commits to model",
          "[project-viewer-dock][gui][notes][state-machine]") {
    qtApp();
    ProjectViewerDock dock;
    auto notes = std::make_shared<NoteModel>();
    dock.setNoteModel(notes);

    QSignalSpy commitSpy(&dock,
        &ProjectViewerDock::noteCommitNewRequested);

    dock.enterNoteDraftMode(1000, 1400, 64);
    auto* pitchEdit = dock.findChild<QLineEdit*>("notePitchEdit");
    auto* button    = dock.findChild<QPushButton*>("addNoteButton");

    // Edit pitch in draft. Buffer should update; model untouched.
    pitchEdit->setText("G4");
    emit pitchEdit->editingFinished();
    REQUIRE(notes->empty());

    // Click button to commit. Signal carries the buffered values.
    button->click();
    REQUIRE(commitSpy.count() == 1);
    auto args = commitSpy.takeFirst();
    REQUIRE(args.at(0).value<std::int64_t>() == 1000);   // startMs
    REQUIRE(args.at(1).value<std::int64_t>() == 1400);   // endMs
    REQUIRE(args.at(2).toInt()              == 67);      // G4

    // After commit, exit back to Empty.
    REQUIRE(button->text() == "New Note ...");
}

TEST_CASE("ProjectViewerDock: Editing-mode pitch edit emits commit-changes "
          "signal live (no Apply button)",
          "[project-viewer-dock][gui][notes][state-machine]") {
    // MEMO[#step6.2]: switched from buffered "Apply Changes" to
    // live commit per field — pitchEdit::editingFinished now fires
    // noteCommitChangesRequested directly. The button is hidden in
    // Editing mode; no click-to-commit gesture exists.
    qtApp();
    ProjectViewerDock dock;
    auto notes = std::make_shared<NoteModel>();
    const auto id = notes->add(1000, 1400, 64);   // E4
    dock.setNoteModel(notes);
    dock.setSelectedNoteId(id);

    QSignalSpy commitSpy(&dock,
        &ProjectViewerDock::noteCommitChangesRequested);

    auto* pitchEdit = dock.findChild<QLineEdit*>("notePitchEdit");

    pitchEdit->setText("F5");
    emit pitchEdit->editingFinished();

    REQUIRE(commitSpy.count() == 1);
    auto args = commitSpy.takeFirst();
    REQUIRE(args.at(0).value<std::int64_t>() == id);
    REQUIRE(args.at(3).toInt()              == 77);   // F5
}

TEST_CASE("ProjectViewerDock: selecting a row while in NewDraft discards "
          "the draft",
          "[project-viewer-dock][gui][notes][state-machine]") {
    qtApp();
    ProjectViewerDock dock;
    auto notes = std::make_shared<NoteModel>();
    const auto id = notes->add(2000, 2400, 71);
    dock.setNoteModel(notes);
    auto* button = dock.findChild<QPushButton*>("addNoteButton");
    dock.show();

    dock.enterNoteDraftMode(500, 900, 60);
    REQUIRE(button->isVisible());
    REQUIRE(button->text() == "Add Note");

    // Selecting the existing note's row discards the draft and
    // switches to Editing — button hides (live-commit, no Apply).
    dock.setSelectedNoteId(id);
    REQUIRE_FALSE(button->isVisible());
}

TEST_CASE("ProjectViewerDock: selecting a note clears marker / loop selection",
          "[project-viewer-dock][gui][notes]") {
    qtApp();
    ProjectViewerDock dock;
    const std::int64_t stamps[]   = { 1000 };
    auto markers = makeModelWith(std::span<const std::int64_t>{stamps});
    auto loops   = makeLoopModelWith(
        std::span<const std::pair<std::int64_t, std::int64_t>>{
            { {2000, 3000} } });
    auto notes = std::make_shared<NoteModel>();
    const auto noteId = notes->add(4000, 4400, 64);

    dock.setMarkerModel(markers);
    dock.setLoopModel(loops);
    dock.setNoteModel(notes);

    const auto markerId = markers->idAt(0).value();
    dock.setSelectedMarkerId(markerId);
    REQUIRE(dock.selectedMarkerId().has_value());

    QSignalSpy markerSpy(&dock, &ProjectViewerDock::markerSelectionChanged);
    QSignalSpy noteSpy  (&dock, &ProjectViewerDock::noteSelectionChanged);

    dock.setSelectedNoteId(noteId);
    REQUIRE(dock.selectedNoteId() == noteId);
    REQUIRE_FALSE(dock.selectedMarkerId().has_value());
    REQUIRE(markerSpy.count() == 1);
    REQUIRE(noteSpy.count()   == 1);
}

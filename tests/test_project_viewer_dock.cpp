// Tests for ProjectViewerDock — the right-side artifact viewer.
//
// MEMO[refactor]: each TEST_CASE has a one-line comment naming the
// rule it pins. When the dock grows new categories (loops in PR B,
// notes / sections later), don't be afraid to refactor these tests
// — but read each comment first so you preserve the *property*
// being checked rather than the literal value.

#include "qt_test_app.h"
#include "score/MarkerModel.h"
#include "ui/ProjectViewerDock.h"

#include <QLineEdit>
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

using fiddler::score::MarkerModel;
using fiddler::test::qtApp;
using fiddler::ui::ProjectViewerDock;

namespace {

constexpr int kMarkerIdRole = Qt::UserRole + 1;   // mirrors dock impl

std::shared_ptr<MarkerModel>
makeModelWith(std::span<const std::int64_t> stamps) {
    auto m = std::make_shared<MarkerModel>();
    for (auto ms : stamps) (void)m->add(ms);
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

// Locate the marker entry in the tree whose UserRole+1 == id.
// Returns nullptr if not found. Helper for tests that need to
// click / inspect a specific entry.
QTreeWidgetItem*
findMarkerRow(QTreeWidget& tree, std::int64_t id) {
    // The Markers category is the only top-level item for now.
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

} // namespace

// ---------------------------------------------------------------------------
// Empty / model-less state
// ---------------------------------------------------------------------------

TEST_CASE("ProjectViewerDock: with no model, tree is empty and 'no selection' shows",
          "[project-viewer-dock][gui]") {
    qtApp();
    ProjectViewerDock dock;
    auto* tree = treeOf(dock);

    REQUIRE(tree->topLevelItemCount() == 1);          // just the Markers category
    REQUIRE(tree->topLevelItem(0)->childCount() == 0);

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

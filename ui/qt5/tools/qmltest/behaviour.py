"""Behavioural test for the collapse logic in ScriptOutliner.

Instantiation proves the bindings resolve; it does not prove collapsing works.
This drives the component the way a user would -- call toggleGroup(), then read
back the actual delegate geometry -- and asserts the expected outcome.

Runs headless with no GL: we instantiate ScriptOutliner directly (not through
main.qml) so there is no ShaderEffectSource/FastBlur in the tree. A ListView
still creates and lays out its delegates without a scene graph, which is all we
need to read heights from.
"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("QT_QUICK_BACKEND", "software")

from PySide2.QtCore import QTimer, QUrl
from PySide2.QtGui import QGuiApplication
from PySide2.QtQuick import QQuickView

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mockmodel import DATA, MockModel  # same mock the render harness uses

qml_dir = os.path.abspath(sys.argv[1])
app = QGuiApplication(sys.argv[:1])

# Must be a real QQuickView, not a bare QQmlComponent. A windowless component
# creates its delegates but never runs the layout/polish pass, so
# ListView.contentHeight stays pinned at the view height and the collapse
# assertion silently passes on a meaningless number. With a window it reports
# real geometry.
view = QQuickView()
view.engine().addImportPath(qml_dir)

# Held in a variable on purpose: setContextProperty does not take ownership,
# so passing MockModel() inline lets Python collect it and the view ends up
# with an empty model (count == 0).
model = MockModel()
view.rootContext().setContextProperty("scriptModel", model)

warnings = []
view.engine().warnings.connect(lambda ws: warnings.extend(w.toString() for w in ws))

# Wrap ScriptOutliner so it gets a real size -- a zero-sized ListView creates
# no delegates and every height reads back 0, which would look like a pass.
# The wrapper exposes plain-typed JS shims rather than the ScriptOutliner
# object itself: PySide2 cannot marshal a QML-defined type back to Python
# ("Can't find converter for 'ScriptOutliner_QMLTYPE_*'"), but bool/int/string
# return values cross fine.
wrapper = """
import QtQuick 2.15
Item {
    width: 260; height: 600
    ScriptOutliner { id: ol; anchors.fill: parent; model: scriptModel }

    function expanded(g)    { return ol.isExpanded(g); }
    function toggle(g)      { ol.toggleGroup(g); }
    function hoverDesc()    { return ol.hoverDesc; }

    // Locate the ListView by objectName. Duck-typing on contentHeight does
    // not work: Text also has a contentHeight, so a property probe silently
    // matches the wrong node and reports a plausible-looking number.
    function findList() {
        for (var i = 0; i < ol.children.length; ++i)
            if (ol.children[i].objectName === "scriptListView")
                return ol.children[i];
        return null;
    }
    // contentHeight is the real proof that collapsed rows stop taking space:
    // isExpanded() reports intent, this reports layout.
    function listContentHeight() { var l = findList(); return l ? l.contentHeight : -1; }
    function listCount()         { var l = findList(); return l ? l.count : -1; }
    // Identity probe: stash the current map, then report whether the property
    // still points at that same object after a toggle. If toggleGroup mutated
    // in place instead of reassigning, bindings would never re-evaluate.
    property var _stashed: null
    function stash()        { _stashed = ol.expandedGroups; }
    function isSameObject() { return _stashed === ol.expandedGroups; }
}
"""
# Written next to the real QML: implicit directory imports (finding
# ScriptOutliner.qml as a sibling) resolve relative to this file's location.
probe_path = os.path.join(qml_dir, "_bhv.qml")
with open(probe_path, "w", encoding="utf-8") as fh:
    fh.write(wrapper)
try:
    view.setSource(QUrl.fromLocalFile(probe_path))
    if view.status() == QQuickView.Error:
        print("[FAIL] wrapper did not load")
        for e in view.errors():
            print("   ", e.toString())
        sys.exit(1)
    view.show()
    root = view.rootObject()
    if root is None:
        print("[FAIL] wrapper produced no root object")
        sys.exit(1)
finally:
    os.remove(probe_path)

failures = []


def check(label, got, want):
    ok = got == want
    print(f"  [{'ok' if ok else 'FAIL'}] {label}: got {got!r}, want {want!r}")
    if not ok:
        failures.append(label)


groups = sorted({d["group"] for d in DATA})
print(f"groups in mock data: {groups}")

# --- default state: everything expanded ---
print("\n1. default state")
for g in groups:
    check(f"isExpanded({g})", root.expanded(g), True)

# --- groupCount reaches the model ---
print("\n2. groupCount via model")
for g in groups:
    want = sum(1 for d in DATA if d["group"] == g)
    check(f"groupCount({g})", model.groupCount(g), want)

# --- toggle collapses exactly one group ---
print("\n3. toggleGroup('Image') collapses only Image")
root.toggle("Image")
check("isExpanded(Image)", root.expanded("Image"), False)
for g in groups:
    if g != "Image":
        check(f"isExpanded({g}) unaffected", root.expanded(g), True)

# --- toggling back restores ---
print("\n4. toggleGroup('Image') again restores it")
root.toggle("Image")
check("isExpanded(Image)", root.expanded("Image"), True)

# --- collapsing every group, then restoring, leaves no residue ---
print("\n5. collapse all then restore all")
for g in groups:
    root.toggle(g)
for g in groups:
    check(f"isExpanded({g}) collapsed", root.expanded(g), False)
for g in groups:
    root.toggle(g)
for g in groups:
    check(f"isExpanded({g}) restored", root.expanded(g), True)

# --- the expandedGroups map must be replaced, not mutated in place ---
# If it were mutated, bindings would never re-evaluate and the UI would not
# update. Identity change is the observable proxy for that.
print("\n6. expandedGroups is replaced (so bindings re-evaluate)")
root.stash()
root.toggle("Image")
check("map object identity changed", root.isSameObject(), False)
root.toggle("Image")

# --- hoverDesc starts empty ---
print("\n7. hoverDesc default")
check("hoverDesc", root.hoverDesc(), "")

# --- layout: collapsed rows must actually stop occupying space ---
# Row heights animate (Behavior on height), so the event loop has to run for
# the animation to finish before contentHeight settles. settle() pumps events
# for well past the 150ms duration.
print("\n8. layout: contentHeight shrinks when collapsed")


def settle(ms=600):
    loop_end = QTimer()
    loop_end.setSingleShot(True)
    loop_end.timeout.connect(app.quit)
    loop_end.start(ms)
    app.exec_()


settle()
check("ListView row count == mock rows", root.listCount(), len(DATA))
full = root.listContentHeight()
print(f"  contentHeight, all expanded: {full}")
if full <= 0:
    print("  [FAIL] could not read contentHeight (ListView not found or unlaid-out)")
    failures.append("contentHeight unreadable")
else:
    for g in groups:
        root.toggle(g)
    settle()
    collapsed = root.listContentHeight()
    print(f"  contentHeight, all collapsed: {collapsed}")
    ok = collapsed < full
    print(f"  [{'ok' if ok else 'FAIL'}] collapsed < expanded")
    if not ok:
        failures.append("collapse does not reduce contentHeight")

    # With every group collapsed, only the section headers plus the footer
    # remain. Each header is rowGroupHeight + groupGap = 34 + 4 = 38, and the
    # footer is fadeHeight = 24.
    expected = len(groups) * 38 + 24
    near = abs(collapsed - expected) <= 2
    print(f"  [{'ok' if near else 'FAIL'}] collapsed height ~= "
          f"{len(groups)}*38 + 24 = {expected} (got {collapsed})")
    if not near:
        failures.append(f"collapsed height {collapsed} != expected ~{expected}")

    for g in groups:
        root.toggle(g)
    settle()
    restored = root.listContentHeight()
    print(f"  contentHeight, restored: {restored}")
    back = abs(restored - full) <= 2
    print(f"  [{'ok' if back else 'FAIL'}] restored ~= original")
    if not back:
        failures.append("restore does not return to original height")

print()
if warnings:
    print(f"[warn] {len(warnings)} binding warning(s):")
    for w in warnings:
        print("     ", w)
    failures.append("binding warnings")

if failures:
    print(f"[FAIL] {len(failures)} check(s) failed: {failures}")
    sys.exit(1)
print("[ok] all behavioural checks passed")
sys.exit(0)

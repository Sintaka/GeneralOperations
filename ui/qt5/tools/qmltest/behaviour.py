"""Behavioural test for the two-level collapse logic in ScriptOutliner.

Instantiation proves the bindings resolve; it does not prove collapsing works.
This drives the component the way a user would -- call toggleGroup() on the
two key kinds (major = group's first segment, e.g. "Image"; sub = full group,
e.g. "Image/Edit"), then read back the realized row geometry and asserts the
expected stack: no overlap, no hole, contentHeight correct -- sampled AFTER
animations settle AND mid-animation (the positioner must reflow every frame).

Layout mechanism note: ScriptOutliner is a Flickable + Column + Repeater
(since the section-delegate rewrite). Ordinary rows' height changes reflow the
positioner correctly every frame, which is exactly what the mid-animation
sampling below asserts; there is no relayout timer / criteria swap to test
anymore.

Runs headless with no GL: we instantiate ScriptOutliner directly (not through
main.qml) so there is no ShaderEffectSource/FastBlur in the tree. The positioner
lays out its children without a scene graph, which is all we need to read
heights from.
"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
# Same reason as qmlcheck.py: the offscreen platform finds no fonts inside the
# PySide2 wheel, and the resulting QtWarnings would surface here as engine
# warnings -- indistinguishable from the binding warnings this test fails on.
if sys.platform == "win32":
    os.environ.setdefault("QT_QPA_FONTDIR", "C:/Windows/Fonts")
os.environ.setdefault("QT_QUICK_BACKEND", "software")

from PySide2.QtCore import QTimer, QUrl
from PySide2.QtGui import QGuiApplication
from PySide2.QtQuick import QQuickView

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mockmodel import DATA, MockModel  # same mock the render harness uses

qml_dir = os.path.abspath(sys.argv[1])
app = QGuiApplication(sys.argv[:1])

# Must be a real QQuickView, not a bare QQmlComponent. A windowless component
# creates its delegates but never runs the layout/polish pass, so heights can
# stay pinned at placeholder values and a collapse assertion silently passes
# on a meaningless number. With a window everything reports real geometry.
view = QQuickView()
view.engine().addImportPath(qml_dir)

# Held in a variable on purpose: setContextProperty does not take ownership,
# so passing MockModel() inline lets Python collect it and the view ends up
# with an empty model (count == 0).
model = MockModel()
view.rootContext().setContextProperty("scriptModel", model)

warnings = []
view.engine().warnings.connect(lambda ws: warnings.extend(w.toString() for w in ws))

# Wrap ScriptOutliner so it gets a real size -- a zero-sized view lays nothing
# out and every height reads back 0, which would look like a pass.
# The wrapper exposes plain-typed JS shims rather than the ScriptOutliner
# object itself: PySide2 cannot marshal a QML-defined type back to Python
# ("Can't find converter for 'ScriptOutliner_QMLTYPE_*'"), but bool/int/string
# return values cross fine.
wrapper = """
import QtQuick 2.15
Item {
    width: 260; height: 600
    ScriptOutliner { id: ol; anchors.fill: parent; model: scriptModel }

    // Key semantics mirror the component: majors use the first path segment,
    // subs use the full group string. Both go through the same isExpanded/
    // toggleGroup pair.
    function expanded(g)    { return ol.isExpanded(g); }
    function toggle(g)      { return ol.toggleGroup(g); }
    function hoverDesc()    { return ol.hoverDesc; }
    // layoutStore 的恢复路径走同一个入口：整体赋值 expandedGroups。
    function setExpanded(m) { ol.expandedGroups = m; }

    // Locate the view by objectName. Duck-typing on contentHeight does
    // not work: Text also has a contentHeight, so a property probe silently
    // matches the wrong node and reports a plausible-looking number.
    function findList() {
        for (var i = 0; i < ol.children.length; ++i)
            if (ol.children[i].objectName === "scriptListView")
                return ol.children[i];
        return null;
    }
    function findColumn() {
        var l = findList();
        if (!l) return null;
        var ch = l.contentItem.children;
        for (var i = 0; i < ch.length; ++i)
            if (ch[i].objectName === "rowsColumn")
                return ch[i];
        return null;
    }
    // contentHeight is the real proof that collapsed rows stop taking space:
    // isExpanded() reports intent, this reports layout.
    function listContentHeight() { var l = findList(); return l ? l.contentHeight : -1; }
    // 行永远全量存在（折叠不销毁行），断言"折叠不改行身份"用。
    function listRowCount() {
        var c = findColumn();
        if (!c) return -1;
        var n = 0;
        for (var i = 0; i < c.children.length; ++i)
            if (c.children[i].item && c.children[i].item.objectName === "scriptRow")
                ++n;
        return n;
    }

    // ---- realized rows: kind/key/title/badge/geometry, top to bottom ----
    // rowsColumn.children 是 Loader（其 item 才是行根）和 footer Item。
    function rows() {
        var c = findColumn();
        if (!c) return [];
        var out = [];
        var kids = c.children;
        for (var i = 0; i < kids.length; ++i) {
            var ld = kids[i];
            if (ld.objectName === "listFooter") {
                out.push({ tag: "FOOTER", key: "", title: "",
                           badge: "", badgeVisible: false,
                           y: ld.y, h: ld.height, clip: true });
                continue;
            }
            var it = ld.item;
            if (!it) continue;
            out.push({
                tag: it.objectName,
                key: it.rowKey !== undefined ? it.rowKey : "",
                title: it.rowTitle !== undefined ? it.rowTitle : "",
                badge: it.rowBadge !== undefined ? it.rowBadge : "",
                badgeVisible: it.rowBadgeVisible === true,
                y: ld.y, h: ld.height,
                clip: it.clip === true
            });
        }
        return out;
    }

    // ---- mid-animation sampling ----
    // 位置器必须每帧重排：动画进行中（height 未收敛）几何栈也不允许有
    // 交叠/空洞。Python 侧在 toggle 后不等 settle 就采样。
    function midHeights() {
        var c = findColumn();
        if (!c) return [];
        var out = [];
        for (var i = 0; i < c.children.length; ++i)
            out.push({ y: c.children[i].y, h: c.children[i].height });
        return out;
    }

    // ---- row identity probe (flicker regression) ----
    // 折叠绝不重建行（重建 = 悬停丢失/状态重放，就是上一版闪烁的机制）。
    // Stash 一个脚本行的 QObject，toggle 后报告同一路径的行是否仍是它。
    // Touching a stashed wrapper of a destroyed row throws, which itself
    // proves recreation -- both paths return false.
    property var _stashedRow: null
    function findRow(filePath) {
        var c = findColumn();
        if (!c) return null;
        for (var i = 0; i < c.children.length; ++i) {
            var it = c.children[i].item;
            if (it && it.objectName === "scriptRow" && it.rowFilePath === filePath)
                return it;
        }
        return null;
    }
    function stashRow(filePath) { _stashedRow = findRow(filePath); return _stashedRow !== null; }
    function rowSame() {
        if (_stashedRow === null)
            return false;
        var cur = findRow(_stashedRow.rowFilePath);
        return cur === _stashedRow;
    }

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


def check_h(label, got, want, tol=2):
    near = abs(got - want) <= tol
    print(f"  [{'ok' if near else 'FAIL'}] {label}: got {got}, want ~{want} (+-{tol})")
    if not near:
        failures.append(label)


# ---- two-level structure mirrored from DATA, same math as the QML side ----
ROW = 32 + 2          # Theme.rowItemHeight + Theme.rowGap
MAJOR_RAW = 34        # Theme.rowGroupHeight
SUB_RAW = 28          # Theme.rowSubGroupHeight
GAP = 4               # Theme.groupGap
FOOTER = 24           # Theme.fadeHeight


def major_of(group):
    return group.split("/")[0]


def row_count(group):
    return sum(1 for d in DATA if d["group"] == group)


GROUPS = sorted({d["group"] for d in DATA})
MAJORS = sorted({major_of(g) for g in GROUPS})
SUBGROUPS = sorted({g for g in GROUPS if "/" in g})

# Row order == deduped DATA row order (the registry sorts by relative path).
SECTIONS = []
_seen = set()
for _d in DATA:
    if _d["group"] not in _seen:
        _seen.add(_d["group"])
        SECTIONS.append(_d["group"])

# 大类按首现序（模型行序），不是字母序 —— rowList 按 sectionTree 的插入序展开。
MAJOR_ORDER = []
for _g in SECTIONS:
    _m = major_of(_g)
    if _m not in MAJOR_ORDER:
        MAJOR_ORDER.append(_m)

HAS_SUBS = {m: any(s != m for s in SECTIONS if major_of(s) == m) for m in MAJOR_ORDER}
# 组间隙的分账：有子分类的大类头后紧跟子分类头（不留隙），无子分类的大类头
# 后直接是脚本行（间隙烘在大类头里）。
MAJOR_H = {m: MAJOR_RAW + (0 if HAS_SUBS[m] else GAP) for m in MAJOR_ORDER}


def major_total(m):
    return sum(row_count(g) for g in SECTIONS if major_of(g) == m)


def expected_stack(open_majors, open_subs):
    """The visual stack as (kind, key, height) top-to-bottom.

    kind is "MAJOR", "SUB", "ROW" (script row) or "FOOTER". Follows the same
    order the QML rowList walks: model row order, each major's first section
    carrying the major header. Sub headers take space while their major is
    expanded (the 4px group gap rides on them); rows need the major AND the
    full-group key open. Zero-height rows are filtered by the caller before
    the positional walk.
    """
    blocks = []
    first_seen = set()
    for g in SECTIONS:
        m = major_of(g)
        if m not in first_seen:
            first_seen.add(m)
            blocks.append(("MAJOR", m, MAJOR_H[m]))
        if m not in open_majors:
            continue
        has_sub = "/" in g
        if has_sub:
            blocks.append(("SUB", g, SUB_RAW + GAP))
        if not has_sub or g in open_subs:
            blocks.extend([("ROW", g, ROW)] * row_count(g))
    blocks.append(("FOOTER", "", FOOTER))
    return blocks


def expected_height(open_majors, open_subs):
    return sum(h for (_, _, h) in expected_stack(open_majors, open_subs))


# QML objectName -> expected stack tag.
TAG_MAP = {"majorHeader": "MAJOR", "subHeader": "SUB", "scriptRow": "ROW"}


def qml_rows():
    v = root.rows()
    rows = v.toVariant() if hasattr(v, "toVariant") else (v if v is not None else [])
    for s in rows:
        s["tag"] = TAG_MAP.get(s["tag"], s["tag"])
    return rows


def check_stack(label, open_majors, open_subs):
    """Assert the realized stack has no overlap and no hole.

    Zero-height rows occupy no space, so both the expected blocks and the
    realized stack drop them before the positional walk (cumulative-y
    comparison with 0.5px tolerance).
    """
    got = [s for s in qml_rows() if float(s["h"]) > 0.5]
    got.sort(key=lambda s: float(s["y"]))
    want = [b for b in expected_stack(open_majors, open_subs) if b[2] > 0]
    if len(got) != len(want):
        print(f"  [FAIL] {label}: stack size got {len(got)}, want {len(want)}")
        for s in got:
            print(f"      {s['tag']}:{s['key']} y={s['y']} h={s['h']}")
        failures.append(f"{label}: stack size")
        return
    problems = []
    y = 0.0
    for i, (s, (etag, ekey, eh)) in enumerate(zip(got, want)):
        if s["tag"] != etag or s["key"] != ekey:
            problems.append(f"pos {i}: got {s['tag']}:{s['key']} want {etag}:{ekey}")
        if abs(float(s["y"]) - y) > 0.5:
            problems.append(f"pos {i} ({etag}:{ekey}): y={s['y']} want {y}")
        if abs(float(s["h"]) - eh) > 0.5:
            problems.append(f"pos {i} ({etag}:{ekey}): h={s['h']} want {eh}")
        y += eh
    if problems:
        print(f"  [FAIL] {label}")
        for p in problems:
            print("     ", p)
        failures.append(label)
    else:
        print(f"  [ok] {label}: {len(got)} stacked rows, "
              f"no overlap, no hole (cum height {y})")


def check_rows(label, open_majors, open_subs):
    """Assert every header row's title/badge/height and every row's clip."""
    got = qml_rows()
    problems = []
    seen_sub = set()
    for s in got:
        tag = s["tag"]
        if tag == "MAJOR":
            m = s["key"]
            if s["title"] != m:
                problems.append(f"major {m}: title={s['title']!r}")
            if s["badge"] != str(major_total(m)):
                problems.append(f"major {m}: badge={s['badge']!r} want {major_total(m)}")
            if s["badgeVisible"] != (m not in open_majors):
                problems.append(f"major {m}: badgeVisible={s['badgeVisible']}")
            if abs(float(s["h"]) - MAJOR_H[m]) > 0.5:
                problems.append(f"major {m}: h={s['h']} want {MAJOR_H[m]}")
        elif tag == "SUB":
            g = s["key"]
            seen_sub.add(g)
            want_h = SUB_RAW + GAP if g.split("/")[0] in open_majors else 0
            if abs(float(s["h"]) - want_h) > 0.5:
                problems.append(f"sub {g}: h={s['h']} want {want_h}")
            if s["title"] != g.split("/", 1)[1]:
                problems.append(f"sub {g}: title={s['title']!r}")
            if s["badge"] != str(row_count(g)):
                problems.append(f"sub {g}: badge={s['badge']!r} want {row_count(g)}")
            if s["badgeVisible"] != (g not in open_subs):
                problems.append(f"sub {g}: badgeVisible={s['badgeVisible']}")
        if not s["clip"]:
            problems.append(f"{tag}:{s['key']}: clip is false")
    if seen_sub != set(SUBGROUPS):
        problems.append(f"sub rows mismatch: got {sorted(seen_sub)}")
    if problems:
        print(f"  [FAIL] {label}")
        for p in problems:
            print("     ", p)
        failures.append(label)
    else:
        print(f"  [ok] {label}: {len(got)} rows match titles/badges/heights")


def settle(ms=600):
    loop_end = QTimer()
    loop_end.setSingleShot(True)
    loop_end.timeout.connect(app.quit)
    loop_end.start(ms)
    app.exec_()


def check_mid_animation(label, open_majors, open_subs):
    """Sample the stack DURING the height animations (no settle first).

    The positioner must reflow every frame: even mid-animation, cumulative
    heights must leave no overlap and no hole. Heights are animating, so only
    the stack invariant (y contiguity) is asserted here, not final values.
    """
    got = [s for s in root.midHeights().toVariant()
           if isinstance(s, dict) and float(s["h"]) > 0.5]
    got.sort(key=lambda s: float(s["y"]))
    problems = []
    y = 0.0
    for s in got:
        sy, sh = float(s["y"]), float(s["h"])
        if abs(sy - y) > 0.5:
            problems.append(f"hole/overlap at y={sy}, previous stack ends at {y}")
        y = sy + sh
    if problems:
        print(f"  [FAIL] {label}")
        for s in got:
            print(f"      y={s['y']} h={s['h']}")
        for p in problems:
            print("     ", p)
        failures.append(label)
    else:
        print(f"  [ok] {label}: {len(got)} visible rows contiguous mid-animation")


print(f"groups in mock data: {GROUPS}")
print(f"majors (first-seen order): {MAJOR_ORDER}")
print(f"subgroups: {SUBGROUPS}")
print(f"major totals: { {m: major_total(m) for m in MAJORS} }")

# --- default state: everything expanded at both levels ---
print("\n1. default state")
for m in MAJORS:
    check(f"isExpanded({m})", root.expanded(m), True)
for g in SUBGROUPS:
    check(f"isExpanded({g})", root.expanded(g), True)
settle()
check("script rows always exist (fold never destroys rows)",
      root.listRowCount(), len(DATA))
check_rows("rows, all expanded", set(MAJORS), set(SUBGROUPS))
check_h("contentHeight, all expanded", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))
check_stack("stack, all expanded", set(MAJORS), set(SUBGROUPS))

# --- groupCount reaches the model ---
print("\n2. groupCount via model")
for g in GROUPS:
    check(f"groupCount({g})", model.groupCount(g), row_count(g))

# --- toggling a major collapses the whole major ---
print("\n3. toggleGroup('Image') collapses the whole major")
root.toggle("Image")
open_majors = set(MAJORS) - {"Image"}
check("isExpanded(Image)", root.expanded("Image"), False)
for m in ("Geometry", "System"):
    check(f"isExpanded({m}) unaffected", root.expanded(m), True)
for g in SUBGROUPS:
    # The sub keys' map values are untouched: their rows disappear because the
    # major gate closed, not because the subs were collapsed.
    check(f"sub key {g} map value untouched", root.expanded(g), True)
check_mid_animation("mid-animation, Image collapsing", open_majors, set(SUBGROUPS))
settle()
check_h("contentHeight with Image collapsed", root.listContentHeight(),
        expected_height(open_majors, set(SUBGROUPS)))
check_rows("rows with Image collapsed", open_majors, set(SUBGROUPS))
check_stack("stack with Image collapsed", open_majors, set(SUBGROUPS))
root.toggle("Image")

# --- toggling a single sub leaves the major (and other subs) alone ---
print("\n4. toggleGroup('Image/Edit') collapses one sub only")
root.toggle("Image/Edit")
check("isExpanded(Image/Edit)", root.expanded("Image/Edit"), False)
check("isExpanded(Image) still open", root.expanded("Image"), True)
check_mid_animation("mid-animation, Image/Edit collapsing",
                    set(MAJORS), set(SUBGROUPS) - {"Image/Edit"})
settle()
open_subs = set(SUBGROUPS) - {"Image/Edit"}
check_h("contentHeight with Image/Edit collapsed", root.listContentHeight(),
        expected_height(set(MAJORS), open_subs))
check_rows("rows with Image/Edit collapsed", set(MAJORS), open_subs)
check_stack("stack with Image/Edit collapsed", set(MAJORS), open_subs)
root.toggle("Image/Edit")
settle()
check_h("contentHeight restored after sub toggle", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))
check_stack("stack restored after sub toggle", set(MAJORS), set(SUBGROUPS))

# --- collapsing every key, then restoring, leaves no residue ---
print("\n5. collapse all (majors + subs) then restore all")
all_keys = sorted(set(MAJORS) | set(SUBGROUPS))
for k in all_keys:
    root.toggle(k)
for k in all_keys:
    check(f"isExpanded({k}) collapsed", root.expanded(k), False)
for k in all_keys:
    root.toggle(k)
for k in all_keys:
    check(f"isExpanded({k}) restored", root.expanded(k), True)

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

# --- layout: the two-level collapse matrix ---
# Row/header heights animate (Behavior on height), so the event loop has to
# run for the animation to finish before contentHeight settles. settle() pumps
# events for well past the 150ms duration.
print("\n8. layout: two-level collapse matrix")
settle()
check_h("all expanded", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))

for g in SUBGROUPS:
    root.toggle(g)
settle()
check_h("all subs collapsed, majors open", root.listContentHeight(),
        expected_height(set(MAJORS), set()))
check_rows("rows, subs collapsed", set(MAJORS), set())
check_stack("stack, subs collapsed", set(MAJORS), set())

for m in MAJORS:
    root.toggle(m)
settle()
# With every major collapsed only the major headers and the footer remain.
all_collapsed = root.listContentHeight()
check_h("all majors collapsed (+ subs, which are hidden)",
        all_collapsed, expected_height(set(), set()))
check_h("  == majors*(34[+4]) + 0*subHeader + footer(24)",
        all_collapsed, sum(MAJOR_H.values()) + FOOTER)
check_rows("rows, all majors collapsed", set(), set())
check_stack("stack, all majors collapsed", set(), set())

for g in SUBGROUPS:
    root.toggle(g)
settle()
check_h("subs re-expanded while majors collapsed (major gate holds)",
        root.listContentHeight(), expected_height(set(), set(SUBGROUPS)))
check_stack("stack, subs re-expanded while majors collapsed", set(), set(SUBGROUPS))

for m in MAJORS:
    root.toggle(m)
settle()
check_h("majors re-expanded (subs open again)", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))
check_stack("stack, majors re-expanded", set(MAJORS), set(SUBGROUPS))

for g in SUBGROUPS:
    root.toggle(g)
settle()
check_h("majors open, subs collapsed", root.listContentHeight(),
        expected_height(set(MAJORS), set()))
check_stack("stack, majors open subs collapsed", set(MAJORS), set())

for g in SUBGROUPS:
    root.toggle(g)
settle()
check_h("fully restored", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))
check_stack("stack, fully restored", set(MAJORS), set(SUBGROUPS))

# --- the previously broken path: sub collapsed first, then the major ---
# This was the pure section-height path that needed the criteria swap under
# the ListView implementation. As a plain positioner the reflow is automatic;
# geometry must be exact after every step with no relayout machinery.
print("\n9. pure section-height path (sub collapsed first, then the major)")
root.toggle("Geometry/Format Convert")
settle()
sub_closed_subs = set(SUBGROUPS) - {"Geometry/Format Convert"}
check_h("contentHeight, Geometry sub collapsed", root.listContentHeight(),
        expected_height(set(MAJORS), sub_closed_subs))
check_stack("stack, Geometry sub collapsed", set(MAJORS), sub_closed_subs)
root.toggle("Geometry")
check_mid_animation("mid-animation, Geometry major collapsing (pure path)",
                    set(MAJORS) - {"Geometry"}, sub_closed_subs)
settle()
geom_closed_majors = set(MAJORS) - {"Geometry"}
check_h("contentHeight, Geometry major collapsed (pure path)",
        root.listContentHeight(),
        expected_height(geom_closed_majors, sub_closed_subs))
check_stack("stack, Geometry major collapsed (pure path)",
            geom_closed_majors, sub_closed_subs)
root.toggle("Geometry")
settle()
check_h("contentHeight, Geometry re-expanded (pure path)",
        root.listContentHeight(),
        expected_height(set(MAJORS), sub_closed_subs))
check_stack("stack, Geometry re-expanded (pure path)",
            set(MAJORS), sub_closed_subs)
root.toggle("Geometry/Format Convert")
settle()
check_h("contentHeight, fully restored again", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))
check_stack("stack, fully restored again", set(MAJORS), set(SUBGROUPS))

# --- flicker regression: toggles must never recreate rows ---
# The old ListView implementation needed a criteria swap that replayed icon
# animations and reset hover; as a positioner there is no such machinery, but
# the invariant is still what protects against flicker: row QObject identity
# survives every toggle, and the total row count never changes.
print("\n10. row identity survives toggles")
check("stashRow found the row", root.stashRow("D:/GeneralOperations/img.FlipImage_Horizontal.py"), True)
root.toggle("Image/Edit")
settle()
check("sub toggle: script row identity preserved", root.rowSame(), True)
check("sub toggle: script row count unchanged", root.listRowCount(), len(DATA))
root.toggle("Image/Edit")
settle()
check("sub toggle back: row identity preserved", root.rowSame(), True)
root.toggle("Image")
settle()
check("major toggle: row identity preserved", root.rowSame(), True)
check("major toggle: script row count unchanged", root.listRowCount(), len(DATA))
root.toggle("Image")
settle()

# --- layoutStore restore path: whole-map assignment drives the fold ---
print("\n11. setExpanded (restore from layout.json semantics)")
root.setExpanded({"Image": False})
check("isExpanded(Image) after setExpanded", root.expanded("Image"), False)
settle()
restored_majors = set(MAJORS) - {"Image"}
check_h("contentHeight after setExpanded", root.listContentHeight(),
        expected_height(restored_majors, set(SUBGROUPS)))
check_stack("stack after setExpanded", restored_majors, set(SUBGROUPS))
root.setExpanded({})
settle()
check_h("contentHeight after reset", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))
check_stack("stack after reset", set(MAJORS), set(SUBGROUPS))

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

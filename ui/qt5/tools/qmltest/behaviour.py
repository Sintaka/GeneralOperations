"""Behavioural test for the two-level collapse logic in ScriptOutliner.

Instantiation proves the bindings resolve; it does not prove collapsing works.
This drives the component the way a user would -- call toggleGroup() on the
two key kinds (major = group's first segment, e.g. "Image"; sub = full group,
e.g. "Image/Edit"), then read back the actual delegate geometry and every
section delegate's forked state -- and asserts the expected outcome.

Runs headless with no GL: we instantiate ScriptOutliner directly (not through
main.qml) so there is no ShaderEffectSource/FastBlur in the tree. A ListView
still creates and lays out its delegates without a scene graph, which is all we
need to read heights from.
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

    // Key semantics mirror the component: majors use the first path segment,
    // subs use the full group string. Both go through the same isExpanded/
    // toggleGroup pair.
    function expanded(g)    { return ol.isExpanded(g); }
    function toggle(g)      { return ol.toggleGroup(g); }
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

    // Two-level headers: each section delegate renders up to two rows -- a
    // major header row (only on the major's first section, the "carrier") and
    // a sub header row (every group containing "/"). Collect both rows'
    // title/badge/height so Python can assert the fork and the badge sums.
    function headers() {
        var l = findList();
        if (!l || !l.contentItem)
            return [];
        var out = [];
        // contentItem.children, not childItems: the latter is a
        // QQmlListProperty and reads as undefined from QML JS.
        var items = l.contentItem.children;
        for (var i = 0; i < items.length; ++i) {
            var it = items[i];
            if (it.objectName !== "sectionHeader")
                continue;
            out.push({
                section: it.headerSection,
                majorTitle: it.majorRowTitle,
                majorHeight: it.majorRowHeight,
                majorBadge: it.majorRowBadge,
                majorBadgeVisible: it.majorRowBadgeVisible,
                subTitle: it.subRowTitle,
                subHeight: it.subRowHeight,
                subBadge: it.subRowBadge,
                subBadgeVisible: it.subRowBadgeVisible
            });
        }
        return out;
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

# Section order == deduped DATA row order (the registry sorts by relative
# path, so this is exactly the order ListView emits sections in).
SECTIONS = []
_seen = set()
for _d in DATA:
    if _d["group"] not in _seen:
        _seen.add(_d["group"])
        SECTIONS.append(_d["group"])

FIRST_OF_MAJOR = {}
for _g in SECTIONS:
    FIRST_OF_MAJOR.setdefault(major_of(_g), _g)


def major_total(m):
    return sum(row_count(g) for g in SECTIONS if major_of(g) == m)


def key_open(key, open_majors, open_subs):
    """Expansion state of one expandedGroups key.

    Sub keys (containing "/") live in open_subs; bare major keys live in
    open_majors (for a group without "/", the major key IS the full-group
    key, so one toggle drives both).
    """
    if "/" in key:
        return key in open_subs
    return key in open_majors


def expected_height(open_majors, open_subs):
    """contentHeight for a given expansion state.

    open_majors: expanded major keys (first segments);
    open_subs:   expanded sub keys (full group strings).
    Each section delegate renders up to two header rows: the major's FIRST
    section always carries a major header (34px); every group containing "/"
    renders a sub header (28px) that only takes space while its major is
    expanded. Rows need the major AND the full-group key open. The per-
    delegate group gap (4px) only applies while the delegate still shows at
    least one row -- fully hidden delegates must collapse to exactly 0.
    """
    total = 0
    for m in MAJORS:
        secs = [s for s in SECTIONS if major_of(s) == m]
        for pos, g in enumerate(secs):
            first = pos == 0
            has_sub = "/" in g
            h = (MAJOR_RAW if first else 0) \
                + (SUB_RAW if has_sub and m in open_majors else 0)
            if h > 0:
                h += GAP
            total += h
            # Rows need the major open AND the full-group key open; for a
            # group without "/" the full-group key IS the major key, so the
            # second check collapses into the first.
            if m in open_majors and (not has_sub or g in open_subs):
                total += ROW * row_count(g)
    return total + FOOTER


def expected_rows(open_majors, open_subs):
    """(section, majorTitle, majorHeight, majorBadge, majorBadgeVisible,
    subTitle, subHeight, subBadge, subBadgeVisible) per section delegate."""
    rows = []
    for g in SECTIONS:
        m = major_of(g)
        first = FIRST_OF_MAJOR[m] == g
        has_sub = "/" in g
        if first:
            mj_title, mj_h = m, MAJOR_RAW
            mj_badge, mj_vis = str(major_total(m)), m not in open_majors
        else:
            mj_title, mj_h, mj_badge, mj_vis = "", 0, "", False
        if has_sub:
            sb_title = g.split("/", 1)[1]
            sb_h = SUB_RAW if m in open_majors else 0
            sb_badge, sb_vis = str(row_count(g)), g not in open_subs
        else:
            sb_title, sb_h, sb_badge, sb_vis = "", 0, "", False
        rows.append((g, mj_title, mj_h, mj_badge, mj_vis,
                     sb_title, sb_h, sb_badge, sb_vis))
    return rows


def qml_headers():
    """headers() result as plain Python data.

    PySide2 hands JS arrays back as QJSValue instead of converting them;
    toVariant() turns them into a list of dicts.
    """
    v = root.headers()
    if hasattr(v, "toVariant"):
        return v.toVariant()
    return v if v is not None else []


def check_headers(label, open_majors, open_subs, check_heights=True):
    """Assert every section delegate's major/sub rows match the expected fork."""
    got = qml_headers()
    want = expected_rows(open_majors, open_subs)
    if len(got) != len(want):
        print(f"  [FAIL] {label}: header count got {len(got)}, want {len(want)}")
        failures.append(f"{label}: header count")
        return
    problems = []
    for h, w in zip(got, want):
        (w_section, mj_t, mj_h, mj_b, mj_v, sb_t, sb_h, sb_b, sb_v) = w
        if h["section"] != w_section:
            problems.append(f"{w_section}: section={h['section']!r}")
        if h["majorTitle"] != mj_t:
            problems.append(f"{w_section}: majorTitle={h['majorTitle']!r} want {mj_t!r}")
        if h["subTitle"] != sb_t:
            problems.append(f"{w_section}: subTitle={h['subTitle']!r} want {sb_t!r}")
        if h["majorBadgeVisible"] != mj_v:
            problems.append(f"{w_section}: majorBadgeVisible={h['majorBadgeVisible']!r} want {mj_v!r}")
        if h["subBadgeVisible"] != sb_v:
            problems.append(f"{w_section}: subBadgeVisible={h['subBadgeVisible']!r} want {sb_v!r}")
        if h["majorBadge"] != mj_b:
            problems.append(f"{w_section}: majorBadge={h['majorBadge']!r} want {mj_b!r}")
        if h["subBadge"] != sb_b:
            problems.append(f"{w_section}: subBadge={h['subBadge']!r} want {sb_b!r}")
        if check_heights:
            if abs(float(h["majorHeight"]) - mj_h) > 0.5:
                problems.append(f"{w_section}: majorHeight={h['majorHeight']!r} want {mj_h}")
            if abs(float(h["subHeight"]) - sb_h) > 0.5:
                problems.append(f"{w_section}: subHeight={h['subHeight']!r} want {sb_h}")
    if problems:
        print(f"  [FAIL] {label}")
        for p in problems:
            print("     ", p)
        failures.append(label)
    else:
        print(f"  [ok] {label}: {len(got)} section delegates match the two-level fork")


def settle(ms=600):
    loop_end = QTimer()
    loop_end.setSingleShot(True)
    loop_end.timeout.connect(app.quit)
    loop_end.start(ms)
    app.exec_()


print(f"groups in mock data: {GROUPS}")
print(f"majors: {MAJORS}")
print(f"subgroups: {SUBGROUPS}")
print(f"major totals (sum of groupCount over the major's sections): "
      f"{ {m: major_total(m) for m in MAJORS} }")

# --- default state: everything expanded at both levels ---
print("\n1. default state")
for m in MAJORS:
    check(f"isExpanded({m})", root.expanded(m), True)
for g in SUBGROUPS:
    check(f"isExpanded({g})", root.expanded(g), True)
settle()
check("ListView row count == mock rows", root.listCount(), len(DATA))
check_headers("headers, all expanded", set(MAJORS), set(SUBGROUPS))
check_h("contentHeight, all expanded", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))

# --- groupCount reaches the model ---
print("\n2. groupCount via model (full group strings; major totals are "
      "QML-side sums over a major's sections)")
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
settle()
check_h("contentHeight with Image collapsed", root.listContentHeight(),
        expected_height(open_majors, set(SUBGROUPS)))
check_headers("headers with Image collapsed", open_majors, set(SUBGROUPS))
root.toggle("Image")

# --- toggling a single sub leaves the major (and other subs) alone ---
print("\n4. toggleGroup('Image/Edit') collapses one sub only")
root.toggle("Image/Edit")
check("isExpanded(Image/Edit)", root.expanded("Image/Edit"), False)
check("isExpanded(Image) still open", root.expanded("Image"), True)
settle()
open_subs = set(SUBGROUPS) - {"Image/Edit"}
check_h("contentHeight with Image/Edit collapsed", root.listContentHeight(),
        expected_height(set(MAJORS), open_subs))
check_headers("headers with Image/Edit collapsed", set(MAJORS), open_subs)
root.toggle("Image/Edit")
settle()
check_h("contentHeight restored after sub toggle", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))

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
check_headers("headers, subs collapsed", set(MAJORS), set())

for m in MAJORS:
    root.toggle(m)
settle()
# With every major collapsed only the major headers and the footer remain:
# majors * (34 + 4) + footer 24; zero sub headers are visible.
all_collapsed = root.listContentHeight()
check_h("all majors collapsed (+ subs, which are hidden)",
        all_collapsed, expected_height(set(), set()))
check_h("  == majors*(34+4) + 0*subHeader + footer(24)",
        all_collapsed, len(MAJORS) * 38 + 24)
check_headers("headers, all majors collapsed", set(), set())

for g in SUBGROUPS:
    root.toggle(g)
settle()
check_h("subs re-expanded while majors collapsed (major gate holds)",
        root.listContentHeight(), expected_height(set(), set(SUBGROUPS)))

for m in MAJORS:
    root.toggle(m)
settle()
check_h("majors re-expanded (subs open again)", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))

for g in SUBGROUPS:
    root.toggle(g)
settle()
check_h("majors open, subs collapsed", root.listContentHeight(),
        expected_height(set(MAJORS), set()))

for g in SUBGROUPS:
    root.toggle(g)
settle()
check_h("fully restored", root.listContentHeight(),
        expected_height(set(MAJORS), set(SUBGROUPS)))

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

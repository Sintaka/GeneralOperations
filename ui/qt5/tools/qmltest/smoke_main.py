"""Load the real main.qml and exercise binding, rendering, and interaction.

The smoke run mirrors production's QQmlApplicationEngine path and verifies:
- no engine warnings;
- Backdrop paints once and does not repaint across resize;
- CommandOutput stays fixed-height, wraps, scrolls, and follows conditionally;
- real mouse selection plus idle-copy/running-cancel Ctrl+C routing.

Usage: smoke_main.py <qml_dir>
Exit code 0 = all checks passed, 1 = any failure.
"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
# Same reason as qmlcheck.py / behaviour.py: the offscreen platform finds no
# fonts inside the PySide2 wheel, and the resulting QtWarnings would surface
# here as engine warnings -- indistinguishable from the binding warnings this
# test fails on.
if sys.platform == "win32":
    os.environ.setdefault("QT_QPA_FONTDIR", "C:/Windows/Fonts")

# Backend note (measured 2026-09-06, PySide2 / Qt 5.15.2, Windows): the
# DEFAULT quick backend works under offscreen here -- main.qml loads,
# QtGraphicalEffects compiles, and the run is completely warning-free.
# So unlike behaviour.py we do NOT force QT_QUICK_BACKEND=software (the
# software renderer has no ShaderEffectSource/FastBlur; main.qml hangs there,
# see README). On a GL-less machine only QtGraphicalEffects load/shader
# warnings would be excusable -- if that ever shows up, add the exact message
# to EXEMPT_WARNINGS below; everything else still fails the run.
EXEMPT_WARNINGS = ()  # reserved for "offscreen has no GL" noise only

from PySide2.QtCore import QObject, QPoint, Qt, QTimer, Slot  # noqa: E402
from PySide2.QtGui import QGuiApplication  # noqa: E402
from PySide2.QtQml import QQmlApplicationEngine  # noqa: E402
from PySide2.QtTest import QTest  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mockmodel import MockLayoutStore, MockModel, MockRunner  # noqa: E402

qml_dir = os.path.abspath(sys.argv[1])

app = QGuiApplication(sys.argv[:1])

engine = QQmlApplicationEngine()
engine.addImportPath(qml_dir)

# Held in variables on purpose: setContextProperty does not take ownership,
# so inline temporaries get collected and QML silently sees nothing (same
# trap behaviour.py documents in its README).
model = MockModel()
runner = MockRunner()
layout = MockLayoutStore()


class _DummyLogger(QObject):
    """debugLogger stand-in. DropZone calls debugLogger.log(...) in
    --dropdebug mode; the smoke run never drags, but the slot must exist so
    the object is a faithful stand-in for DropDebugLogger."""

    @Slot(str)
    def log(self, msg):
        pass


logger = _DummyLogger()

engine.rootContext().setContextProperty("scriptModel", model)
engine.rootContext().setContextProperty("scriptRunner", runner)
engine.rootContext().setContextProperty("debugLogger", logger)
# main.qml 启动期就读 layoutStore（折叠状态恢复），必须先于加载存在。
engine.rootContext().setContextProperty("layoutStore", layout)
# Empty request == plain start (main.qml's Component.onCompleted early-returns
# on a falsy .script, so no ScriptModel.resolveScript call happens either).
engine.rootContext().setContextProperty("startupRequest", {})
engine.rootContext().setContextProperty("dropDebugMode", False)

warnings = []
engine.warnings.connect(lambda ws: warnings.extend(w.toString() for w in ws))

engine.load(os.path.join(qml_dir, "main.qml"))

roots = engine.rootObjects()
if not roots:
    print("[FAIL] main.qml produced no root object; engine warnings:")
    for w in warnings:
        print("     ", w)
    sys.exit(1)

win = roots[0]


def find_by_class(obj, needle):
    """First descendant whose metaObject className contains `needle`.

    QML-instantiated types report class names like "Backdrop_QMLTYPE_13", so
    a substring match is the reliable discriminator. findChildren(QObject)
    is used because PySide2 wraps the QML Window root as plain QWindow --
    it has no contentItem/childItems to walk manually.
    """
    for c in obj.findChildren(QObject):
        if needle in c.metaObject().className():
            return c
    return None


def find_by_object_name(obj, name):
    for c in obj.findChildren(QObject):
        if c.objectName() == name:
            return c
    return None


backdrop = find_by_class(win, "Backdrop")
canvas = find_by_class(backdrop, "Canvas") if backdrop is not None else None
if backdrop is None or canvas is None:
    print("[FAIL] could not locate the Backdrop Canvas in the loaded tree")
    sys.exit(1)

# Count every actual paint execution from creation onward.
paints = [0]
canvas.paint.connect(lambda *a: paints.__setitem__(0, paints[0] + 1))

win.show()


def settle(ms=500):
    """Pump the event loop for ms milliseconds (behaviour.py's pattern)."""
    timer = QTimer()
    timer.setSingleShot(True)
    timer.timeout.connect(app.quit)
    timer.start(ms)
    app.exec_()


failures = []


def check(label, ok, detail=""):
    print(f"  [{'ok' if ok else 'FAIL'}] {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures.append(label)


# ---- 1. creation: the canvas paints exactly once and keeps its fixed size ----
print("1. creation paint")
settle(500)
baseline = paints[0]
check("onPaint ran exactly once at creation", baseline == 1, f"count={baseline}")
check("canvas pinned at 1600x900",
      canvas.property("width") == 1600 and canvas.property("height") == 900,
      f"got {canvas.property('width')}x{canvas.property('height')}")

# ---- 2. ten resizes must not trigger a single repaint ----
print("2. ten width changes -> repaints")
widths = [900 + i * 30 for i in range(10)]
for w in widths:
    win.setWidth(w)
settle(500)

# Guard against a vacuous pass: if the resize never reached the QML tree,
# "0 repaints" would mean nothing. The Backdrop is anchored to the window,
# so its width must now equal the final window width.
final_width = widths[-1]
check("resize reached the QML tree",
      backdrop.property("width") == final_width,
      f"backdrop width={backdrop.property('width')}, want {final_width}")

total = paints[0]
check("10 width changes -> onPaint total still exactly 1", total == 1,
      f"total={total} (baseline={baseline}, +{total - baseline} after resizes)")

# ---- 3. command output: fixed geometry and follow-tail state machine ----
print("3. command output behaviour")
output = find_by_object_name(win, "commandOutput")
output_view = find_by_object_name(win, "commandOutputFlickable")
output_edit = find_by_object_name(win, "commandOutputTextEdit")
run_status = find_by_object_name(win, "runStatus")
check("command output objects found",
      all(x is not None for x in (output, output_view, output_edit, run_status)))

base_tail = "\n".join(
    f"line {i:02d}: " + ("wrapped output " * 12) for i in range(30)
)
runner.setTail(base_tail)
settle(300)

if all(x is not None for x in (output, output_view, output_edit)):
    def output_max_y():
        return max(0.0, float(output_view.property("contentHeight"))
                   - float(output_view.property("height")))

    def output_y():
        return float(output_view.property("contentY"))

    check("output height stays fixed at 132", float(output.property("height")) == 132.0,
          f"height={output.property('height')}")
    check("long output wraps and becomes scrollable", output_max_y() > 0,
          f"maxY={output_max_y()}")
    check("initial output follows the tail", abs(output_y() - output_max_y()) <= 1.0,
          f"y={output_y()}, maxY={output_max_y()}")

    output_view.setProperty("contentY", 0.0)
    settle(100)
    check("user scroll up pauses following", output.property("followTail") is False)
    paused_y = output_y()
    runner.setTail(base_tail + "\nnew line while paused")
    settle(200)
    check("append preserves user scroll position", abs(output_y() - paused_y) <= 1.0,
          f"y={output_y()}, paused={paused_y}")

    output_view.setProperty("contentY", output_max_y())
    settle(100)
    check("returning to bottom resumes following", output.property("followTail") is True)
    runner.setTail(base_tail + "\nnew line while paused\nnew line while following")
    settle(200)
    check("append follows after resume", abs(output_y() - output_max_y()) <= 1.0,
          f"y={output_y()}, maxY={output_max_y()}")

# ---- 4. real pointer/key routing through the loaded main.qml ----
print("4. output selection and Ctrl+C routing")
if output_view is not None and output_edit is not None:
    def item_scene_point(item, x, y):
        while item is not None:
            x += float(item.property("x") or 0)
            y += float(item.property("y") or 0)
            item = item.parent()
        return QPoint(round(x), round(y))

    output_view.setProperty("contentY", 0.0)
    settle(100)
    output_edit.forceActiveFocus()
    start = item_scene_point(output_edit, 4, 7)
    end = item_scene_point(output_edit, 115, 7)
    QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, start)
    QTest.mouseMove(win, end, 80)
    QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, end)
    settle(100)

    selected = output_edit.property("selectedText") or ""
    check("mouse drag reaches TextEdit and selects output", len(selected) > 0,
          f"selected={selected!r}")
    check("selection pauses following", output.property("followTail") is False)

    selection_start = output_edit.property("selectionStart")
    selection_end = output_edit.property("selectionEnd")
    runner.setTail(runner._tail_text + "\nappend with selection")
    settle(200)
    check("append preserves selection",
          output_edit.property("selectionStart") == selection_start
          and output_edit.property("selectionEnd") == selection_end,
          f"selection={output_edit.property('selectionStart')}.."
          f"{output_edit.property('selectionEnd')}")

    clipboard = QGuiApplication.clipboard()
    clipboard.clear()
    runner.setRunning(False)
    QTest.keyClick(win, Qt.Key_C, Qt.ControlModifier)
    settle(100)
    check("idle Ctrl+C copies selected output", clipboard.text() == selected,
          f"clipboard={clipboard.text()!r}, selected={selected!r}")

    QTest.keyClick(win, Qt.Key_A, Qt.ControlModifier)
    settle(100)
    check("keyboard selection works in read-only output",
          len(output_edit.property("selectedText") or "")
          == len(output_edit.property("text") or ""))

    clipboard.setText("cancel-sentinel")
    before_cancel = runner.cancel_calls
    runner.setRunning(True)
    settle(100)
    QTest.keyClick(win, Qt.Key_C, Qt.ControlModifier)
    settle(100)
    check("running Ctrl+C calls cancel exactly once",
          runner.cancel_calls == before_cancel + 1,
          f"calls={runner.cancel_calls - before_cancel}")
    check("running Ctrl+C is not also routed as copy",
          clipboard.text() == "cancel-sentinel", f"clipboard={clipboard.text()!r}")
    check("cancelled status is visible",
          run_status is not None and run_status.property("text") == "执行已取消",
          f"status={run_status.property('text') if run_status else None!r}")

    runner.setTail("")
    settle(100)
    check("clearing output for a new run resets following",
          output.property("followTail") is True)

# ---- 5. warnings ----
print("5. engine warnings")
noise = [w for w in warnings if not any(x in w for x in EXEMPT_WARNINGS)]
if noise:
    print(f"  [FAIL] {len(noise)} engine warning(s):")
    for w in noise:
        print("     ", w)
    failures.append("engine warnings")
else:
    print(f"  [ok] none ({len(warnings)} raw, "
          f"{len(warnings) - len(noise)} exempt)")

print()
if failures:
    print(f"[FAIL] {len(failures)} check(s) failed: {failures}")
    sys.exit(1)
print("[ok] main.qml smoke: bindings clean, Backdrop repaints once at "
      "creation and never on resize")
sys.exit(0)

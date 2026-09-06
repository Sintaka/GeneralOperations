"""Smoke test that loads the REAL main.qml and counts Backdrop repaints.

Two assertions in one load, mirroring how production instantiates the UI
(main.cpp uses QQmlApplicationEngine, so a Window-root QML must go through
QQmlApplicationEngine too -- QQuickView.setSource rejects a Window root with
"invalid root object", found out the hard way; see the probe notes in
docs/pitfalls/2026-09-06-canvas-resize-repaint.md):

1. Binding smoke: every binding in the real tree gets to run under the same
   context-property stubs C++ supplies (scriptModel / scriptRunner /
   debugLogger / startupRequest / dropDebugMode, all idle). The run fails on
   any engine warning, exactly like qmlcheck.py.

2. Backdrop repaint counter: the Canvas inside Backdrop must paint exactly
   once (creation) and never again when the window resizes. This is the
   assertable evidence for the live-resize fix: the old Backdrop re-issued
   requestPaint() from onWidthChanged/onHeightChanged, so every resize step
   ran the full-window JS radial-gradient repaint on the main thread
   (Canvas.Cooperative) -- measurable as ~1 paint per resize. The fix pins
   the canvas at 1600x900 and scales it; the counter must stay at 1 across
   10 width changes. Canvas exposes the signal as `paint`; connecting one
   more Python slot alongside the QML onPaint handler is safe and counts
   every actual paint execution.

Usage: smoke_main.py <qml_dir>   (same CLI as qmlcheck.py / behaviour.py)
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

from PySide2.QtCore import QObject, QTimer, Slot  # noqa: E402
from PySide2.QtGui import QGuiApplication  # noqa: E402
from PySide2.QtQml import QQmlApplicationEngine  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mockmodel import MockModel, MockRunner  # noqa: E402

qml_dir = os.path.abspath(sys.argv[1])

app = QGuiApplication(sys.argv[:1])

engine = QQmlApplicationEngine()
engine.addImportPath(qml_dir)

# Held in variables on purpose: setContextProperty does not take ownership,
# so inline temporaries get collected and QML silently sees nothing (same
# trap behaviour.py documents in its README).
model = MockModel()
runner = MockRunner()


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

# ---- 3. warnings ----
print("3. engine warnings")
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

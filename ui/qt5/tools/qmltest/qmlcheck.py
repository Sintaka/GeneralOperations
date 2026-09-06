"""Load AND instantiate each QML file, reporting parse errors and binding errors.

Why instantiate: QQmlComponent.loadUrl only parses. A reference to a
nonexistent property (Theme.fog after a rename, or Glyph.Kind.Chevron if
scoped enums do not resolve) parses fine and then evaluates to undefined at
create() time. Undefined color renders transparent, undefined int renders 0 --
silent, no error text. Only create() surfaces those.

Usage: qmlcheck.py <qml_dir> [file.qml ...]   (default: every .qml found)
"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
# The offscreen platform plugin builds its own font database and looks in
# <PySide2>/lib/fonts, which the PySide2 wheel does not ship. Every text query
# then emits "Cannot find font directory" as a QtWarning AND routes it through
# engine.warnings(), drowning the binding errors this script exists to catch.
# Point the platform at the system fonts instead (Windows only; other platforms
# ship fontconfig/freetype system databases).
if sys.platform == "win32":
    os.environ.setdefault("QT_QPA_FONTDIR", "C:/Windows/Fonts")
from PySide2.QtCore import QtMsgType, QUrl, qInstallMessageHandler
from PySide2.QtGui import QGuiApplication
from PySide2.QtQml import QQmlComponent, QQmlEngine

captured = []


def handler(mode, ctx, msg):
    # Only warning severity and above. console.log from QML arrives as
    # QtDebugMsg/QtInfoMsg and is not a failure; binding errors arrive as
    # QtWarningMsg.
    if int(mode) >= int(QtMsgType.QtWarningMsg):
        captured.append(msg)


qInstallMessageHandler(handler)

qml_dir = sys.argv[1]
targets = sys.argv[2:]
if not targets:
    targets = []
    for root, _d, files in os.walk(qml_dir):
        for fn in sorted(files):
            if fn.endswith(".qml") and fn != "Theme.qml":
                targets.append(os.path.relpath(os.path.join(root, fn), qml_dir))

app = QGuiApplication(sys.argv[:1])
engine = QQmlEngine()
engine.addImportPath(qml_dir)

# main.qml reads the `scriptModel` context property that C++ normally supplies.
# Without it every run reports a ReferenceError, and a genuine new error would
# be easy to miss among the expected noise. Held in a variable because
# setContextProperty does not take ownership.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mockmodel import MockModel, MockLayoutStore  # noqa: E402

_model = MockModel()
engine.rootContext().setContextProperty("scriptModel", _model)

# main.qml 启动期就读 layoutStore（折叠状态恢复），必须先于加载存在。
_layout = MockLayoutStore()
engine.rootContext().setContextProperty("layoutStore", _layout)

# main.qml reads more context properties than scriptModel -- C++ main.cpp
# also supplies startupRequest / scriptRunner / dropDebugMode at runtime, and
# main.qml grew references to them after this harness first stubbed only
# scriptModel. Without stubs every main.qml run reports one ReferenceError per
# reference, and a genuine new error would be easy to miss among the expected
# noise -- the exact reason scriptModel is stubbed above. Shapes mirror
# main.cpp: an empty startup request (no CLI run) and an idle runner.
from PySide2.QtCore import QObject, Property, Signal, Slot  # noqa: E402

engine.rootContext().setContextProperty("startupRequest", {"script": "", "files": []})
engine.rootContext().setContextProperty("dropDebugMode", False)


class _StubRunner(QObject):
    """Idle ScriptRunner stand-in with the members main.qml reads at
    instantiation time (running/spawnError/hasRun/lastExitCode/tail).
    run() exists for completeness but is never called here -- exercising it
    would need the C++ process machinery."""

    _changed = Signal()

    def _running(self):
        return False

    def _spawnError(self):
        return ""

    def _hasRun(self):
        return False

    def _lastExitCode(self):
        return 0

    def _tail(self):
        return ""

    running = Property(bool, _running, notify=_changed)
    spawnError = Property(str, _spawnError, notify=_changed)
    hasRun = Property(bool, _hasRun, notify=_changed)
    lastExitCode = Property(int, _lastExitCode, notify=_changed)
    tail = Property(str, _tail, notify=_changed)

    @Slot(str, list, "QVariant")
    def run(self, script, files, values):
        pass


_runner = _StubRunner()
engine.rootContext().setContextProperty("scriptRunner", _runner)

warns = []
engine.warnings.connect(lambda ws: warns.extend(w.toString() for w in ws))

fail = 0
for t in targets:
    captured.clear()
    del warns[:]
    path = os.path.join(qml_dir, t)
    comp = QQmlComponent(engine, QUrl.fromLocalFile(path))
    if comp.isError():
        fail = 1
        print(f"[PARSE-ERROR] {t}")
        for e in comp.errors():
            print("     ", e.toString())
        continue

    obj = comp.create()
    if obj is None:
        fail = 1
        print(f"[CREATE-FAIL] {t}")
        for e in comp.errors():
            print("     ", e.toString())
        continue

    # Binding errors during create() arrive as engine warnings / qWarning text,
    # not as component errors, so check both channels.
    noise = [m for m in captured if "XDG_RUNTIME_DIR" not in m]
    if warns or noise:
        fail = 1
        print(f"[BINDING-WARN] {t}")
        for w in list(warns) + noise:
            print("     ", w)
    else:
        print(f"[ok] {t}")
    obj.deleteLater()

sys.exit(fail)

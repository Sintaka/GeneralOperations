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
from mockmodel import MockModel  # noqa: E402

_model = MockModel()
engine.rootContext().setContextProperty("scriptModel", _model)

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

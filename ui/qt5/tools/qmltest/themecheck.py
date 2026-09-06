"""Verify every Theme.<prop> referenced in the QML actually exists on the singleton.

QQmlComponent.loadUrl only parses; a reference to a nonexistent singleton
property is a *runtime* binding error, so it passes parsing and then silently
evaluates to undefined. That failure mode is exactly what we need to catch:
undefined color -> transparent, undefined int -> 0, no error message.

Approach: instantiate the Theme singleton for real, enumerate its properties,
then grep every Theme.X occurrence out of the QML sources and diff the sets.
"""
import os
import re
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide2.QtCore import QUrl
from PySide2.QtGui import QGuiApplication
from PySide2.QtQml import QQmlComponent, QQmlEngine

qml_dir = sys.argv[1]

app = QGuiApplication(sys.argv[:1])
engine = QQmlEngine()
engine.addImportPath(qml_dir)

# Theme.qml is `pragma Singleton`, which QQmlComponent refuses to load
# directly. So go through the module the way real code does: a tiny wrapper
# that does `import App 1.0` and hands the singleton back as a property.
wrapper = (
    "import QtQuick 2.15\n"
    "import App 1.0\n"
    "QtObject { property var t: Theme }\n"
)
comp = QQmlComponent(engine)
comp.setData(wrapper.encode("utf-8"), QUrl.fromLocalFile(os.path.join(qml_dir, "_probe.qml")))
if comp.isError():
    print("[FAIL] cannot import App module / Theme singleton:")
    for e in comp.errors():
        print("   ", e.toString())
    sys.exit(1)

holder = comp.create()
if holder is None:
    print("[FAIL] probe wrapper did not instantiate:")
    for e in comp.errors():
        print("   ", e.toString())
    sys.exit(1)

obj = holder.property("t")
if obj is None:
    print("[FAIL] Theme singleton resolved to null")
    sys.exit(1)

mo = obj.metaObject()
defined = set()
for i in range(mo.propertyCount()):
    defined.add(mo.property(i).name())

# Collect Theme.<name> references from all QML except Theme.qml itself.
refs = {}
for root, _dirs, files in os.walk(qml_dir):
    for fn in sorted(files):
        if not fn.endswith(".qml") or fn == "Theme.qml":
            continue
        path = os.path.join(root, fn)
        rel = os.path.relpath(path, qml_dir)
        with open(path, encoding="utf-8") as fh:
            for lineno, line in enumerate(fh, 1):
                # Strip // comments so documentation mentioning old names
                # (e.g. "上一版 Theme.fog") does not cause false failures.
                code = re.sub(r"//.*$", "", line)
                for name in re.findall(r"\bTheme\.([A-Za-z_][A-Za-z0-9_]*)", code):
                    refs.setdefault(name, []).append(f"{rel}:{lineno}")

missing = {n: locs for n, locs in refs.items() if n not in defined}

print(f"Theme defines {len(defined)} properties; QML references {len(refs)} distinct.")

if missing:
    print("\n[FAIL] referenced but NOT defined on Theme:")
    for name in sorted(missing):
        print(f"  Theme.{name}")
        for loc in missing[name]:
            print(f"      {loc}")
    sys.exit(1)

print("[ok] every Theme.<prop> reference resolves.")

# Informational: tokens defined but never used. Not a failure -- a token can
# legitimately be staged ahead of the slice that consumes it -- but a long
# list usually means dead theme entries.
unused = sorted(
    n for n in defined
    if n not in refs and not n.startswith("_") and n != "objectName"
)
if unused:
    print(f"\n[info] defined but unused ({len(unused)}): {', '.join(unused)}")
sys.exit(0)

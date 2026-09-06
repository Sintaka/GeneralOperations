"""Shared mock of the C++ ScriptListModel, for the QML test harnesses.

Implements the same role names and the same two Q_INVOKABLEs (scanErrors,
groupCount) as model/ScriptListModel, so the QML under test exercises its real
code paths: section headers, collapse bindings, the group-count badge, and the
invalid/destructive row states.
"""
from PySide2.QtCore import (
    Property,
    QAbstractListModel,
    QByteArray,
    QModelIndex,
    Qt,
    Signal,
    Slot,
)

ROLES = [
    "name", "group", "desc", "filePath", "valid", "errorText",
    "destructive", "destructiveReason", "paramCount", "requiresText", "host",
]

# Mirrors the real registry output: grouped, stable order, includes an invalid
# entry and destructive entries because those drive distinct visual states.
DATA = [
    dict(name="Flip Horizontal", group="Image", desc="水平翻转图像",
         filePath="D:/GeneralOperations/img.FlipImage_Horizontal.py",
         valid=True, errorText="", destructive=False, destructiveReason="",
         paramCount=0, requiresText="Pillow", host="python"),
    dict(name="Resize to 1k/2k/4k", group="Image", desc="按最长边缩放",
         filePath="D:/GeneralOperations/img.Resize.py",
         valid=True, errorText="", destructive=True,
         destructiveReason="输入为 jpg 时会静默覆盖原文件",
         paramCount=1, requiresText="Pillow", host="python"),
    dict(name="EXR to PNG (4K)", group="Image", desc="流式读取 + mipmap",
         filePath="D:/GeneralOperations/img.EXR2PNG_Large.py",
         valid=True, errorText="", destructive=False, destructiveReason="",
         paramCount=2, requiresText="OpenEXR, numpy, opencolorio?", host="python"),
    dict(name="Zbrush UDIM Correction", group="Image", desc="修正 UDIM 编号",
         filePath="D:/GeneralOperations/img.Zbrush_UDIM_Correction.py",
         valid=True, errorText="", destructive=True,
         destructiveReason="原地覆盖 EXR 且重命名 UDIM 编号，不可逆",
         paramCount=0, requiresText="OpenEXR", host="python"),
    dict(name="Cross Split", group="Image", desc="十字切分",
         filePath="D:/GeneralOperations/img.crossSplit.py",
         valid=True, errorText="", destructive=False, destructiveReason="",
         paramCount=0, requiresText="Pillow", host="python"),
    dict(name="pmx to fbx", group="Geometry", desc="MMD 模型转 FBX",
         filePath="D:/GeneralOperations/geo.pmx2fbx.py",
         valid=True, errorText="", destructive=False, destructiveReason="",
         paramCount=1, requiresText="bpy, mmd_tools", host="blender"),
    dict(name="Flatten Folder Hierarchy", group="System", desc="打平目录层级",
         filePath="D:/GeneralOperations/os.FlattenFolderHierarchy.py",
         valid=True, errorText="", destructive=False, destructiveReason="",
         paramCount=1, requiresText="psutil?", host="python"),
    dict(name="broken_script", group="System", desc="",
         filePath="D:/GeneralOperations/os.broken.py",
         valid=False, errorText="缺少 @name 声明; @param 语法错误 (行 12)",
         destructive=False, destructiveReason="",
         paramCount=0, requiresText="", host="python"),
]


class MockModel(QAbstractListModel):
    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(DATA)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid():
            return None
        key = ROLES[role - (Qt.UserRole + 1)] if role >= Qt.UserRole + 1 else None
        if key is None:
            return None
        return DATA[index.row()].get(key)

    def roleNames(self):
        return {Qt.UserRole + 1 + i: QByteArray(n.encode()) for i, n in enumerate(ROLES)}

    # --- the Q_INVOKABLEs the QML calls ---
    #
    # @Slot is required, not decorative. A plain Python method on a QObject is
    # invisible to QML, so `model.groupCount` evaluates to undefined and the
    # QML falls through to its `: ""` fallback -- the count badge silently
    # renders empty and looks like a product bug. @Slot is what makes these
    # the equivalent of Q_INVOKABLE in the real C++ model.
    @Slot(result="QStringList")
    def scanErrors(self):
        return []

    @Slot(str, result=int)
    def groupCount(self, group):
        return sum(1 for d in DATA if d["group"] == group)

    @Slot(str)
    def load(self, d):
        pass

    countChanged = Signal()

    def _count(self):
        return len(DATA)

    # Mirrors Q_PROPERTY(int count READ rowCount NOTIFY countChanged).
    # A Python @property would not be visible to QML either.
    count = Property(int, _count, notify=countChanged)



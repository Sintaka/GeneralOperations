# ============================================================================
# ui/qt6.8lts —— Qt 6.8 LTS（QML）前端占位
# ============================================================================
#
# 本文件目前只做一件事：configure 到这里立即 FATAL_ERROR。
#
# 触发路径：根 CMakeLists.txt 的 qt6.8lts 分支会 add_subdirectory(ui/qt6.8lts)，
# 即 -DGO_FRONTEND=qt6.8lts 时在 configure 期就撞上下面的报错。这是有意的：
# 占位目录要"保留位置、拒绝构建"——误选的人第一秒就得到明确指引，而不是留一个
# 空目录让 CMake 静默通过、到构建期才发现里面什么都没有。也因此本文件刻意
# 不写任何 target、不调 cmake_minimum_required：占位不需要能 configure 成功。
#
# 为什么占位也先建目录：monorepo 的前端槽位（接入步骤见
# docs/ARCHITECTURE.md「新增一个前端」）先占住名字，避免将来接入时才发现
# 目录名 / 根分支 / preset 名被占用或拼写漂移。
#
# ---- 迁移要点（预记，接入时照此展开为真正的 CMakeLists）----
# * Qt 6.8 LTS + QML。沿用同一套脚本契约（docs/SCRIPT_SPEC.md）与 bundle
#   装配管线（GO_CORE_SCRIPTS_DIR / GO_BUNDLE_DIR / GO_ZIP_FILE 等根契约
#   全部照旧），只换 Qt 层——前端对后端零假设正是分层的意义。
# * QML 从 ui/qt5 演进而非重写：视觉与交互已在第二代定稿（设计来源见
#   ui/qt5/README.md），增量迁移，别推倒重来。
# * Qt6 的 import 语句不带版本号：import QtQuick，不是 import QtQuick 2.15。
# * 构建入口用 qt_add_qml_module（Qt6 标准 QML 模块机制），替代 Qt5 时代
#   AUTORCC + 手工维护 qmldir 的做法。
# * Quick Controls2 的样式机制在 Qt6 有变化（styles/ 目录 + qtquickcontrols2.conf
#   按目标选择样式），若引入 Controls 需按 Qt6 方式配，别照搬 Qt5 的 import
#   覆盖法。
# * 打包用 windeployqt6（二进制名不同），bundle 管线的 BUNDLE_WDT 指向需
#   随之调整；preset 命名按预留模板 qt68lts-mingw-debug 这类
#   （<前端><工具链>-<配置>，见 docs/ARCHITECTURE.md「preset 命名」）。
# ============================================================================

message(FATAL_ERROR
    "ui/qt6.8lts 仅保留位置，尚未实现。"
    " 当前可用前端：qt5（-DGO_FRONTEND=qt5，默认）。")

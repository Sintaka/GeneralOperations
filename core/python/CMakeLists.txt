# ============================================================================
# core/python —— Python 内核脚本层
# ============================================================================
#
# 为什么这里不声明工程：
#   project 命令由仓库根 CMakeLists 统一调用。core/* 只是根工程挂载的子目录
#   （add_subdirectory），各自再声明一次会生成重复的编译器探测和 VS 解决
#   方案文件夹，毫无收益。
#
# 为什么本目录不构建任何东西：
#   Python 没有"编译"步骤，.py 源文件本身就是交付物，随包分发源码即可。
#   这里自然也没有任何 target、任何构建产物。
#
# 为什么"脚本即契约"：
#   每个脚本模块 docstring 顶部的声明块（@name/@group/@desc/@accepts/
#   @requires/@param/...，规范见根目录 docs/SCRIPT_SPEC.md）是唯一事实来源。
#   启动器只读 docstring，从不解析 Python 代码——因此脚本内容对 C++/CMake
#   侧完全透明：加脚本、改参数都不需要动任何构建文件。
#
# 为什么打包也不在这里做：
#   requirements.txt 汇总、内嵌 Python 运行时等打包工作，由前端侧的 bundle
#   管线在构建期统一完成，本目录不重复实现。
#
# 本文件唯一的职责：向父作用域导出脚本目录路径。
# 根 CMakeLists 依赖 GO_PYTHON_SCRIPTS_DIR 定义 GO_CORE_SCRIPTS_DIR 并据此
# 收集/安装脚本——变量名是跨层契约，一字不能改。
set(GO_PYTHON_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts" PARENT_SCOPE)

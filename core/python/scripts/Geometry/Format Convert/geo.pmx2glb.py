r"""
@name        PMX to GLB
@group       Geometry/Format Convert
@desc        将 MMD PMX 模型转换为 GLB（贴图内嵌），输出到 PMX 同目录同名 .glb
@accepts     file
@ext         .pmx
@multi       false
@host        exe
@exe         go_pmx2glb

@param  scale : choice : 0.08 : 输出缩放 : 0.08|1.0
"""

# 本文件只是契约声明（SCRIPT_SPEC docstring 头），不是可执行脚本。
# 执行体是 @exe 指向的 C++ 后端可执行文件（go_pmx2glb，由 core/cpp 构建，
# 发行包位于 tools/go_pmx2glb/）。启动器读到 @host exe 时直接 spawn 该
# 可执行文件，本 .py 永远不会被 Python 运行。

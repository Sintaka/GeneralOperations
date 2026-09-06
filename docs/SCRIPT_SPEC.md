# 脚本契约规范

> 脚本实体位于 `core/python/scripts/`；本契约（docstring 声明块）由启动器前端与打包管线共同消费。

启动器不解析 Python 代码，只读每个脚本模块 docstring 顶部的声明块。加脚本 = 放一个 `.py` 进 `scripts/` 并写好这个头，启动器下次启动自动发现，C++ 侧不用改。

## 完整示例

```python
"""
@name        Resize to JPEG
@group       Image
@desc        等比缩放长边到指定像素并转为 JPEG，输出到源目录：原名_<长边像素>px.jpg，同名直接覆盖
@accepts     file
@ext         .jpg .jpeg .png .bmp .gif .tiff .tif .webp .ico .jfif
@multi       true
@requires    Pillow

@param  max_pixels : int    : 1024 : 长边像素上限 : 256..16384 : presets 1024|2048|4096
@param  quality    : int    : 80   : JPEG 质量    : 1..100
@param  workers    : int    : 0    : 线程数(0=自动CPU数,上限8) : 0..64
"""
```

## 元信息键

| 键 | 必填 | 说明 |
|---|---|---|
| `@name` | ✅ | Outliner 里显示的名字 |
| `@group` | ✅ | Outliner 分组。当前用 `Image` / `Geometry` / `System` |
| `@desc` | ✅ | 一句话说明，显示在参数面板顶部 |
| `@accepts` | ✅ | `file` / `dir` / `both` —— 决定拖拽时接受什么 |
| `@ext` | | 空格分隔的扩展名白名单，带点，小写。省略 = 不限 |
| `@multi` | | `true`(默认) / `false`。false 表示一次只处理一个目标 |
| `@requires` | | 空格分隔的 pip 包名。启动器据此在运行前检查/安装 |
| `@destructive` | | 有这行就是破坏性操作，**值是给用户看的原因**，执行前弹二次确认 |
| `@host` | | `python`(默认) / `blender`。见下 |
| `@blender` | | Blender 主程序完整路径，可含空格（如 `C:/Program Files/Blender 5.2/blender.exe`），取行内剩余全部内容。`@host blender` 时必填，缺了会被运行器明确拒绝 |

`@ext` 写的是**扩展名**不是 glob，`.jpg` 而不是 `*.jpg`。大小写不敏感匹配，但声明时统一写小写。

## 参数声明

```
@param  <名字> : <类型> : <默认值> : <标签> [: <约束>]
```

冒号分隔，字段两侧空白忽略。前四段必填，第五段按类型可选，第六段只有 int/float 用。

| 类型 | 约束写法 | 生成的控件 |
|---|---|---|
| `int` | `min..max` | 滑块（拖动吸附整数）；有 `presets` 时先给快捷档位胶囊 + "自定义" |
| `float` | `min..max` | 滑块 |
| `bool` | 无 | 复选框（默认值写 `true`/`false`） |
| `choice` | `a\|b\|c` | 互斥胶囊组 |
| `str` | 无 | 暂不支持编辑，用默认值 |
| `path` | 无 | 暂不支持编辑，用默认值 |

`int`/`float` 省略约束则不限范围。`choice` 的约束必填，默认值必须是候选之一。

第 6 段是**预设档位**（可选）：`presets 1024|2048|4096`。参数面板会把它们渲染成
快捷档位胶囊（能被 1024 整除的值显示成 1K/2K/4K），另带一个"自定义"档位展开滑块。
写和不写都不影响命令行映射 —— 预设只是把某个值送进当前参数。

```
@param  max_pixels : int : 2048 : 长边像素上限 : 256..16384 : presets 1024|2048|4096
```

## 命令行映射

参数名下划线转连字符加 `--` 前缀，文件列表放在 `--` 之后：

```
python <脚本> --max-pixels 2048 --quality 85 --workers 0 -- <file1> <file2> ...
```

- `bool` 为 true 时传 `--flag`，false 时**不传**（store_true 语义）
- 其余类型传 `--key value`
- `--` 分隔符是硬要求，避免文件名以连字符开头时被当成选项

脚本侧对应写法：

```python
import argparse, sys

def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("--max-pixels", type=int, default=2048)
    p.add_argument("--quality",    type=int, default=85)
    p.add_argument("--workers",    type=int, default=0)
    p.add_argument("files", nargs="*")
    return p

if __name__ == "__main__":
    args = build_parser().parse_args()
    sys.exit(main(args))
```

**docstring 里的默认值必须和 argparse 的默认值一致。** 两处重复是这套设计的已知代价 —— 换来的是 C++ 侧完全不用理解 Python。改参数时记得改两处。

## 移植时的硬规则

**0. 脚本是纯后端，禁止依赖 Qt。** 不许 import PyQt/PySide/qtpy —— Qt 只存在于启动器前端（打包时会检查并直接报错）。Python 环境与 Qt 的版本、许可完全解耦，这样脚本才能跑在任何没有 GUI 库的部署形态里。

**1. 不许有阻塞调用。** `input()`、`os.system('pause')`、`msvcrt.getch()` 一律删掉。脚本是被 `QProcess` 托管的，这些会直接挂住子进程。需要告诉用户什么，`print` 就行，启动器会把 stdout/stderr 收进日志区。

**2. 进度信息写 stdout，一行一条。** 启动器逐行读取显示。别用 `\r` 原地刷新进度条 —— 在管道里那是一坨。

**3. 退出码要准确。** 成功 `0`，失败非 `0`。启动器靠这个判断成败并在 UI 上标红。批处理多个文件时，部分失败也应返回非 0，但要把成功的那些做完，别中途退出。

**4. 不要自己弹 GUI。** 没有 tkinter、没有 messagebox。UI 是启动器的职责。

**5. 输出路径规则写进 `@desc` 或 `@destructive`。** 用户在执行前需要知道文件会落在哪。

## `@host blender` 的特殊情况

`geo.pmx2fbx.py` 依赖 `bpy`，只能由 Blender 内置的 Python 跑。启动器对 `@host blender` 的脚本改用：

```
<blender.exe> --background --python <脚本> -- <参数与文件>
```

Blender 主程序路径写在脚本头的 `@blender` 键里 —— 与整套契约的设计一致：启动器不解析 Python 代码、不猜安装位置，所有信息都来自 docstring。路径可含空格，取行内剩余全部内容；没配 `@blender` 的 `@host blender` 脚本会在运行前被拒绝，提示补上这一行，换机器时改脚本头一处即可。

这类脚本的 `@requires` 不走 pip —— 依赖装在 Blender 自己的 Python 环境里，启动器不管。

## 解析实现约定

C++ 侧只扫 docstring 第一段（文件开头的 `"""` 到 `"""`），逐行正则匹配 `^@(\w+)\s+(.*)$`。

- 未知的 `@key` 忽略并记一条警告，不算错误 —— 这样加新键不会让旧启动器崩
- 缺必填键的脚本在 Outliner 里显示为灰色且不可执行，鼠标悬停给出原因，而不是静默跳过（静默跳过会让人以为脚本没被发现，白折腾半天）

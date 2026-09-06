#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 src/assets/app.ico —— 黑底白色双蛇（Python 风格）应用图标。

【为什么程序化生成】图标必须跟 UI 的黑底白字风格一致；几何参数写进脚本，
改一处即可重出全套尺寸，源头可评审、可复现，仓库里不用养一份二进制源稿。

【几何设计】画布 256×256，RGBA：背景是一块圆角黑底，半径 56（约 22% 边长，
观感对齐 macOS/iOS 应用图标），四角完全透明（alpha=0）。整幅先按 4 倍
（1024）超采样绘制再缩回——圆角直接在 256 上画是硬边，放大白拿一次抗锯齿。
两个部件互为 180° 旋转对称（绕画布中心 (128,128)），各含一枚黑色小圆眼睛：

  上蛇（白）：三块圆角矩形并集（256 坐标，r=16）
    B1 头/顶横杠 [70, 24, 170, 64]   头在左端，眼睛圆心 (99,46) r=8
    B4 身体      [30, 60, 170, 124]  底边 y=124，止于中线 128 上方 4px
    B3 左尾      [30, 66,  72, 170]  从身体左肩下垂，越过中线到 y=170
  下蛇 = 上蛇绕中心旋转 180°，坐标由 rot180() 自动生成（不单独手调，
  对称性靠构造保证而不是靠对表）：
    底杠 [86,192,186,232]、身体 [86,132,226,196]、右尾 [184,86,226,190]

  互锁关系：上蛇右臂止于中线上方、下蛇左臂止于中线下方（纵向间隙 8px）；
  上蛇左尾越过中线、下蛇右尾越过中线，两者分别与对方的臂横向错开 14px。
  两蛇间隙 8~14px，缩到 16px 时仍有约 0.5px 分隔，互锁轮廓可辨认。

  沿背景圆角边缘加一圈 8% 白的极细圆角描边（RGB 20,20,20，内缩 2px、
  宽 2px，描边外弧与背景弧同心，半径 56-2=54，完全落在不透明区内）：
  可选装饰，为的是纯黑底在深色任务栏上不至于完全隐身。

【输出】256/128/64/48/32/24/16 七档打进单个 ICO（save(sizes=...)，
由 256 主图 LANCZOS 缩出）。脚本幂等：可重复运行，原地覆盖同一文件。
"""

from pathlib import Path

from PIL import Image, ImageDraw

# 前端根（ui/qt5）= tools/ 的上一级。输出路径钉在前端源码树上（ui/qt5/src/
# assets/app.ico），不依赖运行时的 cwd —— 从仓库根执行 `python
# ui/qt5/tools/gen_icon.py`，或从任何其他目录执行，结果都一致。
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "assets" / "app.ico"

CANVAS = 256   # ICO 最大档尺寸
SS = 4         # 超采样倍数：先画成 1024 再缩回 256
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
EDGE = (20, 20, 20)  # 8% 白：极细描边，纯黑底在深色任务栏上的轮廓提示

# 圆角背景（256 坐标系）：四角透明，描边贴边内缩、外弧与背景弧同心。
BG_RADIUS = 56   # 背景圆角半径，约 22% 边长（macOS/iOS 图标观感）
EDGE_INSET = 2   # 描边相对背景边缘的内缩量，描边半径 = BG_RADIUS - EDGE_INSET

# 上蛇三块圆角矩形（256 坐标系），含义见文件头【几何设计】。
BLUE_RECTS = [
    (70, 24, 170, 64),    # B1 头/顶横杠
    (30, 60, 170, 124),   # B4 身体
    (30, 66, 72, 170),    # B3 左尾
]
BLUE_EYE = (99, 46, 8)  # (cx, cy, r) 头上的黑色小圆眼睛
RADIUS = 16


def rot180_rect(rect):
    """矩形绕画布中心旋转 180°。下蛇完全由它派生，保证严格对称。"""
    x0, y0, x1, y1 = rect
    return (CANVAS - x1, CANVAS - y1, CANVAS - x0, CANVAS - y0)


def rot180_point(pt):
    """点绕画布中心旋转 180°（眼睛坐标用）。"""
    cx, cy = pt
    return (CANVAS - cx, CANVAS - cy)


def draw_snake(draw, rects, eye):
    """画一条蛇：白色圆角矩形并集 + 黑色小圆眼睛。坐标统一乘 SS 换到超采样层。"""
    for x0, y0, x1, y1 in rects:
        draw.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS],
                               radius=RADIUS * SS, fill=WHITE)
    cx, cy, r = eye
    draw.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS],
                 fill=BLACK)


def render_master():
    """画 256×256 主图：圆角黑底（透明四角）+ 细描边 + 双蛇。"""
    side = CANVAS * SS
    img = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # 圆角黑底：四角留全透明，圆角抗锯齿靠超采样 + LANCZOS 缩回得到。
    d.rounded_rectangle([0, 0, side, side], radius=BG_RADIUS * SS, fill=BLACK)

    # 极细描边：贴着背景圆角边缘内缩 EDGE_INSET，外弧与背景弧同心
    # （radius = BG_RADIUS - EDGE_INSET），保证完全落在不透明区内。
    # 刻意压到 8% 白、2px，肉眼只是"黑没那么死"。
    m = EDGE_INSET * SS
    d.rounded_rectangle([m, m, side - m, side - m],
                        radius=(BG_RADIUS - EDGE_INSET) * SS,
                        outline=EDGE, width=2 * SS)

    draw_snake(d, BLUE_RECTS, BLUE_EYE)
    # 下蛇眼睛 = 上蛇眼睛旋转 180°，半径不变（BLUE_EYE[:2] 去掉 r 分量）。
    draw_snake(d, [rot180_rect(r) for r in BLUE_RECTS],
               rot180_point(BLUE_EYE[:2]) + (BLUE_EYE[2],))

    # 超采样层缩回目标尺寸，LANCZOS 保小尺寸下的边缘质量。
    return img.resize((CANVAS, CANVAS), Image.LANCZOS)


def main():
    master = render_master()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    master.save(OUT, format="ICO",
                sizes=[(256, 256), (128, 128), (64, 64), (48, 48),
                       (32, 32), (24, 24), (16, 16)])

    # 回读校验：确认真写进了全部尺寸，而不是静默只出单档。
    with Image.open(OUT) as ico:
        sizes = ico.info.get("sizes") or sorted(ico.ico.sizes())
        print(f"OK {OUT.relative_to(ROOT)}  max={ico.size}  sizes={sorted(sizes)}")


if __name__ == "__main__":
    main()

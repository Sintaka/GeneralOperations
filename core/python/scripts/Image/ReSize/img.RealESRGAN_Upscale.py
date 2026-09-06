"""
@name        Real-ESRGAN Upscale
@group       Image/ReSize
@desc        用 Real-ESRGAN(ncnn-vulkan, 发行包自带) AI 放大图片，输出到源目录：原名_x<倍率>.<原扩展名>（photo.png → photo_x2.png，1.5 倍 → photo_x1.5.png，1 倍 = 原尺寸过一遍模型），同名直接覆盖
@accepts     file
@ext         .png .jpg .jpeg .webp
@multi       true
@requires    Pillow

@param  scale : float : 2 : 放大倍率 : 1..4 : presets 1|2|3
"""

import argparse
import math
import os
import subprocess
import sys
from pathlib import Path

from PIL import Image

# 可执行文件在发行包里的相对位置：<包根>/tools/realesrgan/realesrgan-ncnn-vulkan.exe
# 脚本本体在 <包根>/scripts/Image/ReSize/，从脚本目录向上最多找 5 层。
EXE_NAME = "realesrgan-ncnn-vulkan.exe"
MAX_UPLEVELS = 5

# 工具 -s 只原生支持整数 2/3/4（-s 1/1.5 会因缺 x1 模型报错退出码 127）。
# 其它倍率（含 1 倍"过一下"）先按 ceil 放大再等比缩回目标尺寸。
NATIVE_SCALES = (2, 3, 4)

# 工具实际支持的输入格式（实测：png/jpg/jpeg/webp 可用，bmp 会失败）
SUPPORTED_EXTS = {".png", ".jpg", ".jpeg", ".webp"}

# Pillow 保存时的格式映射（兜底缩放路径重新落盘用）
_SAVE_FMT = {".png": "PNG", ".jpg": "JPEG", ".jpeg": "JPEG", ".webp": "WEBP"}


def find_tool():
    """从脚本所在目录向上逐级查找发行包自带的工具，返回 exe 路径或 None。"""
    here = Path(__file__).resolve().parent
    for _ in range(MAX_UPLEVELS):
        cand = here / "tools" / "realesrgan" / EXE_NAME
        if cand.is_file():
            return cand
        parent = here.parent
        if parent == here:
            break
        here = parent
    return None


def plan_scale(scale):
    """把任意 1..4 倍率映射成 (工具倍率, 是否需要事后缩放)。"""
    if float(scale).is_integer() and int(scale) in NATIVE_SCALES:
        return int(scale), False
    tool_scale = min(4, max(2, math.ceil(scale)))
    return tool_scale, True


def scale_tag(scale):
    """倍率的文件名片段：2.0 -> '2'，1.5 -> '1.5'。"""
    return "%g" % scale


def process_one(exe, models_dir, path, scale):
    """处理单张图片。返回 (ok, 提示信息)。"""
    base, ext = os.path.splitext(path)
    ext_l = ext.lower()
    out_path = f"{base}_x{scale_tag(scale)}{ext_l}"
    tool_scale, need_resize = plan_scale(scale)

    cmd = [str(exe), "-i", path, "-o", out_path, "-s", str(tool_scale)]
    if models_dir is not None:
        cmd += ["-m", str(models_dir)]

    # 工具的 stdout/stderr 直接继承本进程（透传给启动器日志区），不捕获不吞掉
    try:
        proc = subprocess.run(cmd)
    except OSError as e:
        return False, f"无法启动工具: {e}"

    if proc.returncode != 0:
        return False, f"工具退出码 {proc.returncode}（-s {tool_scale}）"
    if not os.path.isfile(out_path):
        return False, f"工具未产出输出文件: {os.path.basename(out_path)}"

    if need_resize:
        # 兜底：非原生倍率（含 1 倍）先被 ceil 倍放大，再等比缩回目标尺寸。
        # 目标尺寸按 源尺寸×scale 换算 —— 即 输出尺寸×(scale/tool_scale)。
        try:
            with Image.open(out_path) as img:
                w, h = img.size
                target = (max(1, round(w * scale / tool_scale)),
                          max(1, round(h * scale / tool_scale)))
                resized = img.resize(target, Image.LANCZOS)
                fmt = _SAVE_FMT.get(ext_l)
                if fmt == "JPEG":
                    resized.save(out_path, fmt, quality=95)
                else:
                    resized.save(out_path, fmt)
        except Exception as e:
            return False, f"缩放到目标尺寸失败: {e}"

    with Image.open(out_path) as img:
        out_size = img.size
    return True, f"→ {os.path.basename(out_path)} ({out_size[0]}x{out_size[1]})"


def main(args):
    # 脚本被 QProcess 托管时 stdout 是管道，默认块缓冲会吞行，改成按行刷出
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    if not (1.0 <= args.scale <= 4.0):
        print(f"✗ 倍率必须在 1..4 之间，当前 {args.scale}")
        return 1

    exe = find_tool()
    if exe is None:
        print(f"✗ 找不到 {EXE_NAME}：已从脚本目录向上 {MAX_UPLEVELS} 层查找 "
              f"tools/realesrgan/{EXE_NAME}。请确认使用发行包（工具随包在 tools/ 下），"
              f"不要依赖系统 PATH。")
        return 1

    models_dir = exe.parent / "models"
    models_dir = models_dir if models_dir.is_dir() else None

    paths = []
    failed = 0
    for path in args.files:
        if not os.path.isfile(path):
            print(f"⚠ 跳过（文件不存在）：{path}")
            failed += 1
            continue
        ext = os.path.splitext(path)[1].lower()
        if ext not in SUPPORTED_EXTS:
            print(f"⚠ 跳过（工具不支持的格式 {ext or '无扩展名'}）："
                  f"{os.path.basename(path)}")
            failed += 1
            continue
        paths.append(path)

    if not paths:
        print("没有可处理的图片（支持 png/jpg/jpeg/webp）。")
        return 1

    total = len(paths)
    tool_scale, need_resize = plan_scale(args.scale)
    print(f"共 {total} 张，倍率 x{scale_tag(args.scale)}"
          + ("" if not need_resize else f"（先按 x{tool_scale} 放大再缩回）")
          + f"，输出 原名_x{scale_tag(args.scale)}.原扩展名，同名直接覆盖")

    for idx, path in enumerate(paths, 1):
        name = os.path.basename(path)
        ok, msg = process_one(exe, models_dir, path, args.scale)
        if ok:
            print(f"[{idx}/{total}] ✓ {name} {msg}")
        else:
            print(f"[{idx}/{total}] ✗ {name}: {msg}")
            failed += 1

    print(f"完成：成功 {total - failed} 张，失败 {failed} 张")
    return 0 if failed == 0 else 1


def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("--scale", type=float, default=2)
    p.add_argument("files", nargs="*")
    return p


if __name__ == "__main__":
    sys.exit(main(build_parser().parse_args()))

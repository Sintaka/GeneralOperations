"""
@name        Resize to JPEG
@group       Image/ReSize
@desc        等比缩放长边到指定像素并转为 JPEG，输出到源目录：原名_<长边像素>px.jpg（photo.png → photo_1024px.jpg），同名直接覆盖
@accepts     file
@ext         .jpg .jpeg .png .bmp .gif .tiff .tif .webp .ico .jfif
@multi       true
@requires    Pillow

@param  max_pixels : int : 1024 : 长边像素上限            : 256..16384 : presets 1024|2048|4096
@param  quality    : int : 80   : JPEG 质量               : 1..100
@param  workers    : int : 0    : 线程数(0=自动CPU数,上限8) : 0..64
"""

import argparse
import os
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed

from PIL import Image

# 支持的图片扩展名
SUPPORTED_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tiff", ".tif",
                  ".webp", ".ico", ".jfif", ".pjpeg", ".pjp"}

_print_lock = threading.Lock()

def resize_to_limit(img: Image.Image, max_pixels: int) -> Image.Image:
    """将图片长边缩放到 max_pixels 以内，保持宽高比。"""
    w, h = img.size
    longest = max(w, h)
    if longest <= max_pixels:
        return img  # 已经足够小，不需要缩放
    ratio = max_pixels / longest
    new_size = (int(w * ratio), int(h * ratio))
    return img.resize(new_size, Image.LANCZOS)


def process_one(path: str, max_pixels: int, quality: int) -> dict:
    """处理单张图片，返回结果字典（线程安全）。"""
    name = os.path.basename(path)
    try:
        orig_size = os.path.getsize(path)
        # 输出名带上像素上限（photo.png → photo_1024px.jpg）：不同档位的产物
        # 可以并存，也永远不会撞上用户输入的原始文件。目标已存在时
        # Image.save 默认直接覆盖，这里不做任何确认 —— 覆盖范围止步于
        # "同名 _<N>px.jpg 产物"，原图永远是安全的。
        base, _ = os.path.splitext(path)
        output_path = f"{base}_{max_pixels}px.jpg"

        with Image.open(path) as img:
            # 转 RGB（兼容 RGBA / P 模式等）
            if img.mode in ("RGBA", "P", "LA"):
                img = img.convert("RGBA")
                background = Image.new("RGB", img.size, (255, 255, 255))
                background.paste(img, mask=img.split()[3])
                img = background
            elif img.mode != "RGB":
                img = img.convert("RGB")

            img = resize_to_limit(img, max_pixels)
            img.save(output_path, "JPEG", quality=quality, optimize=True)

        new_size = os.path.getsize(output_path)
        return {
            "ok": True,
            "name": name,
            "out_name": os.path.basename(output_path),
            "orig_size": orig_size,
            "new_size": new_size,
        }
    except Exception as e:
        return {"ok": False, "name": name, "error": str(e)}


def tsprint(*args, **kwargs):
    """线程安全的 print。"""
    with _print_lock:
        print(*args, **kwargs)


def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("--max-pixels", type=int, default=1024)
    p.add_argument("--quality", type=int, default=80)
    p.add_argument("--workers", type=int, default=0)
    p.add_argument("files", nargs="*")
    return p


def main(args):
    paths = []
    for path in args.files:
        if not os.path.isfile(path):
            print(f"⚠ 跳过（非文件）：{path}")
            continue
        ext = os.path.splitext(path)[1].lower()
        if ext not in SUPPORTED_EXTS:
            print(f"⚠ 跳过（非图片格式）：{os.path.basename(path)}")
            continue
        paths.append(path)

    if not paths:
        print("没有找到可处理的图片文件。")
        return 1

    workers = args.workers if args.workers > 0 else min(os.cpu_count() or 4, 8)
    total = len(paths)
    workers = min(workers, total)
    print(f"共 {total} 张图片，{workers} 线程并行处理"
          f"（输出 原名_{args.max_pixels}px.jpg，同名直接覆盖）\n")

    success = 0
    skipped = 0
    orig_total_kb = 0
    new_total_kb = 0
    done = 0

    with ThreadPoolExecutor(max_workers=workers) as executor:
        future_map = {
            executor.submit(process_one, p, args.max_pixels, args.quality): p
            for p in paths
        }

        for future in as_completed(future_map):
            result = future.result()
            done += 1

            if result["ok"]:
                name = result["name"]
                out_name = result["out_name"]
                orig_kb = result["orig_size"] / 1024
                new_kb = result["new_size"] / 1024
                saved_pct = (1 - new_kb / orig_kb) * 100 if orig_kb > 0 else 0

                tsprint(f"[{done}/{total}] ✓ {name} → {out_name}")
                tsprint(f"         {orig_kb:.0f} KB → {new_kb:.0f} KB"
                        f"（节省 {saved_pct:.0f}%）")

                orig_total_kb += result["orig_size"]
                new_total_kb += result["new_size"]
                success += 1
            else:
                tsprint(f"[{done}/{total}] ✗ {result['name']}: {result['error']}")
                skipped += 1

    print(f"\n{'='*50}")
    print(f"处理完成：成功 {success} 张", end="")
    if skipped > 0:
        print(f"，跳过 {skipped} 张", end="")
    if success > 1 and orig_total_kb > 0:
        total_saved = (1 - new_total_kb / orig_total_kb) * 100
        print(f"\n合计：{orig_total_kb/1024:.0f} KB → {new_total_kb/1024:.0f} KB"
              f"（节省 {total_saved:.0f}%）", end="")
    print()

    return 0 if skipped == 0 else 1


if __name__ == "__main__":
    sys.exit(main(build_parser().parse_args()))

"""
@name        Cross Split
@group       Image/Edit
@desc        以中心十字线将图片裁成四块（左上/右上/左下/右下），输出到源目录
@accepts     file
@ext         .png .jpg .jpeg .bmp .gif .tiff .tif .webp .ico .jfif
@multi       true
@requires    Pillow
"""

import argparse
import os
import sys

from PIL import Image

def split_image_by_center_cross(image_path):
    """将一张图片以中心十字线裁成四块，保存到源目录。"""
    img = Image.open(image_path)
    w, h = img.size

    cx, cy = w // 2, h // 2

    boxes = [
        (0, 0, cx, cy),       # 左上
        (cx, 0, w, cy),       # 右上
        (0, cy, cx, h),       # 左下
        (cx, cy, w, h),       # 右下
    ]

    base_dir = os.path.dirname(image_path)
    base_name, ext = os.path.splitext(os.path.basename(image_path))

    for i, box in enumerate(boxes, start=1):
        part_img = img.crop(box)
        new_name = f"{base_name}_part{i}{ext}"
        save_path = os.path.join(base_dir, new_name)
        part_img.save(save_path)
        print(f"Saved {save_path}")

    img.close()


def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("files", nargs="*")
    return p


def main(args):
    if not args.files:
        print("未提供任何图片文件。")
        return 1

    fail_count = 0
    for image_path in args.files:
        if os.path.isfile(image_path):
            try:
                split_image_by_center_cross(image_path)
            except Exception as e:
                print(f"处理 {image_path} 发生错误: {e}")
                fail_count += 1
        else:
            print(f"{image_path} 不是有效文件")
            fail_count += 1

    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main(build_parser().parse_args()))

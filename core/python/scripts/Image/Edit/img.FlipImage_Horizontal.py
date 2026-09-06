"""
@name        Flip Image Horizontal
@group       Image/Edit
@desc        水平翻转图片，输出为同目录下 *_flipped 后缀的新文件（含 EXR 支持）
@accepts     file
@ext         .png .jpg .jpeg .bmp .gif .tiff .tif .webp .ico .jfif .exr
@multi       true
@requires    Pillow OpenEXR Imath
"""

import argparse
import array
import os
import sys

def flip_image_horizontal(image_path):
    try:
        image_path = os.path.normpath(image_path)

        if not os.path.exists(image_path):
            print(f"文件不存在: {image_path}")
            return False

        _, ext = os.path.splitext(image_path)
        if ext.lower() == '.exr':
            return flip_exr_horizontal(image_path)
        else:
            return flip_standard_image(image_path)

    except Exception as e:
        print(f"✗ 处理失败: {image_path}")
        print(f"  错误详情: {type(e).__name__}: {e}")
        return False


def flip_standard_image(image_path):
    """处理常规图片格式（PNG, JPG, TIF等）"""
    from PIL import Image

    img = Image.open(image_path)
    flipped_img = img.transpose(Image.FLIP_LEFT_RIGHT)
    img.close()

    base_dir = os.path.dirname(image_path)
    base_name, ext = os.path.splitext(os.path.basename(image_path))
    new_name = f"{base_name}_flipped{ext}"
    save_path = os.path.join(base_dir, new_name)

    flipped_img.save(save_path)
    flipped_img.close()

    print(f"✓ 已水平翻转: {os.path.basename(image_path)} -> {new_name}")
    return True


def flip_exr_horizontal(image_path):
    """处理EXR格式"""
    import OpenEXR
    import Imath

    exr_file = OpenEXR.InputFile(image_path)

    header = exr_file.header()
    dw = header['dataWindow']
    width = dw.max.x - dw.min.x + 1
    height = dw.max.y - dw.min.y + 1

    channels = header['channels'].keys()

    channel_data = {}
    pt = Imath.PixelType(Imath.PixelType.FLOAT)

    for channel in channels:
        channel_str = exr_file.channel(channel, pt)
        channel_array = array.array('f', channel_str)

        flipped_array = array.array('f')
        for y in range(height):
            row_start = y * width
            row_end = row_start + width
            row = channel_array[row_start:row_end]
            row.reverse()
            flipped_array.extend(row)

        channel_data[channel] = flipped_array.tobytes()

    base_dir = os.path.dirname(image_path)
    base_name, ext = os.path.splitext(os.path.basename(image_path))
    new_name = f"{base_name}_flipped{ext}"
    save_path = os.path.join(base_dir, new_name)

    output_header = OpenEXR.Header(width, height)
    output_header['channels'] = header['channels']

    for key in header.keys():
        if key not in ['channels', 'dataWindow', 'displayWindow']:
            output_header[key] = header[key]

    output_file = OpenEXR.OutputFile(save_path, output_header)
    output_file.writePixels(channel_data)
    output_file.close()

    print(f"✓ 已水平翻转(EXR): {os.path.basename(image_path)} -> {new_name}")
    return True


def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("files", nargs="*")
    return p


def main(args):
    if not args.files:
        print("未提供任何图片文件。")
        return 1

    success_count = 0
    fail_count = 0

    for image_path in args.files:
        image_path = image_path.strip('"').strip("'")

        if os.path.isfile(image_path):
            if flip_image_horizontal(image_path):
                success_count += 1
            else:
                fail_count += 1
        else:
            print(f"✗ 不是有效文件: {image_path}")
            fail_count += 1

    print(f"\n处理完成: 成功 {success_count} 个，失败 {fail_count} 个")
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main(build_parser().parse_args()))

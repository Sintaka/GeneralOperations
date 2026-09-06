"""
@name        Zbrush UDIM Correction
@group       Image/Edit
@desc        水平翻转 EXR 内容并按行镜像重排 UDIM 编号，修正 Zbrush 导出方向
@accepts     file
@ext         .exr
@multi       true
@requires    OpenEXR Imath
@destructive 原地覆盖 EXR 像素内容并重命名文件（UDIM 编号变化），不可逆

@param  threads : int : 0 : 线程数(0=自动CPU数) : 0..64
"""

import argparse
import array
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from threading import Lock

import OpenEXR
import Imath

# Thread-safe print
print_lock = Lock()


def safe_print(message):
    """Thread-safe print"""
    with print_lock:
        print(message)


def extract_udim(filename):
    """Extract 4-digit UDIM from filename"""
    match = re.search(r'(?<=[^\d])\d{4}(?=[^\d]|$)', filename)
    return int(match.group()) if match else None


def calculate_mirrored_udim(old_udim, min_udim, all_udims):
    """
    Calculate mirrored UDIM with per-row horizontal flip
    UDIM layout: 10 tiles per row (1001-1010, 1011-1020, etc.)
    """
    index = old_udim - min_udim

    row = index // 10
    col = index % 10

    row_start_index = row * 10
    row_end_index = (row + 1) * 10
    tiles_in_row = len([u for u in all_udims if row_start_index <= (u - min_udim) < row_end_index])

    new_col = (tiles_in_row - 1) - col

    new_index = row * 10 + new_col

    new_udim = 1001 + new_index

    return new_udim


def flip_exr_horizontal(file_path):
    """Flip EXR file horizontally (overwrite original)"""
    try:
        exr_file = OpenEXR.InputFile(file_path)

        header = exr_file.header()
        dw = header['dataWindow']
        width = dw.max.x - dw.min.x + 1
        height = dw.max.y - dw.min.y + 1

        channels = list(header['channels'].keys())

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

        exr_file.close()

        temp_path = file_path + '.tmp.exr'

        output_header = OpenEXR.Header(width, height)
        output_header['channels'] = header['channels']

        for key in header.keys():
            if key not in ['channels', 'dataWindow', 'displayWindow']:
                output_header[key] = header[key]

        output_file = OpenEXR.OutputFile(temp_path, output_header)
        output_file.writePixels(channel_data)
        output_file.close()

        os.replace(temp_path, file_path)

        return True, os.path.basename(file_path)

    except Exception as e:
        return False, f"{os.path.basename(file_path)}: {type(e).__name__}: {e}"


def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("--threads", type=int, default=0)
    p.add_argument("files", nargs="*")
    return p


def main(args):
    files = args.files
    file_info = []

    for filepath in files:
        filepath = filepath.strip('"').strip("'")

        if not os.path.isfile(filepath):
            continue

        _, ext = os.path.splitext(filepath)
        if ext.lower() != '.exr':
            safe_print(f"Skipped (not EXR): {os.path.basename(filepath)}")
            continue

        dirname = os.path.dirname(filepath)
        basename = os.path.basename(filepath)
        name, ext = os.path.splitext(basename)

        udim = extract_udim(name)
        if udim:
            file_info.append({
                'path': filepath,
                'dir': dirname,
                'name': name,
                'ext': ext,
                'udim': udim,
            })
        else:
            safe_print(f"Skipped (no UDIM found): {basename}")

    if not file_info:
        print("No valid UDIM EXR files found")
        return 1

    threads = args.threads if args.threads > 0 else (os.cpu_count() or 4)

    print(f"\n=== Step 1: Flip {len(file_info)} EXR files horizontally (multi-threaded) ===")

    success_count = 0
    fail_count = 0

    with ThreadPoolExecutor(max_workers=threads) as executor:
        futures = {executor.submit(flip_exr_horizontal, info['path']): info for info in file_info}

        for future in as_completed(futures):
            success, result = future.result()
            if success:
                safe_print(f"✓ Flipped: {result}")
                success_count += 1
            else:
                safe_print(f"✗ Failed: {result}")
                fail_count += 1

    print(f"\nFlip result: {success_count} succeeded, {fail_count} failed")

    if fail_count > 0:
        print("\nSome files failed to flip. Aborting rename step.")
        return 1

    min_udim = min(info['udim'] for info in file_info)
    all_udims = sorted([info['udim'] for info in file_info])

    print(f"\n=== Step 2: Rename UDIM with per-row mirroring ===")
    print(f"Input range: {min_udim} - {max(all_udims)} ({len(file_info)} tiles)")

    udim_map = {}
    for udim in all_udims:
        new_udim = calculate_mirrored_udim(udim, min_udim, all_udims)
        udim_map[udim] = new_udim

    print("\nUDIM mapping:")
    for old_udim in sorted(udim_map.keys()):
        print(f"  {old_udim} → {udim_map[old_udim]}")

    # Sort by new UDIM descending to avoid overwrite conflicts
    file_info.sort(key=lambda x: udim_map[x['udim']], reverse=True)

    print("\n--- Pass 1: Rename to temporary names ---")
    for i, info in enumerate(file_info):
        temp_name = f"_temp_{i:04d}_{info['name']}{info['ext']}"
        temp_path = os.path.join(info['dir'], temp_name)
        os.rename(info['path'], temp_path)
        info['temp_path'] = temp_path
        print(f"{info['name']}{info['ext']} -> {temp_name}")

    print("\n--- Pass 2: Rename to final UDIM names ---")
    for info in file_info:
        old_udim = info['udim']
        new_udim = udim_map[old_udim]

        new_name = re.sub(r'(?<=[^\d])\d{4}(?=[^\d]|$)', str(new_udim).zfill(4), info['name'], count=1)
        new_path = os.path.join(info['dir'], new_name + info['ext'])

        os.rename(info['temp_path'], new_path)
        print(f"{old_udim} → {new_udim}: {new_name}{info['ext']}")

    print(f"\nAll done! Processed {len(file_info)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main(build_parser().parse_args()))

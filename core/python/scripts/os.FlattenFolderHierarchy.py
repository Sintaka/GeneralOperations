"""
@name        Flatten Folder Hierarchy
@group       System
@desc        将指定层级内的子文件夹内容展平到根目录，并删除展平后为空的子文件夹
@accepts     dir
@multi       false
@destructive 会移动文件/文件夹并删除已清空的子文件夹，不可逆

@param  levels : int : 1 : 展平层级深度(1=直接子文件夹,2=子文件夹+孙文件夹) : 1..64
"""

import argparse
import os
import shutil
import sys
from pathlib import Path


def flatten_subfolders(root_path, levels):
    """
    将指定层级的子文件夹内容展平到根目录，然后删除这些子文件夹

    参数:
        root_path: 起始目录路径
        levels: 要展平的层级深度（1表示直接子文件夹，2表示子文件夹和孙文件夹）
    """
    root = Path(root_path)

    if not root.exists():
        print(f"错误: 路径 {root_path} 不存在")
        return 1

    if not root.is_dir():
        print(f"错误: {root_path} 不是一个文件夹")
        return 1

    # 收集需要展平的文件夹
    folders_to_flatten = []

    def collect_folders(current_path, current_level):
        if current_level > levels:
            return

        try:
            for item in current_path.iterdir():
                if item.is_dir():
                    folders_to_flatten.append((item, current_level))
                    if current_level < levels:
                        collect_folders(item, current_level + 1)
        except PermissionError:
            print(f"警告: 无权限访问 {current_path}")

    collect_folders(root, 1)

    if not folders_to_flatten:
        print(f"没有找到需要展平的文件夹")
        return 0

    print(f"找到 {len(folders_to_flatten)} 个文件夹需要展平:")
    for folder, level in folders_to_flatten:
        print(f"  [层级 {level}] {folder}")

    # 按层级从深到浅排序，先处理深层文件夹
    folders_to_flatten.sort(key=lambda x: x[1], reverse=True)

    moved_count = 0
    error_count = 0

    # 处理每个文件夹
    for folder, level in folders_to_flatten:
        if not folder.exists():
            continue

        try:
            # 移动文件夹内的所有内容到根目录
            for item in folder.iterdir():
                target_path = root / item.name

                # 处理重名情况
                if target_path.exists():
                    base_name = item.stem if item.is_file() else item.name
                    extension = item.suffix if item.is_file() else ""
                    counter = 1

                    while target_path.exists():
                        if item.is_file():
                            new_name = f"{base_name}_{counter}{extension}"
                        else:
                            new_name = f"{base_name}_{counter}"
                        target_path = root / new_name
                        counter += 1

                # 移动文件或文件夹
                shutil.move(str(item), str(target_path))
                moved_count += 1
                print(f"已移动: {item.name} -> {target_path}")

        except Exception as e:
            print(f"错误: 处理 {folder} 时出错: {e}")
            error_count += 1

    # 删除现在为空的文件夹
    # 按层级从深到浅排序，先删除深层文件夹
    folders_to_delete = sorted(
        [f for f, _ in folders_to_flatten if f.exists()],
        key=lambda x: len(x.parts),
        reverse=True,
    )

    deleted_count = 0
    for folder in folders_to_delete:
        try:
            if folder.exists() and folder.is_dir():
                # 检查是否为空
                if not any(folder.iterdir()):
                    folder.rmdir()
                    deleted_count += 1
                    print(f"已删除空文件夹: {folder}")
                else:
                    print(f"警告: {folder} 不为空，未删除")
        except Exception as e:
            print(f"错误: 删除 {folder} 时出错: {e}")
            error_count += 1

    print(f"\n完成! 移动了 {moved_count} 个项目，删除了 {deleted_count} 个文件夹")
    return 0 if error_count == 0 else 1


def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("--levels", type=int, default=1)
    p.add_argument("files", nargs="*")
    return p


def main(args):
    if not args.files:
        print("未提供任何文件夹路径。")
        return 1

    if args.levels < 1:
        print("错误: 层级深度必须大于等于1")
        return 1

    exit_code = 0
    for path in args.files:
        path = path.strip('"').strip("'")
        result = flatten_subfolders(path, args.levels)
        if result != 0:
            exit_code = result

    return exit_code


if __name__ == "__main__":
    sys.exit(main(build_parser().parse_args()))

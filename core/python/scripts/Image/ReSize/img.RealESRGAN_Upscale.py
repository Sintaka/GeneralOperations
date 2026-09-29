"""
@name        Real-ESRGAN Upscale
@group       Image/ReSize
@desc        优先使用随包或本机缓存的 Real-ESRGAN，首次运行可联网缓存官方工具；不可用时用 Pillow LANCZOS + 适度锐化（非 AI 保底），输出到源目录：原名_x<倍率>.<原扩展名>，同名直接覆盖
@accepts     file
@ext         .png .jpg .jpeg .webp
@multi       true
@requires    Pillow

@param  scale : float : 2 : 放大倍率 : 1..4 : presets 1|2|3
"""

import argparse
import hashlib
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import uuid
import zipfile
from pathlib import Path
from pathlib import PurePosixPath

from PIL import Image, ImageFilter

# 可执行文件在发行包里的相对位置：<包根>/tools/realesrgan/realesrgan-ncnn-vulkan.exe
# 脚本本体在 <包根>/scripts/Image/ReSize/，从脚本目录向上最多找 5 层。
EXE_NAME = "realesrgan-ncnn-vulkan.exe"
MAX_UPLEVELS = 5
DOWNLOAD_URL = (
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
    "realesrgan-ncnn-vulkan-20220424-windows.zip"
)
DOWNLOAD_SHA256 = "ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D"
DOWNLOAD_TIMEOUT_SECONDS = 30

# 工具 -s 只原生支持整数 2/3/4（-s 1/1.5 会因缺 x1 模型报错退出码 127）。
# 其它倍率（含 1 倍"过一下"）先按 ceil 放大再等比缩回目标尺寸。
NATIVE_SCALES = (2, 3, 4)

# 工具实际支持的输入格式（实测：png/jpg/jpeg/webp 可用，bmp 会失败）
SUPPORTED_EXTS = {".png", ".jpg", ".jpeg", ".webp"}

# Pillow 保存时的格式映射（兜底缩放路径重新落盘用）
_SAVE_FMT = {".png": "PNG", ".jpg": "JPEG", ".jpeg": "JPEG", ".webp": "WEBP"}
_VULKAN_FAILURE = re.compile(r"\bvk[A-Za-z0-9_]*\s+failed(?:\s+-?\d+)?|VK_ERROR_DEVICE_LOST",
                             re.IGNORECASE)


def _find_bundled_tool():
    """查找旧发行包同级的工具，保持旧包的首选顺序。"""
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


def _runtime_root():
    """返回 runtime 根目录；GO_RUNTIME_HOME 可替代默认用户缓存根。"""
    override = os.environ.get("GO_RUNTIME_HOME")
    if override:
        return Path(override).expanduser()
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        return Path(local_app_data) / "GeneralOperations" / "runtime"
    return None


def _runtime_tool_dir():
    root = _runtime_root()
    return None if root is None else root / "tools" / "realesrgan"


def find_tool():
    """按兼容顺序查找旧包工具，再查用户 runtime 缓存。"""
    bundled = _find_bundled_tool()
    if bundled is not None:
        return bundled
    cache_dir = _runtime_tool_dir()
    if cache_dir is not None:
        cached = cache_dir / EXE_NAME
        if cached.is_file():
            return cached
    return None


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def _safe_extract(zip_path, destination):
    """安全解压 ZIP，拒绝绝对路径、路径穿越、ADS 和符号链接。"""
    root = Path(destination).resolve()
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            normalized = info.filename.replace("\\", "/")
            member = PurePosixPath(normalized)
            if (not normalized or member.is_absolute()
                    or any(part in ("..", "") for part in member.parts)
                    or any(":" in part for part in member.parts)):
                raise ValueError(f"ZIP 含不安全路径: {info.filename!r}")
            mode = (info.external_attr >> 16) & 0xFFFF
            if stat.S_ISLNK(mode):
                raise ValueError(f"ZIP 含符号链接: {info.filename!r}")
            target = (root / Path(*member.parts)).resolve()
            try:
                target.relative_to(root)
            except ValueError:
                raise ValueError(f"ZIP 路径越界: {info.filename!r}") from None

            if info.is_dir() or normalized.endswith("/"):
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info, "r") as source, open(target, "wb") as output:
                shutil.copyfileobj(source, output)


def _remove_path(path):
    if not path.exists() and not path.is_symlink():
        return
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path, ignore_errors=True)
    else:
        try:
            path.unlink()
        except OSError:
            pass


def _download_and_install():
    """下载固定官方包、校验并在 runtime 缓存目录中暂存后落位。"""
    target_dir = _runtime_tool_dir()
    if target_dir is None:
        raise RuntimeError("LOCALAPPDATA 未设置，且没有配置 GO_RUNTIME_HOME")
    parent = target_dir.parent
    parent.mkdir(parents=True, exist_ok=True)

    zip_fd, zip_name = tempfile.mkstemp(prefix=".realesrgan-download-", suffix=".zip",
                                        dir=str(parent))
    os.close(zip_fd)
    zip_path = Path(zip_name)
    stage_dir = None
    backup_dir = None
    installed = False
    try:
        stage_dir = Path(tempfile.mkdtemp(prefix=".realesrgan-stage-", dir=str(parent)))
        print("未找到 Real-ESRGAN 本地工具，正在从 GitHub 下载；下载失败会自动使用非 AI 保底。")
        with urllib.request.urlopen(DOWNLOAD_URL, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response:
            with open(zip_path, "wb") as output:
                downloaded = 0
                next_report = 4 * 1024 * 1024
                for chunk in iter(lambda: response.read(1024 * 1024), b""):
                    output.write(chunk)
                    downloaded += len(chunk)
                    if downloaded >= next_report:
                        print(f"Real-ESRGAN 已下载 {downloaded // (1024 * 1024)} MiB…")
                        next_report += 4 * 1024 * 1024
        print("Real-ESRGAN 下载完成，正在校验并安装工具。")
        actual_sha256 = _sha256(zip_path)
        if actual_sha256 != DOWNLOAD_SHA256:
            raise ValueError(f"Real-ESRGAN SHA256 不匹配（{actual_sha256}）")

        _safe_extract(zip_path, stage_dir)
        staged_exe = stage_dir / EXE_NAME
        if not staged_exe.is_file():
            raise ValueError(f"官方 ZIP 中找不到 {EXE_NAME}")

        # 目标通常不存在；若此前留下了不完整目录，先原子移开再发布完整目录。
        if target_dir.exists() or target_dir.is_symlink():
            backup_dir = parent / f".realesrgan-old-{uuid.uuid4().hex}"
            os.replace(target_dir, backup_dir)
        try:
            os.replace(stage_dir, target_dir)
            stage_dir = None
            installed = True
        except Exception:
            if backup_dir is not None and not target_dir.exists():
                try:
                    os.replace(backup_dir, target_dir)
                    backup_dir = None
                except OSError:
                    pass
            raise
        return target_dir / EXE_NAME
    finally:
        _remove_path(zip_path)
        if stage_dir is not None:
            _remove_path(stage_dir)
        if installed and backup_dir is not None:
            _remove_path(backup_dir)


def ensure_tool():
    """先复用现有工具，没有时下载；错误作为说明返回给非 AI 保底。"""
    try:
        exe = find_tool()
        if exe is not None:
            return exe, None
        return _download_and_install(), None
    except Exception as error:
        return None, str(error)


def plan_scale(scale):
    """把任意 1..4 倍率映射成 (工具倍率, 是否需要事后缩放)。"""
    if float(scale).is_integer() and int(scale) in NATIVE_SCALES:
        return int(scale), False
    tool_scale = min(4, max(2, math.ceil(scale)))
    return tool_scale, True


def scale_tag(scale):
    """倍率的文件名片段：2.0 -> '2'，1.5 -> '1.5'。"""
    return "%g" % scale


def _save_image(image, out_path, ext_l):
    fmt = _SAVE_FMT.get(ext_l)
    if fmt == "JPEG":
        if image.mode not in ("RGB", "L", "CMYK"):
            image = image.convert("RGB")
        image.save(out_path, fmt, quality=95)
    else:
        image.save(out_path, fmt)


def _pillow_fallback(path, out_path, scale, reason):
    """LANCZOS 放大并做适度锐化；返回明确标注的非 AI 保底结果。"""
    ext_l = os.path.splitext(path)[1].lower()
    temp_path = None
    try:
        with Image.open(path) as source:
            source.load()
            image = source.copy()
        if image.mode == "P":
            image = image.convert("RGBA" if "transparency" in image.info else "RGB")
        elif image.mode == "1":
            image = image.convert("L")
        elif image.mode not in ("RGB", "RGBA", "L", "LA", "CMYK"):
            image = image.convert("RGBA" if "A" in image.getbands() else "RGB")

        target = (max(1, round(image.width * scale)),
                  max(1, round(image.height * scale)))
        resized = image.resize(target, Image.LANCZOS)
        # 锐化颜色通道并保留原 alpha，避免锐化透明度造成边缘光晕。
        if resized.mode == "RGBA":
            alpha = resized.getchannel("A")
            sharpened = resized.convert("RGB").filter(
                ImageFilter.UnsharpMask(radius=1.0, percent=125, threshold=3)
            ).convert("RGBA")
            sharpened.putalpha(alpha)
        elif resized.mode == "LA":
            alpha = resized.getchannel("A")
            sharpened = resized.getchannel("L").filter(
                ImageFilter.UnsharpMask(radius=1.0, percent=125, threshold=3)
            ).convert("LA")
            sharpened.putalpha(alpha)
        else:
            sharpened = resized.filter(
                ImageFilter.UnsharpMask(radius=1.0, percent=125, threshold=3)
            )

        output_dir = Path(out_path).parent
        with tempfile.NamedTemporaryFile(prefix=".realesrgan-fallback-", suffix=ext_l,
                                         dir=str(output_dir), delete=False) as temp_file:
            temp_path = temp_file.name
        _save_image(sharpened, temp_path, ext_l)
        os.replace(temp_path, out_path)
        temp_path = None
        return True, (f"→ {os.path.basename(out_path)} ({target[0]}x{target[1]}), "
                      f"Pillow LANCZOS + UnsharpMask 非 AI 保底（{reason}）")
    except Exception as error:
        return False, f"Pillow 非 AI 保底失败（{reason}）：{error}"
    finally:
        if temp_path is not None:
            _remove_path(Path(temp_path))


def _run_tool(cmd):
    """逐行透传工具日志，并识别退出码为零时仍发生的 Vulkan 错误。"""
    # ncnn-vulkan can emit vkQueueSubmit/vkWaitForFences failed -4, write an
    # all-black PNG, and still exit 0. Exit status alone is not a success signal.
    failure = None
    with subprocess.Popen(cmd, stderr=subprocess.PIPE, text=True,
                          encoding="utf-8", errors="replace") as proc:
        for line in proc.stderr:
            sys.stderr.write(line)
            sys.stderr.flush()
            if failure is None and _VULKAN_FAILURE.search(line):
                failure = line.strip()
        return proc.wait(), failure


def process_one(exe, models_dir, path, scale):
    """处理单张图片。返回 (ok, 提示信息)。"""
    base, ext = os.path.splitext(path)
    ext_l = ext.lower()
    out_path = f"{base}_x{scale_tag(scale)}{ext_l}"
    if exe is None:
        return _pillow_fallback(path, out_path, scale, "Real-ESRGAN 工具不可用")

    tool_scale, need_resize = plan_scale(scale)

    cmd = [str(exe), "-i", path, "-o", out_path, "-s", str(tool_scale)]
    if models_dir is not None:
        cmd += ["-m", str(models_dir)]

    try:
        returncode, vulkan_error = _run_tool(cmd)
    except OSError as e:
        return _pillow_fallback(path, out_path, scale, f"Real-ESRGAN 无法启动：{e}")

    if returncode != 0 or vulkan_error:
        reason = (f"Vulkan 报错：{vulkan_error}" if vulkan_error else
                  f"工具退出码 {returncode}（-s {tool_scale}）")
        return _pillow_fallback(
            path, out_path, scale,
            f"Real-ESRGAN/Vulkan 执行失败，{reason}",
        )
    if not os.path.isfile(out_path):
        return _pillow_fallback(
            path, out_path, scale,
            f"Real-ESRGAN 未产出输出文件 {os.path.basename(out_path)}",
        )

    if need_resize:
        # 兜底：非原生倍率（含 1 倍）先被 ceil 倍放大，再等比缩回目标尺寸。
        # 目标尺寸按 源尺寸×scale 换算 —— 即 输出尺寸×(scale/tool_scale)。
        try:
            with Image.open(out_path) as img:
                w, h = img.size
                target = (max(1, round(w * scale / tool_scale)),
                          max(1, round(h * scale / tool_scale)))
                resized = img.resize(target, Image.LANCZOS)
                _save_image(resized, out_path, ext_l)
        except Exception as e:
            return _pillow_fallback(path, out_path, scale, f"AI 输出后缩放失败：{e}")

    try:
        with Image.open(out_path) as img:
            out_size = img.size
            output_has_content = img.convert("RGB").getbbox() is not None
        if not output_has_content:
            with Image.open(path) as source:
                if source.convert("RGB").getbbox() is not None:
                    return _pillow_fallback(path, out_path, scale,
                                            "Real-ESRGAN 输出全黑，但原图含非黑像素")
    except Exception as e:
        return _pillow_fallback(path, out_path, scale, f"Real-ESRGAN 输出无效：{e}")
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

    print("正在检查 Real-ESRGAN 本地工具缓存…")
    exe, tool_error = ensure_tool()
    if exe is None:
        print(f"⚠ Real-ESRGAN 不可用（{tool_error or '工具缺失'}）；"
              "图片将使用 Pillow LANCZOS + UnsharpMask 非 AI 保底。")

    models_dir = None
    if exe is not None:
        models_dir = exe.parent / "models"
        models_dir = models_dir if models_dir.is_dir() else None

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

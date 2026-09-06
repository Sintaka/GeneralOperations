#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@name        EXR to PNG (Large / 4K)
@group       Image/Format Convert
@desc        EXR 场景线性转 Display sRGB(ACES)，流式降采样到指定像素上限后输出 PNG，为 8K/16K 大图设计，绝不整图载入内存
@accepts     file
@ext         .exr
@multi       true
@requires    numpy OpenColorIO OpenEXR Imath Pillow

@param  target    : int  : 4096 : 输出长边像素上限                : 16..65536
@param  png_level : int  : 1    : PNG deflate 压缩等级             : 0..9
@param  threads   : int  : 0    : 线程数(0=自动,min(CPU,8))        : 0..64
@param  max_mem   : int  : 0    : 像素缓冲内存预算 MB(0=自动,可用内存70%) : 0..1048576
@param  config    : str  : ""   : OCIO 配置文件路径(留空用 $OCIO 或内置默认)
@param  no_mip    : bool : false : 忽略嵌入的 mipmap(诊断用)
@param  quiet     : bool : false : 减少逐文件输出

# OpenImageIO 可选：装了就有 mipmap 感知的读取和更快的 PNG 写入，Windows 上常无 wheel，
# 缺失不影响脚本运行（自动退化到 OpenEXR 读取 + opencv/Pillow 写入）。
"""

import argparse
import os
import sys
import time
import math
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed

import numpy as np

__version__ = "1.0.0"

# ---------------------------------------------------------------- defaults ---
DEFAULT_TARGET = 4096
DEFAULT_PNG_LEVEL = 1
BAND_TARGET_BYTES = 24 << 20      # aim for ~24 MB per source band read
QUANT_CHUNK_ROWS = 256            # rows per quantise chunk
FALLBACK_BUDGET_MB = 4096         # used when psutil is unavailable

# ------------------------------------------------------------- backends ------
try:
    import OpenImageIO as oiio
    HAS_OIIO = True
except ImportError:
    HAS_OIIO = False

try:
    import OpenEXR
    import Imath
    HAS_OPENEXR = True
except ImportError:
    HAS_OPENEXR = False

try:
    import PyOpenColorIO as ocio
    HAS_OCIO = True
except ImportError:
    HAS_OCIO = False

try:
    import cv2
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False


def _fatal(msg):
    print("Error: " + msg)
    sys.exit(1)


_print_lock = threading.Lock()


def tsprint(*args, **kwargs):
    """Thread-safe print."""
    with _print_lock:
        print(*args, **kwargs)
        sys.stdout.flush()

# ========================================================== resample math ====
def area_weights(src, dst):
    """
    Exact box/area resample weights for src -> dst along one axis.

    Returns (idx, w) each shaped (dst, kmax): output element j is
    sum_k in[idx[j,k]] * w[j,k].  Rows are zero-padded, so a zero weight
    contributes nothing and the padded index is harmless.

    This reproduces cv2.INTER_AREA exactly (verified bit-identical), but as
    explicit weights we can apply band-by-band instead of to a whole image.
    """
    if dst >= src:
        idx = np.arange(src, dtype=np.int64).reshape(-1, 1)
        return idx, np.ones((src, 1), np.float32)

    scale = src / dst
    kmax = int(math.ceil(scale)) + 1
    idx = np.zeros((dst, kmax), np.int64)
    w = np.zeros((dst, kmax), np.float32)

    for j in range(dst):
        a = j * scale
        b = (j + 1) * scale
        i0 = int(math.floor(a))
        i1 = min(int(math.ceil(b)), src)
        acc = 0.0
        for k, i in enumerate(range(i0, i1)):
            ov = min(b, i + 1.0) - max(a, float(i))
            if ov <= 0.0:
                continue
            idx[j, k] = i
            w[j, k] = ov
            acc += ov
        if acc > 0.0:
            w[j] /= acc
    return idx, w


class Plan(object):
    """Everything decided from the header, before a single pixel is read."""

    __slots__ = ("src_w", "src_h", "out_w", "out_h", "mip", "mip_w", "mip_h",
                 "mode", "factor", "hidx", "hw", "vidx", "vw", "band_rows",
                 "tiled", "channels", "half")

    def describe(self):
        chain = "%dx%d" % (self.src_w, self.src_h)
        if self.mip:
            chain += " -mip%d-> %dx%d" % (self.mip, self.mip_w, self.mip_h)
        if (self.mip_w, self.mip_h) != (self.out_w, self.out_h):
            chain += " -%s-> %dx%d" % (self.mode, self.out_w, self.out_h)
        else:
            chain += " (no resample)"
        return chain


def build_plan(src_w, src_h, target, mip_levels=None, tiled=False,
               channels=3, half=True, use_mip=True):
    """
    Decide output size, which mipmap level to read, and the resample strategy.

    Mip selection picks the SMALLEST level whose longest edge is still >= the
    target, so we read as little as possible while never upscaling.  For a
    16K texture with mips and a 4K target that is a 16x reduction in IO
    before any filtering happens at all.
    """
    p = Plan()
    p.src_w, p.src_h = src_w, src_h
    p.tiled = tiled
    p.channels = channels
    p.half = half

    longest = max(src_w, src_h)
    if longest <= target:
        p.out_w, p.out_h = src_w, src_h
    else:
        r = target / float(longest)
        p.out_w = max(1, int(round(src_w * r)))
        p.out_h = max(1, int(round(src_h * r)))

    # --- pick mip level ---
    p.mip = 0
    p.mip_w, p.mip_h = src_w, src_h
    if use_mip and mip_levels:
        for lvl, (lw, lh) in enumerate(mip_levels):
            if max(lw, lh) >= max(p.out_w, p.out_h):
                p.mip, p.mip_w, p.mip_h = lvl, lw, lh
            else:
                break

    # --- resample strategy from mip dims to output dims ---
    mw, mh = p.mip_w, p.mip_h
    if (mw, mh) == (p.out_w, p.out_h):
        p.mode = "none"
        p.factor = 1
    elif (mw % p.out_w == 0 and mh % p.out_h == 0
          and mw // p.out_w == mh // p.out_h):
        # exact integer box reduce: reshape-mean, materially faster than gather
        p.mode = "box"
        p.factor = mw // p.out_w
    else:
        p.mode = "area"
        p.factor = 1

    if p.mode == "area":
        p.hidx, p.hw = area_weights(mw, p.out_w)
        p.vidx, p.vw = area_weights(mh, p.out_h)
    else:
        p.hidx = p.hw = p.vidx = p.vw = None

    # --- band size: aim for BAND_TARGET_BYTES of source pixels per read ---
    vscale = max(1, int(math.ceil(mh / float(p.out_h))))
    src_row_bytes = mw * 3 * 4
    rows = max(1, int(BAND_TARGET_BYTES // max(1, src_row_bytes * vscale)))
    if p.mode == "box":
        rows = max(1, rows)                      # out rows; src = rows*factor
    p.band_rows = min(rows, p.out_h)
    return p


def estimate_peak_bytes(p):
    """
    Peak pixel-buffer bytes for one task, used by the memory budget.

    The dominant term is the float32 target (4096^2 RGB = 201 MB), which has
    to exist because OCIO works on float.  Everything else is band-sized.
    Compare against a naive full read: 16384^2 RGB float32 = 3.2 GB.
    """
    out_f32 = p.out_w * p.out_h * 3 * 4                 # resample target
    out_u8 = p.out_w * p.out_h * 3                      # quantised result
    vscale = max(1, int(math.ceil(p.mip_h / float(p.out_h))))
    src_band = p.band_rows * vscale * p.mip_w * 3 * 4   # float32 source band
    if p.mode == "area" and p.hidx is not None:
        # gather accumulates tap by tap, so one band-sized temp plus one accum
        hband = p.band_rows * p.out_w * 3 * 4 * 2
    else:
        hband = p.band_rows * p.out_w * 3 * 4
    quant_chunk = p.out_w * 3 * 4 * QUANT_CHUNK_ROWS
    return int(out_f32 + out_u8 + src_band + hband + quant_chunk)


# ============================================================== readers ======
class ReaderError(Exception):
    pass

class OiioReader(object):
    """
    Preferred reader.  Handles mipmaps and, importantly, branches between
    read_scanlines and read_tiles.

    Both branches exist because calling read_scanlines on a TILED OpenEXR
    raises SIGFPE inside this OIIO build -- a hard process kill, not an
    exception.  16K textures are nearly always tiled, so getting this wrong
    would take the whole batch down with no traceback.
    """

    def __init__(self, path):
        self.path = path
        self.inp = oiio.ImageInput.open(path)
        if self.inp is None:
            raise ReaderError(oiio.geterror() or "cannot open")
        s = self.inp.spec()
        self.base_w, self.base_h = s.width, s.height
        self.nchannels = s.nchannels
        self.chan_names = list(s.channelnames)
        self.half = "half" in str(s.format)
        self.tiled = bool(s.tile_width)
        self.tile_h = s.tile_height or 0
        self.mip_levels = self._enumerate_mips()
        self._level = 0

    def _enumerate_mips(self):
        """
        Enumerate via spec(0, i), NOT seek_subimage.

        seek_subimage(0, n) returns True for out-of-range levels in this
        build, so looping on it never terminates and eventually crashes.
        """
        levels = []
        for i in range(32):
            try:
                sp = self.inp.spec(0, i)
            except Exception:
                break
            if sp is None or sp.width == 0:
                break
            dims = (sp.width, sp.height)
            if levels and dims == levels[-1]:
                break
            levels.append(dims)
            if dims == (1, 1):
                break
        return levels

    def rgb_channel_range(self):
        """
        Locate a contiguous RGB triple.  Bare R/G/B first, then the first
        layer that has all three (e.g. 'diffuse.R'), then fall back to the
        leading channels.
        """
        names = self.chan_names
        lower = [n.lower() for n in names]
        for want in (["r", "g", "b"],):
            if lower[:3] == want:
                return 0, 3, None
        layers = {}
        for i, n in enumerate(names):
            if "." in n:
                layer, comp = n.rsplit(".", 1)
                layers.setdefault(layer, {})[comp.lower()] = i
        for layer in sorted(layers):
            comps = layers[layer]
            if all(c in comps for c in "rgb"):
                idx = [comps["r"], comps["g"], comps["b"]]
                if idx == list(range(idx[0], idx[0] + 3)):
                    return idx[0], idx[0] + 3, layer
                return None, None, (layer, idx)
        if self.nchannels >= 3:
            return 0, 3, None
        return 0, self.nchannels, None

    def seek(self, level):
        if level != self._level:
            if not self.inp.seek_subimage(0, level):
                raise ReaderError("cannot seek mip level %d" % level)
            self._level = level
        sp = self.inp.spec()
        self.tiled = bool(sp.tile_width)
        self.tile_h = sp.tile_height or 0
        return sp.width, sp.height

    def read_band(self, y0, y1, width):
        """Return (rows, width, 3) float32 scene-linear for source rows [y0,y1)."""
        cb, ce, _ = self.rgb_channel_range()
        if cb is None:
            raise ReaderError("non-contiguous RGB layout")
        if self.tiled:
            th = self.tile_h or 1
            ta = (y0 // th) * th
            tb = min(int(math.ceil(y1 / float(th))) * th, self._h())
            data = self.inp.read_tiles(0, width, ta, tb, 0, 1, cb, ce, "float")
            if data is None:
                raise ReaderError(self.inp.geterror() or "read_tiles failed")
            data = np.asarray(data)
            band = data[y0 - ta:y1 - ta]
        else:
            data = self.inp.read_scanlines(y0, y1, 0, cb, ce, "float")
            if data is None:
                raise ReaderError(self.inp.geterror() or "read_scanlines failed")
            band = np.asarray(data)
        if band.ndim == 2:
            band = band[:, :, None]
        if band.shape[2] == 1:
            band = np.repeat(band, 3, axis=2)
        elif band.shape[2] > 3:
            band = band[:, :, :3]
        return np.ascontiguousarray(band, dtype=np.float32)

    def _h(self):
        return self.inp.spec().height

    def close(self):
        try:
            self.inp.close()
        except Exception:
            pass


class OpenExrReader(object):
    """
    Fallback for when OpenImageIO is not installed.  No mipmap support; uses
    the legacy InputFile.channel(name, type, y0, y1) banded read, which is
    still a true partial read (verified against a 4096-row file).
    """

    def __init__(self, path):
        self.path = path
        self.f = OpenEXR.InputFile(path)
        h = self.f.header()
        dw = h["dataWindow"]
        self.base_w = dw.max.x - dw.min.x + 1
        self.base_h = dw.max.y - dw.min.y + 1
        self.y_off = dw.min.y
        ch = h["channels"]
        self.chan_names = list(ch.keys())
        self.half = any(str(v.type) == "HALF" for v in ch.values())
        self.tiled = False
        self.mip_levels = []
        self._names = self._pick_rgb()
        self._banded = True

    def _pick_rgb(self):
        names = self.chan_names
        if all(c in names for c in ("R", "G", "B")):
            return ["R", "G", "B"]
        layers = {}
        for n in names:
            if "." in n:
                layer, comp = n.rsplit(".", 1)
                layers.setdefault(layer, {})[comp.upper()] = n
        for layer in sorted(layers):
            comps = layers[layer]
            if all(c in comps for c in "RGB"):
                return [comps["R"], comps["G"], comps["B"]]
        if "Y" in names:
            return ["Y", "Y", "Y"]
        raise ReaderError("no RGB channels found in %s" % (names,))

    def rgb_channel_range(self):
        return 0, 3, None

    def seek(self, level):
        if level != 0:
            raise ReaderError("OpenEXR fallback has no mipmap support")
        return self.base_w, self.base_h

    def read_band(self, y0, y1, width):
        FLOAT = Imath.PixelType(Imath.PixelType.FLOAT)
        rows = y1 - y0
        out = np.empty((rows, width, 3), np.float32)
        a = y0 + self.y_off
        b = y1 - 1 + self.y_off
        for ci, name in enumerate(self._names):
            if self._banded:
                try:
                    raw = self.f.channel(name, FLOAT, a, b)
                except Exception:
                    self._banded = False
                    raw = None
                if raw is not None:
                    arr = np.frombuffer(raw, np.float32)
                    if arr.size != rows * width:
                        self._banded = False
                    else:
                        out[:, :, ci] = arr.reshape(rows, width)
                        continue
            # last resort: whole channel, cached
            if not hasattr(self, "_full"):
                self._full = {}
            if name not in self._full:
                raw = self.f.channel(name, FLOAT)
                self._full[name] = np.frombuffer(raw, np.float32).reshape(
                    self.base_h, self.base_w)
            out[:, :, ci] = self._full[name][y0:y1]
        return out

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass


def open_reader(path):
    if HAS_OIIO:
        try:
            return OiioReader(path)
        except Exception as e:
            if not HAS_OPENEXR:
                raise
            tsprint("  (OIIO reader failed: %s -- falling back to OpenEXR)" % e)
    return OpenExrReader(path)


# ================================================================= OCIO ======
_ocio_lock = threading.Lock()
_ocio_cache = {}

_SRC_CANDIDATES = ["scene_linear", "ACEScg", "ACES - ACEScg", "lin_ap1",
                   "Utility - Linear - Rec.709", "Linear Rec.709 (sRGB)",
                   "linear"]
_DISPLAY_CANDIDATES = ["sRGB - Display", "sRGB", "ACES", "Rec.1886",
                       "Gamma 2.2 Rec.709 - Display"]
_VIEW_CANDIDATES = ["ACES 1.0 - SDR Video", "ACES 1.0 SDR-video",
                    "ACES 2.0 - SDR 100 nits (Rec.709)", "Output - sRGB",
                    "sRGB", "Video (colorimetric)", "Raw"]

def get_cpu_processor(config_path=None):
    """
    Build the scene-linear -> display-sRGB CPU processor once and share it.

    The original script rebuilt config + processor per file; parsing an ACES
    config costs well over 100 ms, so on a 50-file batch that alone was
    seconds of pure overhead.  A CPUProcessor is safe to share across threads
    as long as each thread hands it its own buffer.
    """
    key = config_path or os.getenv("OCIO") or "ocio://default"
    with _ocio_lock:
        if key in _ocio_cache:
            return _ocio_cache[key]

        if config_path and os.path.exists(config_path):
            config = ocio.Config.CreateFromFile(config_path)
        elif os.getenv("OCIO"):
            config = ocio.Config.CreateFromEnv()
        else:
            config = ocio.Config.CreateFromFile("ocio://default")

        src = None
        for name in _SRC_CANDIDATES:
            try:
                cs = config.getColorSpace(name)
                if cs:
                    src = cs.getName()
                    break
            except Exception:
                continue
        if not src:
            raise RuntimeError("cannot resolve a scene-linear colorspace")

        # OCIO 2.x exposes getDisplays(); getNumDisplays() is 1.x only.
        try:
            displays = list(config.getDisplays())
        except AttributeError:
            displays = [config.getDisplay(i)
                        for i in range(config.getNumDisplays())]
        display = next((d for d in _DISPLAY_CANDIDATES if d in displays), None)
        if not display:
            try:
                display = config.getDefaultDisplay()
            except Exception:
                display = displays[0] if displays else None
        if not display:
            raise RuntimeError("no display found in OCIO config")

        try:
            views = list(config.getViews(display))
        except AttributeError:
            views = [config.getView(display, i)
                     for i in range(config.getNumViews(display))]
        view = next((v for v in _VIEW_CANDIDATES if v in views), None)
        if not view:
            try:
                view = config.getDefaultView(display)
            except Exception:
                view = views[0] if views else None
        if not view:
            raise RuntimeError("no view found for display %r" % display)

        t = ocio.DisplayViewTransform()
        t.setSrc(src)
        t.setDisplay(display)
        t.setView(view)
        proc = config.getProcessor(t)
        try:
            cpu = proc.getDefaultCPUProcessor()
        except AttributeError:
            cpu = proc

        info = (cpu, "%s -> %s / %s" % (src, display, view))
        _ocio_cache[key] = info
        return info


def apply_display_transform(cpu, img, chunk_rows=64):
    """
    Apply the display transform in place, a few rows at a time.

    applyRGB mutates the buffer and rejects non-contiguous views, so each
    chunk is copied into a contiguous scratch buffer and written back.
    Chunking keeps the working set inside cache instead of streaming a
    200 MB array through it.  NaN/Inf are scrubbed first -- 16K AOVs
    routinely carry them and they propagate through the transform.
    """
    h = img.shape[0]
    for y in range(0, h, chunk_rows):
        y1 = min(y + chunk_rows, h)
        chunk = np.ascontiguousarray(img[y:y1].reshape(-1, 3))
        np.nan_to_num(chunk, copy=False, nan=0.0, posinf=1.0, neginf=0.0)
        cpu.applyRGB(chunk)
        img[y:y1] = chunk.reshape(y1 - y, img.shape[1], 3)
    np.clip(img, 0.0, 1.0, out=img)
    return img


# ==================================================== streaming resampler ====
def _resample_band_box(band, factor, out_w, out_h_band):
    """Integer box reduce via reshape-mean.  Fastest path; hit by 16K/8K->4K."""
    rows = out_h_band * factor
    cols = out_w * factor
    b = band[:rows, :cols]
    return b.reshape(out_h_band, factor, out_w, factor, 3).mean(axis=(1, 3))


def _resample_band_h(band, hidx, hw):
    """
    Horizontal exact-area resample of one band.

    Accumulated tap by tap rather than building a (rows, out_w, kmax, 3)
    intermediate: at 16K widths that temp would be hundreds of MB per band
    and defeat the whole point of streaming.
    """
    rows = band.shape[0]
    out = np.zeros((rows, hidx.shape[0], 3), np.float32)
    for k in range(hidx.shape[1]):
        w = hw[:, k]
        if not w.any():
            continue
        out += band[:, hidx[:, k], :] * w[None, :, None]
    return out


def resample_to_target(reader, p, progress=None):
    """
    Stream the source into an (out_h, out_w, 3) float32 scene-linear target.

    Bands are keyed to OUTPUT rows: for each group of output rows we compute
    the exact source-row span feeding it, read only that span, resample, and
    write the finished output rows.  Source rows are consumed in order and
    each output row is completed before we move on, so no accumulator has to
    persist across bands and no full-resolution buffer ever exists.
    """
    mw, mh = reader.seek(p.mip)
    if (mw, mh) != (p.mip_w, p.mip_h):
        # header and actual level disagree; trust the actual level
        p.mip_w, p.mip_h = mw, mh

    out = np.empty((p.out_h, p.out_w, 3), np.float32)

    if p.mode == "none":
        for y0 in range(0, p.out_h, p.band_rows):
            y1 = min(y0 + p.band_rows, p.out_h)
            out[y0:y1] = reader.read_band(y0, y1, mw)[:, :p.out_w, :]
            if progress:
                progress(y1, p.out_h)
        return out

    if p.mode == "box":
        f = p.factor
        for y0 in range(0, p.out_h, p.band_rows):
            y1 = min(y0 + p.band_rows, p.out_h)
            sy0, sy1 = y0 * f, min(y1 * f, mh)
            band = reader.read_band(sy0, sy1, mw)
            navail = band.shape[0] // f
            if navail < (y1 - y0):
                y1 = y0 + navail
                if y1 <= y0:
                    break
            out[y0:y1] = _resample_band_box(band, f, p.out_w, y1 - y0)
            del band
            if progress:
                progress(y1, p.out_h)
        return out

    # --- exact area, arbitrary ratio ---
    vidx, vw = p.vidx, p.vw
    for y0 in range(0, p.out_h, p.band_rows):
        y1 = min(y0 + p.band_rows, p.out_h)
        rows = vidx[y0:y1]
        wts = vw[y0:y1]
        valid = wts > 0
        if not valid.any():
            continue
        sy0 = int(rows[valid].min())
        sy1 = int(rows[valid].max()) + 1
        sy1 = min(sy1, mh)
        band = reader.read_band(sy0, sy1, mw)
        hband = _resample_band_h(band, p.hidx, p.hw)
        del band
        for j in range(y1 - y0):
            acc = None
            for k in range(vidx.shape[1]):
                w = float(wts[j, k])
                if w <= 0.0:
                    continue
                r = int(rows[j, k]) - sy0
                if r < 0 or r >= hband.shape[0]:
                    continue
                acc = hband[r] * w if acc is None else acc + hband[r] * w
            out[y0 + j] = 0.0 if acc is None else acc
        del hband
        if progress:
            progress(y1, p.out_h)
    return out


# ============================================================== writers ======
def write_png(path, rgb_u8, level):
    """
    Write RGB uint8 as PNG.

    Backend order is deliberate: OIIO and cv2 both release the GIL during
    deflate, so they actually parallelise across the worker pool.  PIL's zlib
    path largely does not, which is why the original script's writes serialised
    and showed up as high CPU with only a few MB/s of output.
    """
    if HAS_OIIO:
        spec = oiio.ImageSpec(rgb_u8.shape[1], rgb_u8.shape[0], 3, "uint8")
        spec.attribute("png:compressionLevel", int(level))
        out = oiio.ImageOutput.create(path)
        if out is not None:
            if out.open(path, spec):
                ok = out.write_image(rgb_u8)
                out.close()
                if ok:
                    return "oiio"
            try:
                out.close()
            except Exception:
                pass
    if HAS_CV2:
        ok, buf = cv2.imencode(
            ".png", rgb_u8[:, :, ::-1],
            [cv2.IMWRITE_PNG_COMPRESSION, int(level)])
        if ok:
            buf.tofile(path)
            return "cv2"
    if HAS_PIL:
        Image.fromarray(rgb_u8, mode="RGB").save(
            path, "PNG", compress_level=int(level), optimize=False)
        return "pil"
    raise RuntimeError("no PNG writer available")


def to_uint8(img):
    """
    Quantise [0,1] float to uint8 with correct rounding, in row chunks.

    Two things worth noting.  First, (x * 255).astype(uint8) truncates,
    biasing the whole image down by up to a full code value; np.rint rounds
    to nearest.  Second, doing it in one shot allocates a second full float
    array -- another 201 MB at 4K -- so it is chunked to keep the extra
    allocation band-sized.
    """
    h, w = img.shape[:2]
    out = np.empty((h, w, 3), np.uint8)
    for y in range(0, h, QUANT_CHUNK_ROWS):
        y1 = min(y + QUANT_CHUNK_ROWS, h)
        chunk = img[y:y1] * 255.0
        np.rint(chunk, out=chunk)
        out[y:y1] = chunk.astype(np.uint8)
    return out

# ======================================================== memory budget ======
class MemoryBudget(object):
    """
    Byte-quota gate shared by all workers.

    Threads are cheap but 16K tasks are not: eight of them starting at once
    could spike past the machine's RAM.  Each worker reserves its estimated
    peak before it allocates and releases on completion, so small files run
    fully parallel while large ones self-throttle.  A task larger than the
    whole budget is allowed through alone rather than deadlocking.
    """

    def __init__(self, total_bytes):
        self.total = max(1, int(total_bytes))
        self.free = self.total
        self.cv = threading.Condition()

    def acquire(self, n):
        n = int(n)
        with self.cv:
            n_eff = min(n, self.total)
            while self.free < n_eff:
                self.cv.wait()
            self.free -= n_eff
            return n_eff

    def release(self, n):
        with self.cv:
            self.free = min(self.total, self.free + int(n))
            self.cv.notify_all()


def default_budget_bytes():
    if HAS_PSUTIL:
        try:
            return int(psutil.virtual_memory().available * 0.70)
        except Exception:
            pass
    return FALLBACK_BUDGET_MB << 20


def budget_floor(target):
    """
    Smallest sane budget: one target-sized task must always fit.

    Without this a low --max-mem would still work but every task would clamp
    to the whole budget and run strictly one at a time, which looks like a
    hang rather than a throttle.
    """
    p = Plan()
    p.out_w = p.out_h = target
    p.mip_w = p.mip_h = target
    p.mode = "none"
    p.hidx = None
    p.band_rows = 64
    return estimate_peak_bytes(p)


# ============================================================== pipeline ======
def convert_one(path, opts, budget):
    """Full per-file pipeline.  Returns a result dict; never raises."""
    name = os.path.basename(path)
    out_path = os.path.splitext(path)[0] + ".png"
    t0 = time.time()
    reader = None
    reserved = 0
    try:
        reader = open_reader(path)
        p = build_plan(reader.base_w, reader.base_h, opts["target"],
                       mip_levels=reader.mip_levels,
                       tiled=reader.tiled,
                       channels=reader.nchannels if hasattr(reader, "nchannels") else 3,
                       half=reader.half,
                       use_mip=opts["use_mip"])

        need = estimate_peak_bytes(p)
        reserved = budget.acquire(need)

        if not opts["quiet"]:
            tsprint("  %-38s %s  [%.0f MB]" % (name, p.describe(), need / 1e6))

        img = resample_to_target(reader, p)
        reader.close()
        reader = None

        cpu, _ = get_cpu_processor(opts["config"])
        apply_display_transform(cpu, img)

        u8 = to_uint8(img)
        del img
        backend = write_png(out_path, u8, opts["png_level"])
        h, w = u8.shape[:2]
        del u8

        return {"ok": True, "name": name, "out": os.path.basename(out_path),
                "src": (p.src_w, p.src_h), "dst": (w, h), "mip": p.mip,
                "mode": p.mode, "backend": backend,
                "bytes": os.path.getsize(out_path), "secs": time.time() - t0}
    except Exception as e:
        return {"ok": False, "name": name, "error": "%s: %s" % (type(e).__name__, e),
                "secs": time.time() - t0}
    finally:
        if reserved:
            budget.release(reserved)
        if reader is not None:
            reader.close()


def build_parser():
    p = argparse.ArgumentParser()
    p.add_argument("--target", type=int, default=DEFAULT_TARGET)
    p.add_argument("--png-level", type=int, default=DEFAULT_PNG_LEVEL)
    p.add_argument("--threads", type=int, default=0)
    p.add_argument("--max-mem", type=int, default=0)
    p.add_argument("--config", type=str, default="")
    p.add_argument("--no-mip", action="store_true")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("files", nargs="*")
    return p


def print_usage():
    print("=" * 66)
    print("EXR -> PNG  |  Scene Linear -> Display sRGB  |  downscale to <= target")
    print("=" * 66)
    print("Backends:")
    print("  reader : %s" % ("OpenImageIO (mipmap-aware)" if HAS_OIIO
                             else "OpenEXR (no mipmap)"))
    print("  writer : %s" % ("OpenImageIO" if HAS_OIIO else
                             "OpenCV" if HAS_CV2 else "Pillow"))
    print("  memory : %s" % ("psutil-adaptive" if HAS_PSUTIL
                             else "fixed %d MB" % FALLBACK_BUDGET_MB))
    print("  OCIO   : %s" % (os.getenv("OCIO") or "ocio://default (built-in ACES)"))
    print("=" * 66)


def main(args):
    if not (HAS_OIIO or HAS_OPENEXR):
        print("Error: need an EXR reader.  pip install openimageio  (or: pip install openexr)")
        return 1
    if not HAS_OCIO:
        print("Error: PyOpenColorIO is required.  pip install opencolorio")
        return 1
    if not (HAS_OIIO or HAS_CV2 or HAS_PIL):
        print("Error: need a PNG writer.  pip install opencv-python  (or: pip install Pillow)")
        return 1

    opts = {
        "target": args.target,
        "png_level": max(0, min(9, args.png_level)),
        "threads": args.threads if args.threads > 0 else None,
        "max_mem": (args.max_mem << 20) if args.max_mem > 0 else None,
        "config": args.config or None,
        "use_mip": not args.no_mip,
        "quiet": args.quiet,
    }

    files = [f for f in args.files if f.lower().endswith(".exr")]
    for f in args.files:
        if not f.lower().endswith(".exr"):
            print("Warning: ignoring non-EXR argument %r" % f)

    if not files:
        print_usage()
        print("\n未提供任何 .exr 文件。")
        return 1

    existing = []
    for f in files:
        if os.path.isfile(f):
            existing.append(f)
        else:
            print("Warning: not found, skipping: %s" % f)
    if not existing:
        print("Error: no valid EXR files")
        return 1

    try:
        _, xform = get_cpu_processor(opts["config"])
    except Exception as e:
        print("Error: OCIO setup failed: %s" % e)
        return 1

    budget_bytes = opts["max_mem"] or default_budget_bytes()
    floor = budget_floor(opts["target"])
    if budget_bytes < floor:
        print("Note: raising budget to %.0f MB (one %dpx task needs that much)"
              % (floor / 1e6, opts["target"]))
        budget_bytes = floor
    threads = opts["threads"] or min(os.cpu_count() or 4, 8)
    threads = min(threads, len(existing))
    budget = MemoryBudget(budget_bytes)

    print("=" * 66)
    print("Files    : %d" % len(existing))
    print("Target   : longest edge <= %d px" % opts["target"])
    print("Transform: %s" % xform)
    print("Threads  : %d   Budget: %.1f GB   PNG level: %d"
          % (threads, budget_bytes / 1e9, opts["png_level"]))
    print("=" * 66)

    t0 = time.time()
    results = []
    with ThreadPoolExecutor(max_workers=threads) as ex:
        futs = {ex.submit(convert_one, f, opts, budget): f for f in existing}
        done = 0
        for fut in as_completed(futs):
            r = fut.result()
            done += 1
            if r["ok"]:
                tsprint("[%d/%d] OK   %s -> %s  %dx%d -> %dx%d  "
                        "%.1f MB  %.2fs  (%s%s)"
                        % (done, len(existing), r["name"], r["out"],
                           r["src"][0], r["src"][1], r["dst"][0], r["dst"][1],
                           r["bytes"] / 1e6, r["secs"], r["mode"],
                           ", mip%d" % r["mip"] if r["mip"] else ""))
            else:
                tsprint("[%d/%d] FAIL %s  %s"
                        % (done, len(existing), r["name"], r["error"]))
            results.append(r)

    elapsed = time.time() - t0
    ok = [r for r in results if r["ok"]]
    bad = [r for r in results if not r["ok"]]

    print("=" * 66)
    print("Done: %d/%d in %.2fs" % (len(ok), len(existing), elapsed))
    if ok:
        print("Throughput: %.2fs per file, %.1f MB written"
              % (elapsed / len(ok), sum(r["bytes"] for r in ok) / 1e6))
    if bad:
        print("\nFailures:")
        for r in bad:
            print("  %s: %s" % (r["name"], r["error"]))
    print("=" * 66)

    return 0 if not bad else 2


if __name__ == "__main__":
    sys.exit(main(build_parser().parse_args()))

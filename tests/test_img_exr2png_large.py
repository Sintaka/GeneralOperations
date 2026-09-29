import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import threading
import types
import unittest
from unittest import mock

import numpy as np


SCRIPT_PATH = (Path(__file__).resolve().parents[1] / "core" / "python" /
               "scripts" / "Image" / "Format Convert" /
               "img.EXR2PNG_Large.py")
SPEC = importlib.util.spec_from_file_location("img_exr2png_large", SCRIPT_PATH)
EXR2PNG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXR2PNG)


class _PixelType:
    FLOAT = 1

    def __init__(self, value):
        self.value = value


class _FakeImath:
    PixelType = _PixelType


class _Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y


class _DataWindow:
    def __init__(self, width, height, y_offset=0):
        self.min = _Point(0, y_offset)
        self.max = _Point(width - 1, y_offset + height - 1)


class _Channel:
    def __init__(self, pixel_type):
        self.type = pixel_type


class _FakeInputFile:
    def __init__(self, arrays, pixel_types, y_offset=0):
        self.arrays = arrays
        first = next(iter(arrays.values()))
        self.y_offset = y_offset
        self.calls = []
        self._header = {
            "dataWindow": _DataWindow(first.shape[1], first.shape[0], y_offset),
            "channels": {name: _Channel(pixel_types[name]) for name in arrays},
        }

    def header(self):
        return self._header

    def channel(self, name, _pixel_type, *bounds):
        self.calls.append((name, bounds))
        data = self.arrays[name]
        if bounds:
            first, last = bounds
            data = data[first - self.y_offset:last - self.y_offset + 1]
        return np.asarray(data, dtype=np.float32).tobytes()

    def close(self):
        pass


@contextlib.contextmanager
def _fallback_reader(arrays, pixel_types):
    fake = _FakeInputFile(arrays, pixel_types)
    fake_openexr = types.SimpleNamespace(InputFile=lambda _path: fake)
    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.object(EXR2PNG, "HAS_OIIO", False))
        stack.enter_context(mock.patch.object(EXR2PNG, "HAS_OPENEXR", True))
        stack.enter_context(mock.patch.object(
            EXR2PNG, "OpenEXR", fake_openexr, create=True))
        stack.enter_context(mock.patch.object(
            EXR2PNG, "Imath", _FakeImath, create=True))
        yield EXR2PNG.open_reader("mock.exr"), fake


class OpenExrReaderTests(unittest.TestCase):
    def test_single_r_half_and_float_expand_to_rgb_with_one_read_per_band(self):
        source = np.arange(12, dtype=np.float32).reshape(3, 4) / 10.0
        for pixel_type in ("HALF", "FLOAT"):
            with self.subTest(pixel_type=pixel_type):
                with _fallback_reader({"R": source}, {"R": pixel_type}) as (reader, fake):
                    bands = [reader.read_band(0, 2, 4), reader.read_band(2, 3, 4)]
                    self.assertEqual(reader.nchannels, 1)
                    self.assertEqual(reader.half, pixel_type == "HALF")
                    self.assertEqual([call[0] for call in fake.calls], ["R", "R"])
                    for (start, end), band in zip(((0, 2), (2, 3)), bands):
                        expected = source[start:end]
                        self.assertTrue(np.array_equal(band[:, :, 0], expected))
                        self.assertTrue(np.array_equal(band[:, :, 1], expected))
                        self.assertTrue(np.array_equal(band[:, :, 2], expected))

    def test_y_expands_to_rgb_and_reads_once(self):
        source = np.arange(8, dtype=np.float32).reshape(2, 4)
        with _fallback_reader({"Y": source}, {"Y": "HALF"}) as (reader, fake):
            band = reader.read_band(0, 2, 4)
            self.assertEqual([call[0] for call in fake.calls], ["Y"])
            self.assertTrue(np.array_equal(band, np.repeat(source[:, :, None], 3, axis=2)))

    def test_bare_rgb_keeps_channel_order_and_reads_each_once(self):
        arrays = {
            "R": np.full((2, 3), 1.0, np.float32),
            "G": np.full((2, 3), 2.0, np.float32),
            "B": np.full((2, 3), 3.0, np.float32),
        }
        with _fallback_reader(arrays, {name: "FLOAT" for name in arrays}) as (reader, fake):
            band = reader.read_band(0, 2, 3)
            self.assertEqual([call[0] for call in fake.calls], ["R", "G", "B"])
            self.assertTrue(np.array_equal(band, np.stack(list(arrays.values()), axis=2)))

    def test_two_channel_and_arbitrary_single_aov_remain_unsupported(self):
        sample = np.zeros((1, 1), np.float32)
        for arrays in ({"R": sample, "A": sample}, {"depth.Z": sample}):
            with self.subTest(channels=list(arrays)):
                with self.assertRaises(EXR2PNG.ReaderError):
                    with _fallback_reader(
                            arrays, {name: "FLOAT" for name in arrays}) as value:
                        value[0]


class ManifestTests(unittest.TestCase):
    def test_config_default_is_an_empty_manifest_field(self):
        source = SCRIPT_PATH.read_text(encoding="utf-8")
        line = next(line for line in source.splitlines()
                    if line.startswith("@param  config"))
        fields = [field.strip() for field in line[len("@param  "):].split(":")]
        self.assertEqual(fields[2], "")
        self.assertNotIn('""', fields[2])

    def test_optional_dependencies_are_exactly_pinned(self):
        source = SCRIPT_PATH.read_text(encoding="utf-8")
        line = next(line for line in source.splitlines()
                    if line.startswith("@requires"))
        requirements = line.split()[1:]
        self.assertIn("OpenImageIO==3.1.14.0?", requirements)
        self.assertIn("opencv-python-headless==4.11.0.86?", requirements)
        self.assertIn("psutil==7.2.2?", requirements)


class MainConcurrencyTests(unittest.TestCase):
    def test_threads_zero_overlaps_tasks(self):
        barrier = threading.Barrier(2)
        lock = threading.Lock()
        active = 0
        maximum = 0

        def fake_convert(path, _opts, _budget):
            nonlocal active, maximum
            with lock:
                active += 1
                maximum = max(maximum, active)
            try:
                barrier.wait(timeout=5)
            finally:
                with lock:
                    active -= 1
            return {"ok": True, "name": os.path.basename(path), "out": "mock.png",
                    "src": (1, 1), "dst": (1, 1), "mip": 0, "mode": "none",
                    "backend": "mock", "bytes": 0, "secs": 0.0}

        with tempfile.TemporaryDirectory() as temp_dir:
            files = [os.path.join(temp_dir, name) for name in ("a.exr", "b.exr")]
            for path in files:
                Path(path).touch()
            args = EXR2PNG.build_parser().parse_args(
                ["--threads", "0", "--quiet", *files])
            patches = (
                mock.patch.object(EXR2PNG, "HAS_OIIO", False),
                mock.patch.object(EXR2PNG, "HAS_OPENEXR", True),
                mock.patch.object(EXR2PNG, "HAS_OCIO", True),
                mock.patch.object(EXR2PNG, "HAS_PIL", True),
                mock.patch.object(EXR2PNG, "get_cpu_processor", return_value=(None, "mock")),
                mock.patch.object(EXR2PNG, "default_budget_bytes", return_value=1 << 30),
                mock.patch.object(EXR2PNG, "budget_floor", return_value=1),
                mock.patch.object(EXR2PNG, "convert_one", side_effect=fake_convert),
                mock.patch.object(EXR2PNG.os, "cpu_count", return_value=2),
            )
            with contextlib.ExitStack() as stack, contextlib.redirect_stdout(io.StringIO()):
                for patch in patches:
                    stack.enter_context(patch)
                result = EXR2PNG.main(args)
        self.assertEqual(result, 0)
        self.assertEqual(maximum, 2)


if __name__ == "__main__":
    unittest.main()

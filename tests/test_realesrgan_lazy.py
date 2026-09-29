import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

from PIL import Image


SCRIPT_PATH = (Path(__file__).resolve().parents[1] / "core" / "python" / "scripts"
               / "Image" / "ReSize" / "img.RealESRGAN_Upscale.py")
SPEC = importlib.util.spec_from_file_location("img_realesrgan_upscale", SCRIPT_PATH)
REALESRGAN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REALESRGAN)


def _make_image(path, size=(4, 2)):
    Image.new("RGB", size, color=(80, 120, 160)).save(path)


def _mock_release_zip():
    payload = io.BytesIO()
    with zipfile.ZipFile(payload, "w") as archive:
        archive.writestr(REALESRGAN.EXE_NAME, b"mock executable")
        archive.writestr("models/mock.param", b"mock model")
    content = payload.getvalue()
    return content, hashlib.sha256(content).hexdigest().upper()


class RealEsrganLazyTests(unittest.TestCase):
    def test_user_png_reaches_fallback_and_keeps_expected_dimensions(self):
        fixture = (Path(__file__).resolve().parents[1] / ".sandbox" / "fixtures"
                   / "realesrgan-user-input.png")
        if not fixture.is_file():
            self.skipTest("local user PNG fixture is not present")
        self.assertEqual(hashlib.sha256(fixture.read_bytes()).hexdigest(),
                         "2030e30a6ce6a37ebaa01fa8a072c910db53a0bc7406439bb807f4cc17295a35")

        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / fixture.name
            shutil.copyfile(fixture, source)
            args = REALESRGAN.build_parser().parse_args(["--scale", "2", str(source)])
            output = io.StringIO()

            with mock.patch.object(REALESRGAN, "ensure_tool", return_value=(None, "offline")), \
                    contextlib.redirect_stdout(output):
                result = REALESRGAN.main(args)

            self.assertEqual(result, 0, output.getvalue())
            with Image.open(source) as original, Image.open(source.with_name(
                    f"{source.stem}_x2.png")) as scaled:
                self.assertEqual(original.size, (1160, 1696))
                self.assertEqual(scaled.size, (2320, 3392))
                scaled.verify()
            self.assertIn("非 AI 保底", output.getvalue())
            self.assertIn("正在检查 Real-ESRGAN 本地工具缓存", output.getvalue())

    def test_download_failure_uses_pillow_fallback_and_reports_it(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            source = temp / "sample.png"
            _make_image(source)
            args = REALESRGAN.build_parser().parse_args(["--scale", "2", str(source)])
            output = io.StringIO()

            with mock.patch.dict(os.environ, {"GO_RUNTIME_HOME": str(temp / "runtime")},
                                 clear=False), \
                    mock.patch.object(REALESRGAN.urllib.request, "urlopen",
                                      side_effect=OSError("offline")), \
                    contextlib.redirect_stdout(output):
                result = REALESRGAN.main(args)

            rendered = output.getvalue()
            out_path = temp / "sample_x2.png"
            self.assertEqual(result, 0)
            self.assertTrue(out_path.is_file())
            with Image.open(out_path) as image:
                self.assertEqual(image.size, (8, 4))
            self.assertIn("offline", rendered)
            self.assertIn("正在从 GitHub 下载", rendered)
            self.assertIn("非 AI 保底", rendered)
            self.assertIn("成功 1 张，失败 0 张", rendered)
            cache_tools = temp / "runtime" / "tools"
            if cache_tools.exists():
                self.assertEqual(list(cache_tools.iterdir()), [])

    def test_vulkan_tool_failure_uses_pillow_fallback_at_requested_size(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            source = temp / "sample.png"
            _make_image(source)
            exe = temp / "realesrgan-ncnn-vulkan.exe"
            exe.touch()
            args = REALESRGAN.build_parser().parse_args(["--scale", "1.5", str(source)])
            output = io.StringIO()

            with mock.patch.object(REALESRGAN, "ensure_tool", return_value=(exe, None)), \
                    mock.patch.object(REALESRGAN, "_run_tool", return_value=(127, None)), \
                    contextlib.redirect_stdout(output):
                result = REALESRGAN.main(args)

            rendered = output.getvalue()
            out_path = temp / "sample_x1.5.png"
            self.assertEqual(result, 0)
            self.assertTrue(out_path.is_file())
            with Image.open(out_path) as image:
                self.assertEqual(image.size, (6, 3))
            self.assertIn("Real-ESRGAN/Vulkan 执行失败", rendered)
            self.assertIn("非 AI 保底", rendered)

    def test_hash_mismatch_uses_fallback_and_does_not_install_cache(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            source = temp / "sample.png"
            _make_image(source)
            release, _digest = _mock_release_zip()
            args = REALESRGAN.build_parser().parse_args(["--scale", "2", str(source)])
            output = io.StringIO()

            with mock.patch.dict(os.environ, {"GO_RUNTIME_HOME": str(temp / "runtime")},
                                 clear=False), \
                    mock.patch.object(REALESRGAN.urllib.request, "urlopen",
                                      return_value=io.BytesIO(release)), \
                    mock.patch.object(REALESRGAN, "DOWNLOAD_SHA256", "0" * 64), \
                    contextlib.redirect_stdout(output):
                result = REALESRGAN.main(args)

            rendered = output.getvalue()
            self.assertEqual(result, 0)
            self.assertIn("SHA256 不匹配", rendered)
            self.assertIn("非 AI 保底", rendered)
            self.assertTrue((temp / "sample_x2.png").is_file())
            tools_dir = temp / "runtime" / "tools"
            self.assertEqual(list(tools_dir.iterdir()), [])

    def test_download_installs_verified_release_from_staging_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            release, digest = _mock_release_zip()
            with mock.patch.dict(os.environ, {"GO_RUNTIME_HOME": str(temp / "runtime")},
                                 clear=False), \
                    mock.patch.object(REALESRGAN.urllib.request, "urlopen",
                                      return_value=io.BytesIO(release)), \
                    mock.patch.object(REALESRGAN, "DOWNLOAD_SHA256", digest):
                exe, error = REALESRGAN.ensure_tool()

            expected = (temp / "runtime" / "tools" / "realesrgan"
                        / REALESRGAN.EXE_NAME)
            self.assertIsNone(error)
            self.assertEqual(exe, expected)
            self.assertEqual(expected.read_bytes(), b"mock executable")
            self.assertEqual((expected.parent / "models" / "mock.param").read_bytes(),
                             b"mock model")
            self.assertEqual(list(expected.parent.parent.iterdir()), [expected.parent])

    def test_successful_tool_path_keeps_existing_output_behavior(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            source = temp / "sample.png"
            _make_image(source, size=(5, 3))
            exe = temp / "realesrgan-ncnn-vulkan.exe"
            exe.touch()
            args = REALESRGAN.build_parser().parse_args(["--scale", "2", str(source)])
            output = io.StringIO()

            def fake_tool(cmd):
                input_path = cmd[cmd.index("-i") + 1]
                output_path = cmd[cmd.index("-o") + 1]
                scale = int(cmd[cmd.index("-s") + 1])
                with Image.open(input_path) as image:
                    image.resize((image.width * scale, image.height * scale),
                                 Image.LANCZOS).save(output_path)
                return 0, None

            with mock.patch.object(REALESRGAN, "ensure_tool", return_value=(exe, None)), \
                    mock.patch.object(REALESRGAN, "_run_tool", side_effect=fake_tool), \
                    contextlib.redirect_stdout(output):
                result = REALESRGAN.main(args)

            rendered = output.getvalue()
            out_path = temp / "sample_x2.png"
            self.assertEqual(result, 0)
            self.assertTrue(out_path.is_file())
            with Image.open(out_path) as image:
                self.assertEqual(image.size, (10, 6))
            self.assertIn("sample_x2.png (10x6)", rendered)
            self.assertNotIn("非 AI 保底", rendered)

    def test_zero_exit_with_vulkan_device_loss_uses_fallback(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "sample.png"
            _make_image(source)
            exe = Path(temp_dir) / REALESRGAN.EXE_NAME
            exe.touch()
            output = io.StringIO()

            def failed_tool(cmd):
                out_path = Path(cmd[cmd.index("-o") + 1])
                Image.new("RGB", (8, 4), color="black").save(out_path)
                return 0, "vkQueueSubmit failed -4"

            with mock.patch.object(REALESRGAN, "_run_tool", side_effect=failed_tool), \
                    contextlib.redirect_stdout(output):
                ok, message = REALESRGAN.process_one(exe, None, str(source), 2)

            self.assertTrue(ok, message)
            self.assertIn("vkQueueSubmit failed -4", message)
            self.assertIn("非 AI 保底", message)
            with Image.open(source.with_name("sample_x2.png")) as image:
                self.assertIsNotNone(image.convert("RGB").getbbox())

    def test_tool_streams_and_detects_vulkan_failure_despite_zero_exit(self):
        error_output = io.StringIO()
        with contextlib.redirect_stderr(error_output):
            exit_code, failure = REALESRGAN._run_tool([
                sys.executable, "-c", "import sys; print('vkQueueSubmit failed -4', file=sys.stderr)",
            ])
        self.assertEqual(exit_code, 0)
        self.assertEqual(failure, "vkQueueSubmit failed -4")
        self.assertIn("vkQueueSubmit failed -4", error_output.getvalue())

    def test_silent_all_black_ai_output_uses_fallback_for_nonblack_source(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "sample.png"
            _make_image(source)
            exe = Path(temp_dir) / REALESRGAN.EXE_NAME
            exe.touch()

            def black_tool(cmd):
                Image.new("RGB", (8, 4), color="black").save(cmd[cmd.index("-o") + 1])
                return 0, None

            with mock.patch.object(REALESRGAN, "_run_tool", side_effect=black_tool):
                ok, message = REALESRGAN.process_one(exe, None, str(source), 2)

            self.assertTrue(ok, message)
            self.assertIn("输出全黑", message)
            with Image.open(source.with_name("sample_x2.png")) as image:
                self.assertIsNotNone(image.convert("RGB").getbbox())

    def test_find_tool_prefers_bundled_tool_then_uses_runtime_cache(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            bundled = temp / "bundle" / REALESRGAN.EXE_NAME
            cached = temp / "runtime" / "tools" / "realesrgan" / REALESRGAN.EXE_NAME
            bundled.parent.mkdir(parents=True)
            cached.parent.mkdir(parents=True)
            bundled.touch()
            cached.touch()

            with mock.patch.object(REALESRGAN, "_find_bundled_tool", return_value=bundled), \
                    mock.patch.dict(os.environ, {"GO_RUNTIME_HOME": str(temp / "runtime")},
                                    clear=False):
                self.assertEqual(REALESRGAN.find_tool(), bundled)

            with mock.patch.object(REALESRGAN, "_find_bundled_tool", return_value=None), \
                    mock.patch.dict(os.environ, {"GO_RUNTIME_HOME": str(temp / "runtime")},
                                    clear=False):
                self.assertEqual(REALESRGAN.find_tool(), cached)

    def test_safe_extract_rejects_parent_traversal(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            archive_path = temp / "unsafe.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr("../escape.txt", "bad")
            with self.assertRaisesRegex(ValueError, "不安全路径"):
                REALESRGAN._safe_extract(archive_path, temp / "extract")
            self.assertFalse((temp / "escape.txt").exists())


if __name__ == "__main__":
    unittest.main()

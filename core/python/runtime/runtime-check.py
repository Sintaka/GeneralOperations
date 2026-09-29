#!/usr/bin/env python3
"""Small stdlib-only probes used by python-launcher.ps1."""

from __future__ import print_function

import argparse
import importlib
import importlib.util
import json
import platform
import struct
import sys


IMPORT_NAMES = {
    "pillow": "PIL",
    "opencv-python": "cv2",
    "opencv-python-headless": "cv2",
    "opencolorio": "PyOpenColorIO",
}


def _emit(result, status):
    sys.stdout.write(json.dumps(result, ensure_ascii=True, sort_keys=True) + "\n")
    return status


def check_base():
    problems = []
    if platform.python_implementation() != "CPython":
        problems.append("解释器不是 CPython")
    if struct.calcsize("P") != 8:
        problems.append("解释器不是 64 位")
    if importlib.util.find_spec("pip") is None:
        problems.append("未找到 pip")
    return _emit({"ok": not problems, "problems": problems}, 0 if not problems else 2)


def _installed_version(distribution):
    try:
        from importlib import metadata
        return metadata.version(distribution)
    except ImportError:
        # Python 3.7 compatibility for system interpreters. The bundled
        # Python 3.13 always takes the importlib.metadata path.
        try:
            import pkg_resources
            return pkg_resources.get_distribution(distribution).version
        except Exception:
            return None
    except Exception:
        return None


def _parse_requirement(raw):
    value = raw.rstrip("?")
    name, marker, version = value.partition("==")
    name = name.strip()
    if not name:
        raise ValueError("空依赖项")
    module = IMPORT_NAMES.get(name.lower(), name.replace("-", "_"))
    return name, module, version if marker else None


def check_requirements(raw_json):
    try:
        raw_requirements = json.loads(raw_json)
        if not isinstance(raw_requirements, list) or not all(
            isinstance(item, str) for item in raw_requirements
        ):
            raise ValueError("依赖清单必须是字符串数组")
    except Exception as exc:
        return _emit({"ok": False, "problems": ["依赖清单无效: " + str(exc)]}, 2)

    missing = []
    for raw in raw_requirements:
        try:
            distribution, module, expected = _parse_requirement(raw)
            importlib.import_module(module)
            if expected:
                actual = _installed_version(distribution)
                if actual is None:
                    missing.append(distribution + " (无法读取已安装版本)")
                elif expected.endswith("*"):
                    if not actual.startswith(expected[:-1]):
                        missing.append(distribution + "==" + expected)
                elif actual != expected:
                    missing.append(distribution + "==" + expected)
        except Exception as exc:
            missing.append(raw + " (" + type(exc).__name__ + ")")

    return _emit({"ok": not missing, "missing": missing}, 0 if not missing else 1)


def main():
    parser = argparse.ArgumentParser(add_help=False)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--base", action="store_true")
    mode.add_argument("--requirements-json")
    args = parser.parse_args()
    if args.base:
        return check_base()
    return check_requirements(args.requirements_json)


if __name__ == "__main__":
    sys.exit(main())

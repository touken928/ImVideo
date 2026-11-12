#!/usr/bin/env python3
"""Format or check project-owned C/C++ files without traversing build caches."""

import argparse
from pathlib import Path
import re
import subprocess


VERSION = "22.1.8"
ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = (
    "include", "src", "tests", "example/implayer/src",
)
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="Apply formatting; the default only checks.")
    parser.add_argument("--clang-format", default="clang-format", help="Path to clang-format 22.1.8.")
    args = parser.parse_args()
    try:
        version = subprocess.run(
            [args.clang_format, "--version"], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        parser.error(f"Cannot run clang-format: {error}")
    match = re.search(r"version\s+(\d+\.\d+\.\d+)", version)
    if not match or match.group(1) != VERSION:
        parser.error(f"Expected clang-format {VERSION}; found {version.strip()}")
    files = sorted({
        path for directory in SOURCE_ROOTS for path in (ROOT / directory).rglob("*")
        if path.is_file() and path.suffix in SUFFIXES
    })
    mode = ["-i"] if args.write else ["--dry-run", "--Werror"]
    result = subprocess.run([args.clang_format, *mode, *(str(path) for path in files)])
    if result.returncode == 0:
        print(f"{'Formatted' if args.write else 'Checked'} {len(files)} C/C++ files.")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())

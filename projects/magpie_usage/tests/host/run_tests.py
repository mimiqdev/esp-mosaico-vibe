#!/usr/bin/env python3
"""Host-side tests for the Magpie encoder, parsers and formatting helpers.

The firmware sources tested here are portable C with no ESP-IDF dependency, so
this runner simply compiles them with the host compiler and runs a small test
harness. Fixtures under fixtures/ were captured from a real Magpie gateway by
tools/make_fixtures.py (read-only GETs, scrubbed of account data).

Usage:
    python3 tests/host/run_tests.py             # build and run
    python3 tests/host/run_tests.py --verbose   # keep compiler output
    python3 tests/host/run_tests.py --keep      # keep the built binary

Requirements: any C11 compiler (`cc`), Python 3.8+. No ESP-IDF, no network,
no device.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
MAIN = PROJECT / "main"
BUILD = HERE / "build"
BINARY = BUILD / "magpie_host_tests"

SOURCES = [
    MAIN / "magpie_json.c",
    MAIN / "magpie_parse.c",
    MAIN / "magpie_format.c",
    HERE / "test_magpie.c",
]

CFLAGS = [
    "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
]


def compiler() -> str:
    for candidate in (os.environ.get("CC"), "cc", "clang", "gcc"):
        if candidate and shutil.which(candidate):
            return candidate
    raise SystemExit("no C compiler found (set CC or install cc)")


def build(verbose: bool) -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    command = [
        compiler(), *CFLAGS,
        *[str(source) for source in SOURCES],
        f"-I{MAIN}",
        "-o", str(BINARY),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if verbose or result.returncode != 0:
        print("$ " + " ".join(command))
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
    if result.returncode != 0:
        raise SystemExit("host build failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--keep", action="store_true",
                        help="keep build artifacts and the test binary")
    args = parser.parse_args()
    build(args.verbose)
    command = [str(BINARY), str(HERE / "fixtures")]
    if args.verbose:
        print("$ " + " ".join(command))
    result = subprocess.run(command, text=True)
    if not args.keep:
        shutil.rmtree(BUILD, ignore_errors=True)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())

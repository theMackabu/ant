#!/usr/bin/env python3

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


SOURCE_SUFFIXES = {".c", ".cc", ".cpp"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp"}
OWNED_ROOTS = {"include", "src"}
HUNK_PATTERN = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def run(command, *, cwd, check=True):
    return subprocess.run(
        command,
        cwd=cwd,
        check=check,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def repo_relative(repo_root, path):
    resolved = path if path.is_absolute() else repo_root / path
    try:
        return resolved.resolve().relative_to(repo_root)
    except ValueError as error:
        raise ValueError(f"path is outside the repository: {path}") from error


def is_owned(relative_path):
    return len(relative_path.parts) > 1 and relative_path.parts[0] in OWNED_ROOTS


def load_compile_sources(repo_root, compile_database):
    with compile_database.open(encoding="utf-8") as source:
        entries = json.load(source)

    sources = set()
    for entry in entries:
        entry_path = Path(entry["file"])
        if not entry_path.is_absolute():
            entry_path = Path(entry["directory"]) / entry_path
        try:
            relative_path = entry_path.resolve().relative_to(repo_root)
        except ValueError:
            continue
        if is_owned(relative_path) and relative_path.suffix in SOURCE_SUFFIXES:
            sources.add(relative_path)
    return sources


def git_paths(repo_root, command):
    result = run(command, cwd=repo_root)
    return {Path(line) for line in result.stdout.splitlines() if line}


def changed_paths(repo_root):
    tracked = git_paths(
        repo_root,
        ["git", "diff", "--name-only", "--diff-filter=ACMR", "HEAD", "--", "src", "include"],
    )
    untracked = git_paths(
        repo_root,
        ["git", "ls-files", "--others", "--exclude-standard", "--", "src", "include"],
    )
    return tracked | untracked, untracked


def changed_line_ranges(repo_root, relative_path, untracked):
    if relative_path in untracked:
        with (repo_root / relative_path).open(encoding="utf-8", errors="replace") as source:
            line_count = sum(1 for _ in source)
        return [[1, max(1, line_count)]]

    result = run(
        ["git", "diff", "--unified=0", "HEAD", "--", relative_path.as_posix()],
        cwd=repo_root,
    )
    ranges = []
    for line in result.stdout.splitlines():
        match = HUNK_PATTERN.match(line)
        if match is None:
            continue
        start = int(match.group(1))
        count = int(match.group(2) or "1")
        if count > 0:
            ranges.append([start, start + count - 1])
    return ranges


def llvm_major(repo_root):
    versions_path = repo_root / ".github" / "versions.json"
    try:
        with versions_path.open(encoding="utf-8") as source:
            return str(json.load(source)["tools"]["llvm"])
    except (KeyError, OSError, ValueError, TypeError):
        return "21"


def find_clang_tidy(repo_root):
    configured = os.environ.get("ANT_CLANG_TIDY")
    if configured:
        configured_path = shutil.which(configured) or configured
        if os.access(configured_path, os.X_OK):
            return configured_path
        raise RuntimeError(f"ANT_CLANG_TIDY is not executable: {configured}")

    major = llvm_major(repo_root)
    for name in (f"clang-tidy-{major}", "clang-tidy"):
        found = shutil.which(name)
        if found:
            return found

    candidates = [
        Path(f"/opt/homebrew/opt/llvm@{major}/bin/clang-tidy"),
        Path(f"/usr/local/opt/llvm@{major}/bin/clang-tidy"),
        Path("/opt/homebrew/opt/llvm/bin/clang-tidy"),
        Path("/usr/local/opt/llvm/bin/clang-tidy"),
    ]
    for candidate in candidates:
        if os.access(candidate, os.X_OK):
            return str(candidate)

    raise RuntimeError(
        f"clang-tidy {major} or newer was not found; install the repository LLVM toolchain "
        "or set ANT_CLANG_TIDY to its executable"
    )


def validate_clang_tidy(clang_tidy, repo_root):
    result = run([clang_tidy, "--version"], cwd=repo_root)
    match = re.search(r"version\s+(\d+)", result.stdout + result.stderr)
    required = int(llvm_major(repo_root))
    if match is not None and int(match.group(1)) < required:
        raise RuntimeError(f"clang-tidy {required} or newer is required: {result.stdout.strip()}")


def platform_clang_args(repo_root):
    if sys.platform != "darwin":
        return []

    sdk_root = os.environ.get("SDKROOT")
    if not sdk_root:
        xcrun = shutil.which("xcrun")
        if xcrun:
            result = run([xcrun, "--show-sdk-path"], cwd=repo_root, check=False)
            if result.returncode == 0:
                sdk_root = result.stdout.strip()
    if not sdk_root:
        raise RuntimeError("the macOS SDK path is unavailable; set SDKROOT before running Clang-Tidy")
    return ["--extra-arg=-isysroot", f"--extra-arg={sdk_root}"]


def analyze_one(clang_tidy, repo_root, build_dir, relative_path, line_ranges, extra_args, enforce):
    command = [clang_tidy, "--quiet", "-p", str(build_dir)]
    command.extend(extra_args)
    if enforce:
        command.append("--warnings-as-errors=*")
    if line_ranges is not None:
        line_filter = [{"name": relative_path.as_posix(), "lines": line_ranges}]
        command.append(f"--line-filter={json.dumps(line_filter, separators=(',', ':'))}")
    command.append(str(repo_root / relative_path))
    result = run(command, cwd=repo_root, check=False)
    return relative_path, result.returncode, result.stdout + result.stderr


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run Ant's Clang-Tidy policy over changed or explicitly selected C sources."
    )
    parser.add_argument("files", nargs="*", help="specific Ant-owned C translation units")
    parser.add_argument("--all", action="store_true", help="analyze every Ant-owned C translation unit")
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=min(4, os.cpu_count() or 1),
        help="maximum concurrent clang-tidy processes",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if args.all and args.files:
        raise RuntimeError("--all cannot be combined with explicit files")
    if args.jobs < 1:
        raise RuntimeError("--jobs must be at least 1")

    repo_root = Path(__file__).resolve().parents[1]
    compile_database = repo_root / "build" / "compile_commands.json"
    if not compile_database.is_file():
        raise RuntimeError("build/compile_commands.json is missing; run maid setup or configure Meson first")

    compile_sources = load_compile_sources(repo_root, compile_database)
    line_ranges = {}

    if args.all:
        selected = compile_sources
    elif args.files:
        selected = set()
        for raw_path in args.files:
            relative_path = repo_relative(repo_root, Path(raw_path))
            if not is_owned(relative_path) or relative_path.suffix not in SOURCE_SUFFIXES:
                raise RuntimeError(f"not an Ant-owned C translation unit: {raw_path}")
            selected.add(relative_path)
    else:
        changed, untracked = changed_paths(repo_root)
        headers = sorted(path for path in changed if is_owned(path) and path.suffix in HEADER_SUFFIXES)
        if headers:
            formatted = ", ".join(path.as_posix() for path in headers)
            raise RuntimeError(f"changed headers require maid lint_c_all: {formatted}")
        selected = {
            path for path in changed if is_owned(path) and path.suffix in SOURCE_SUFFIXES
        }
        for relative_path in selected:
            line_ranges[relative_path] = changed_line_ranges(repo_root, relative_path, untracked)

    missing = sorted(selected - compile_sources)
    if missing:
        formatted = ", ".join(path.as_posix() for path in missing)
        raise RuntimeError(f"sources are absent from the compilation database; reconfigure Meson: {formatted}")

    selected = sorted(selected)
    if not selected:
        print("C lint passed: no changed C translation units")
        return 0

    clang_tidy = find_clang_tidy(repo_root)
    validate_clang_tidy(clang_tidy, repo_root)
    extra_args = platform_clang_args(repo_root)
    print(f"Analyzing {len(selected)} C translation unit(s) with {clang_tidy}")

    with ThreadPoolExecutor(max_workers=min(args.jobs, len(selected))) as executor:
        results = executor.map(
            lambda path: analyze_one(
                clang_tidy,
                repo_root,
                compile_database.parent,
                path,
                None if args.all or args.files else line_ranges[path],
                extra_args,
                not args.all,
            ),
            selected,
        )

    failed = False
    for relative_path, returncode, output in results:
        if output.strip():
            print(f"\n[{relative_path.as_posix()}]\n{output.rstrip()}")
        if returncode != 0:
            failed = True

    if failed:
        print("C lint failed", file=sys.stderr)
        return 1

    print("C lint passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"C lint failed: {error}", file=sys.stderr)
        sys.exit(2)

#!/usr/bin/env python3
"""Reject Qt Multimedia APIs outside the application's raw audio/video boundary."""

import argparse
import re
import sys
from pathlib import Path


SOURCE_SUFFIXES = {".cpp", ".h", ".mm", ".qml"}
EXCLUDED_TOP_LEVEL_DIRECTORIES = {".git", ".claude", "docs", "tests"}
GENERATED_BUILD_OUTPUT_ROOTS = {"build", "out", "output"}
PROHIBITED_TOKENS = (
    "QMediaPlayer",
    "MediaPlayer",
    "QMediaRecorder",
    "MediaRecorder",
    "QAudioDecoder",
    "QCamera",
    "Camera",
    "QMediaCaptureSession",
    "CaptureSession",
)
PROHIBITED_PATTERN = re.compile(r"\b(?:" + "|".join(PROHIBITED_TOKENS) + r")\b")
RAW_STRING_START_PATTERN = re.compile(r'(?:u8|u|U|L)?R"([^\\()\s]{0,16})\(')


def raw_string_literal_end(source, index):
    """Return the index after a complete C++ raw string literal, if present."""
    match = RAW_STRING_START_PATTERN.match(source, index)
    if match is None:
        return None

    terminator = ")" + match.group(1) + '"'
    terminator_index = source.find(terminator, match.end())
    if terminator_index == -1:
        return None
    return terminator_index + len(terminator)


def strip_comments(source):
    """Mask comments without changing the source's line numbering."""
    result = []
    index = 0
    state = "code"
    quote = ""

    while index < len(source):
        character = source[index]
        next_character = source[index + 1] if index + 1 < len(source) else ""

        if state == "line_comment":
            if character == "\n":
                result.append(character)
                state = "code"
            else:
                result.append(" ")
            index += 1
            continue

        if state == "block_comment":
            if character == "*" and next_character == "/":
                result.extend((" ", " "))
                index += 2
                state = "code"
            else:
                result.append("\n" if character == "\n" else " ")
                index += 1
            continue

        if state == "string":
            result.append(character)
            if character == "\\" and index + 1 < len(source):
                result.append(source[index + 1])
                index += 2
            elif character == quote:
                state = "code"
                index += 1
            else:
                index += 1
            continue

        raw_string_end = raw_string_literal_end(source, index)
        if raw_string_end is not None:
            result.append(source[index:raw_string_end])
            index = raw_string_end
        elif character in ('"', "'", "`"):
            result.append(character)
            quote = character
            state = "string"
            index += 1
        elif character == "/" and next_character == "/":
            result.extend((" ", " "))
            index += 2
            state = "line_comment"
        elif character == "/" and next_character == "*":
            result.extend((" ", " "))
            index += 2
            state = "block_comment"
        else:
            result.append(character)
            index += 1

    return "".join(result)


def is_excluded_directory(relative_path):
    """Return whether a directory is a non-production top-level root."""
    if len(relative_path.parts) != 1:
        return False

    name = relative_path.name
    return (
        name in EXCLUDED_TOP_LEVEL_DIRECTORIES
        or name in GENERATED_BUILD_OUTPUT_ROOTS
        or name.startswith("cmake-build-")
        or name.startswith("build-")
        or name.endswith("_build")
    )


def source_files(root):
    """Yield production sources while following only in-root directory symlinks."""
    resolved_root = root.resolve()
    visited_directories = {resolved_root}

    def walk(directory):
        for path in sorted(directory.iterdir()):
            relative_path = path.relative_to(root)
            if path.is_dir():
                if is_excluded_directory(relative_path):
                    continue

                try:
                    resolved_directory = path.resolve(strict=True)
                except (OSError, RuntimeError) as error:
                    raise ValueError(f"cannot resolve directory symlink: {relative_path.as_posix()}") from error

                if path.is_symlink() and not resolved_directory.is_relative_to(resolved_root):
                    raise ValueError(f"directory symlink escapes scan root: {relative_path.as_posix()}")
                if resolved_directory in visited_directories:
                    continue

                visited_directories.add(resolved_directory)
                yield from walk(path)
            elif path.suffix in SOURCE_SUFFIXES and path.is_file():
                if path.is_symlink():
                    try:
                        resolved_file = path.resolve(strict=True)
                    except (OSError, RuntimeError) as error:
                        raise ValueError(f"cannot resolve source-file symlink: {relative_path.as_posix()}") from error
                    if not resolved_file.is_relative_to(resolved_root):
                        raise ValueError(f"source-file symlink escapes scan root: {relative_path.as_posix()}")
                yield path

    yield from walk(root)


def scan(root):
    findings = []
    for path in sorted(source_files(root)):
        source = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        relative_path = path.relative_to(root).as_posix()
        for match in PROHIBITED_PATTERN.finditer(source):
            line = source.count("\n", 0, match.start()) + 1
            findings.append((relative_path, line, match.group()))
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="repository root to scan")
    args = parser.parse_args()

    try:
        findings = scan(args.root.resolve())
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for path, line, token in findings:
        print(f"{path}:{line}: {token}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# =============================================================================
# tools/gcov_uart_decode.py — reconstruct on-target .gcda files from a Renode
# (or any) UART capture of a -DVAIOS_GCOV=ON run.
#
# The on-target dumper (portable/cortex-m4/gcov_dump.c) streams each instrumented
# translation unit's .gcda as base64, framed like:
#
#     @@VAIOS_GCDA_DUMP_BEGIN
#     @@VAIOS_GCDA_BEGIN <absolute .gcda path>
#     <base64 line>
#     <base64 line>
#     @@VAIOS_GCDA_END
#     ... (repeat per TU) ...
#     @@VAIOS_GCDA_DUMP_END
#
# This script strips Renode's per-line "usart2: [ts]" prefix and ANSI colour,
# decodes each frame, and writes the .gcda to the exact absolute path the target
# emitted — i.e. right next to the .gcno the compiler produced — so `gcov` can
# read the pair directly. Paths are honoured verbatim (they are this machine's
# build-tree paths, baked in at compile time); pass --strip/--prefix only to
# relocate a capture produced on a different machine.
#
# Usage:
#   tools/gcov_uart_decode.py <uart.log>
#   renode ... | tools/gcov_uart_decode.py -           # read stdin
#
# Exit: 0 if at least one .gcda was written and the stream was well-framed,
#       1 on framing/decoding errors or if no frames were found.
# =============================================================================
import argparse
import base64
import os
import re
import sys

# Renode's LoggingUartAnalyzer prints each UART line as e.g.
#   "\x1b[...m12:34:56.789 [INFO] usart2: [   0.01] <payload>"
# Strip ANSI first, then everything up to and including the "usart2: [..]" tag.
ANSI = re.compile(r"\x1b\[[0-9;]*m")
UART_PREFIX = re.compile(r"^.*?usart2:\s*\[[^\]]*\]\s?")

BEGIN = "@@VAIOS_GCDA_BEGIN "
END = "@@VAIOS_GCDA_END"
DUMP_BEGIN = "@@VAIOS_GCDA_DUMP_BEGIN"
DUMP_END = "@@VAIOS_GCDA_DUMP_END"
ERROR = "@@VAIOS_GCDA_ERROR"
GCDA_MAGIC = b"adcg"  # little-endian 'gcda' as written by gcov


def clean(line):
    line = ANSI.sub("", line)
    line = UART_PREFIX.sub("", line)
    return line.rstrip("\r\n")


def remap(path, strip, prefix):
    if strip and path.startswith(strip):
        path = path[len(strip):].lstrip("/")
    if prefix:
        path = os.path.join(prefix, path.lstrip("/"))
    return path


def main():
    ap = argparse.ArgumentParser(description="Reconstruct .gcda from a VAIOS_GCOV UART capture")
    ap.add_argument("log", help="UART capture file, or - for stdin")
    ap.add_argument("--strip", default="", help="leading path fragment to strip from emitted .gcda paths")
    ap.add_argument("--prefix", default="", help="path prefix to prepend after stripping")
    ap.add_argument("--quiet", action="store_true", help="only print the summary line")
    args = ap.parse_args()

    src = sys.stdin if args.log == "-" else open(args.log, "r", errors="replace")

    written, errors, saw_dump = [], 0, False
    cur_path, cur_b64 = None, []

    def flush():
        nonlocal errors
        if cur_path is None:
            return
        try:
            raw = base64.b64decode("".join(cur_b64), validate=True)
        except Exception as e:  # malformed base64 in the frame
            print(f"  ERROR decoding {cur_path}: {e}", file=sys.stderr)
            errors += 1
            return
        if raw[:4] != GCDA_MAGIC:
            print(f"  ERROR {cur_path}: bad .gcda magic {raw[:4]!r} ({len(raw)} bytes)", file=sys.stderr)
            errors += 1
            return
        out = remap(cur_path, args.strip, args.prefix)
        os.makedirs(os.path.dirname(out), exist_ok=True) if os.path.dirname(out) else None
        with open(out, "wb") as f:
            f.write(raw)
        written.append(out)
        if not args.quiet:
            print(f"  wrote {len(raw):6d} B  {out}")

    for line in src:
        s = clean(line)
        if not s:
            continue
        if s.startswith(DUMP_BEGIN):
            saw_dump = True
            continue
        if s.startswith(DUMP_END):
            continue
        if s.startswith(ERROR):
            print(f"  TARGET ERROR: {s}", file=sys.stderr)
            errors += 1
            continue
        if s.startswith(BEGIN):
            cur_path, cur_b64 = s[len(BEGIN):].strip(), []
            continue
        if s.startswith(END):
            flush()
            cur_path, cur_b64 = None, []
            continue
        if cur_path is not None:
            # Base64 payload line (alphabet excludes whitespace and '@').
            if re.fullmatch(r"[A-Za-z0-9+/=]+", s):
                cur_b64.append(s)
            # else: interleaved noise (unlikely) — ignore silently
    if args.log != "-":
        src.close()

    print(f"gcov-uart-decode: {len(written)} .gcda written, {errors} error(s)"
          + ("" if saw_dump else ", NO DUMP MARKERS FOUND"))
    return 0 if (written and errors == 0 and saw_dump) else 1


if __name__ == "__main__":
    sys.exit(main())

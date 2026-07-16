#!/usr/bin/env python3
# =============================================================================
# tools/test_gcov_uart_decode.py — regression test for the gcov UART decoder's
# integrity check (STAGE5_REVIEW_FINDINGS #10). Builds synthetic captures and
# runs gcov_uart_decode.py as a subprocess:
#   - a well-framed frame decodes and exits 0;
#   - a frame with a dropped UART line is REJECTED (length mismatch), exit 1,
#     instead of silently writing a short .gcda.
# Exit 0 if all cases behave; nonzero otherwise.
# =============================================================================
import base64
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DECODER = os.path.join(HERE, "gcov_uart_decode.py")

MAGIC = b"adcg"  # little-endian 'gcda'


def b64_lines(raw, width=60):
    enc = base64.b64encode(raw).decode()
    return [enc[i:i + width] for i in range(0, len(enc), width)]


def make_capture(raw, path, *, end_len=None, drop_line=None):
    """A framed capture. end_len overrides the byte count in the END marker;
    drop_line drops that base64 line index (simulating a lost UART line)."""
    lines = b64_lines(raw)
    if drop_line is not None:
        del lines[drop_line]
    n = len(raw) if end_len is None else end_len
    out = ["@@VAIOS_GCDA_DUMP_BEGIN", f"@@VAIOS_GCDA_BEGIN {path}"]
    out += lines
    out += [f"@@VAIOS_GCDA_END {n}", "@@VAIOS_GCDA_DUMP_END"]
    return "\n".join(out) + "\n"


def run(capture, tmpdir):
    logf = os.path.join(tmpdir, "cap.log")
    with open(logf, "w") as f:
        f.write(capture)
    r = subprocess.run([sys.executable, DECODER, logf, "--prefix", tmpdir,
                        "--quiet"], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    # A payload spanning several base64 lines so a dropped line is mid-frame.
    raw = MAGIC + bytes((i * 7) & 0xFF for i in range(200))
    fails = 0

    with tempfile.TemporaryDirectory() as td:
        # 1. Well-framed frame -> success.
        rc, out = run(make_capture(raw, "/syn/a.gcda"), td)
        if rc != 0:
            print(f"FAIL: well-framed capture rejected (rc={rc})\n{out}")
            fails += 1
        elif not os.path.exists(os.path.join(td, "syn/a.gcda")):
            print("FAIL: well-framed capture wrote no .gcda")
            fails += 1
        else:
            print("PASS: well-framed frame decodes")

        # 2. A dropped UART line -> rejected (length mismatch), no valid output.
        rc, out = run(make_capture(raw, "/syn/b.gcda", drop_line=1), td)
        if rc == 0:
            print(f"FAIL: lossy capture (dropped line) accepted\n{out}")
            fails += 1
        elif "length mismatch" not in out:
            print(f"FAIL: lossy capture not flagged as length mismatch\n{out}")
            fails += 1
        else:
            print("PASS: dropped-line frame rejected (length mismatch)")

        # 3. A wrong count in the END marker -> rejected too.
        rc, out = run(make_capture(raw, "/syn/c.gcda", end_len=len(raw) + 4), td)
        if rc == 0:
            print(f"FAIL: wrong END length accepted\n{out}")
            fails += 1
        else:
            print("PASS: wrong END length rejected")

    if fails:
        print(f"gcov-decode-test: {fails} failure(s)")
        return 1
    print("gcov-decode-test: all PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

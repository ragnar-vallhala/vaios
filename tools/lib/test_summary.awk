# =============================================================================
# tools/lib/test_summary.awk
#
# Shared summary-table renderer for the vaios test runners.
#
# Reads a stream of TEST_RUN output in the host-test format (the same format
# tests/framework.h emits and tools/run_hw_tests.sh now mirrors):
#
#   === Suite: NAME ===
#     test_xxx                                              PASS (N)
#     test_yyy                                              FAIL (P/T)
#
# Produces a wide, banner-style table grouped by suite, with a GRAND TOTAL
# row across every suite the stream contained.
#
# Caller-tunable variables (pass with `awk -v`):
#   title    — banner text (default "TEST SUMMARY")
#
# ANSI colour: failing rows tint red, otherwise green. The caller is expected
# to have already stripped raw ANSI escapes from the stream (sed -E
# 's/\x1B\[[0-9;]*[A-Za-z]//g') so awk only sees plain text.
#
# Gotchas worked through and worth keeping:
#   - `\(` inside an [[:space:]]+.*PASS \( pattern doesn't reliably match a
#     literal '(' in gawk's ERE; use the [(] character class instead.
#   - Uninitialised variables compare to "" by default — s_name[ns] on the
#     first store would index the array with "" instead of 0, silently
#     dropping the first suite. The BEGIN block initialises everything.
#   - The `(P/T)` form on FAIL lines means "P passed of T total" — assert
#     count for that row is T (the second number).
# =============================================================================

BEGIN {
    # Column widths — wide enough to leave breathing room.
    COL_SUITE_MIN = 44
    COL_TESTS    = 9
    COL_PASS     = 9
    COL_FAIL     = 9
    COL_ASSERTS  = 11
    GUTTER       = "    "   # 4 spaces between columns

    # State accumulators.
    ns = 0; maxlen = 0
    tot_pass = 0; tot_fail = 0; tot_tests = 0; tot_asserts = 0
    suite = ""; pass = 0; fail = 0; asserts = 0

    # Banner text — overridable via `awk -v title="..."`.
    if (title == "") title = "TEST SUMMARY"
}

function flush() {
    if (suite != "") {
        s_name[ns]    = suite
        s_pass[ns]    = pass
        s_fail[ns]    = fail
        s_total[ns]   = pass + fail
        s_asserts[ns] = asserts
        ns++
        tot_pass    += pass
        tot_fail    += fail
        tot_tests   += pass + fail
        tot_asserts += asserts
        if (length(suite) > maxlen) maxlen = length(suite)
    }
}

# Extract the assertion count from a TEST_RUN annotation.
#   PASS (3)      → 3        (the single number)
#   FAIL (1/4)    → 4        (the TOTAL — second number)
# POSIX match()+RSTART/RLENGTH so we don't depend on gawk's match(line, re, arr).
function asserts_in(line,    s) {
    if (match(line, /[(][0-9]+\/[0-9]+[)]/)) {
        s = substr(line, RSTART + 1, RLENGTH - 2)
        sub(/.*\//, "", s)
        return s + 0
    }
    if (match(line, /[(][0-9]+[)]/)) {
        s = substr(line, RSTART + 1, RLENGTH - 2)
        return s + 0
    }
    return 0
}

function repeat(ch, n,    out, i) {
    out = ""
    for (i = 0; i < n; i++) out = out ch
    return out
}

function pad_center(text, width,    pad, left, right) {
    pad = width - length(text)
    if (pad <= 0) return text
    left  = int(pad / 2)
    right = pad - left
    return repeat(" ", left) text repeat(" ", right)
}

/^=== Suite:/ {
    flush()
    suite = $0
    sub(/^=== Suite: /, "", suite)
    sub(/ ===$/, "", suite)
    pass = 0; fail = 0; asserts = 0
}
/^[[:space:]]+.*PASS [(]/ { pass++; asserts += asserts_in($0) }
/^[[:space:]]+.*FAIL [(]/ { fail++; asserts += asserts_in($0) }

END {
    flush()

    # Resolve column widths now that all suite names are seen.
    suite_w = (maxlen > COL_SUITE_MIN) ? maxlen : COL_SUITE_MIN

    # Total row width = "  " gutter + suite_w + 4×(gutter + numeric_col).
    row_w = 2 + suite_w + length(GUTTER) + COL_TESTS \
              + length(GUTTER) + COL_PASS \
              + length(GUTTER) + COL_FAIL \
              + length(GUTTER) + COL_ASSERTS

    GREEN = "\033[32m"; RED = "\033[31m"; BOLD = "\033[1m"; RST = "\033[0m"
    bar_eq  = repeat("=", row_w)
    bar_das = repeat("-", row_w)

    # Header banner.
    printf "\n" BOLD "%s" RST "\n", bar_eq
    printf BOLD "%s" RST "\n", pad_center(title, row_w)
    printf BOLD "%s" RST "\n", bar_eq

    # Column headers.
    hdr_fmt = sprintf("  %%-%ds%s%%%ds%s%%%ds%s%%%ds%s%%%ds\n", \
                     suite_w, GUTTER, COL_TESTS, GUTTER, COL_PASS, \
                     GUTTER, COL_FAIL, GUTTER, COL_ASSERTS)
    printf BOLD hdr_fmt RST, "Suite", "Tests", "Pass", "Fail", "Asserts"
    printf "%s\n", bar_das

    # Suite rows.
    for (i = 0; i < ns; i++) {
        fcol = (s_fail[i] > 0) ? RED : GREEN
        printf "  %-*s" GUTTER "%" COL_TESTS "d" \
               GUTTER fcol "%" COL_PASS "d" RST \
               GUTTER fcol "%" COL_FAIL "d" RST \
               GUTTER "%" COL_ASSERTS "d\n", \
               suite_w, s_name[i], \
               s_total[i], s_pass[i], s_fail[i], s_asserts[i]
    }

    # Grand total row.
    printf "%s\n", bar_das
    gcol = (tot_fail > 0) ? RED : GREEN
    printf "  " BOLD "%-*s" RST GUTTER BOLD "%" COL_TESTS "d" RST \
           GUTTER gcol BOLD "%" COL_PASS "d" RST \
           GUTTER gcol BOLD "%" COL_FAIL "d" RST \
           GUTTER BOLD "%" COL_ASSERTS "d" RST "\n", \
           suite_w, "GRAND TOTAL", \
           tot_tests, tot_pass, tot_fail, tot_asserts
    printf BOLD "%s" RST "\n", bar_eq
}

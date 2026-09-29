#!/usr/bin/env bash
# =============================================================================
# tools/run_codeql.sh — run the CodeQL analysis locally, exactly as CI runs it.
#
#   tools/run_codeql.sh [host|arm|all]     default: all
#
# Why this exists: the alerts only appear in GitHub's Security tab after a push,
# which is a slow loop for triaging a backlog or for checking that a fix
# actually cleared a finding. This builds the same two databases .github/
# workflows/codeql.yml builds and runs the same query suite, so a local result
# and a CI result should agree.
#
# The two targets are the two CI matrix entries, and they are NOT
# interchangeable: CodeQL for C/C++ only sees translation units the build
# actually compiles.
#   host  — cmake -S tests, gcc, NavHAL stubbed: the kernel/ core.
#   arm   — NAVHAL=ON, MPU_USER_DEMO, arm-none-eabi: adds portable/armv7e-m/*.c,
#           which the host build stubs out, so syscall.c's dispatch and the
#           pointer validation are only analysed here.
#
# Environment:
#   CODEQL=<path>    the codeql executable; default $HOME/codeql/codeql/codeql,
#                    then whatever is on PATH.
#   CODEQL_OUT=<dir> databases + SARIF; default build_codeql_local/ (gitignored).
#
# Two things the local run does NOT know about, both worth remembering before
# reading anything into a diff against the Security tab:
#
#   1. Dismissals are server-side. An alert dismissed on GitHub (the
#      stack-address-escape family, say) still appears here. Local output is the
#      raw finding set, not the triaged one.
#   2. Submodule findings are included. CI cannot exclude extern/NavHAL either
#      (see the paths-ignore caveat in codeql.yml) and dismisses them by hand;
#      here they are simply printed, tagged [submodule] in the summary.
#
# Exit: 0 if the analysis ran, 1 if a build or the analysis itself failed. It
# does NOT fail on findings — this is a triage tool, and the gate is CI.
# =============================================================================
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

OUT="${CODEQL_OUT:-$ROOT_DIR/build_codeql_local}"
WHICH="${1:-all}"

# The bundle ships the CLI with its query packs precompiled; a bare CLI would
# have to resolve codeql/cpp-queries over the network on first use.
CODEQL="${CODEQL:-$HOME/codeql/codeql/codeql}"
[ -x "$CODEQL" ] || CODEQL="$(command -v codeql 2>/dev/null)"
if [ -z "${CODEQL:-}" ] || [ ! -x "$CODEQL" ]; then
  cat >&2 <<'EOF'
codeql CLI not found.

Install the bundle (CLI + precompiled query packs) once:

  gh release download codeql-bundle-v2.27.1 --repo github/codeql-action \
     --pattern codeql-bundle-linux64.tar.gz --dir "$HOME"
  tar -xzf "$HOME/codeql-bundle-linux64.tar.gz" -C "$HOME/codeql"

or set CODEQL=<path to the codeql executable>.
EOF
  exit 1
fi

SUITE="codeql/cpp-queries:codeql-suites/cpp-security-and-quality.qls"
mkdir -p "$OUT"
rc=0

# analyse <name> <build-dir> <build-command...>
analyse() {
  local name="$1" build_dir="$2"; shift 2
  local db="$OUT/db-$name" sarif="$OUT/$name.sarif"

  echo "=== $name: building + tracing ==="
  # A traced build must be a FULL build: CodeQL only records what it watches
  # compile, so an incremental build yields a database with almost nothing in
  # it and an analysis that cheerfully reports no problems.
  rm -rf "$db" "$build_dir"
  # The build goes through a script file rather than an inline --command:
  # CodeQL does not run --command through a shell, so `&&`, `export` and
  # redirection inside one are not shell syntax to it. A script keeps the
  # multi-step builds honest instead of silently tracing only their first word.
  local runner="$OUT/$name.build.sh"
  { echo '#!/usr/bin/env bash'; echo 'set -euo pipefail'; echo "cd $(printf '%q' "$ROOT_DIR")"; echo "$*"; } >"$runner"
  chmod +x "$runner"
  if ! "$CODEQL" database create "$db" --language=c-cpp --overwrite \
        --command="$runner" >"$OUT/$name.build.log" 2>&1; then
    echo "  FAIL: database create (see $OUT/$name.build.log)" >&2
    tail -15 "$OUT/$name.build.log" >&2
    rc=1; return
  fi

  echo "=== $name: analysing (security-and-quality) ==="
  if ! "$CODEQL" database analyze "$db" "$SUITE" \
        --format=sarif-latest --output="$sarif" --threads=0 \
        >"$OUT/$name.analyze.log" 2>&1; then
    echo "  FAIL: analyze (see $OUT/$name.analyze.log)" >&2
    tail -15 "$OUT/$name.analyze.log" >&2
    rc=1; return
  fi
  summarise "$name" "$sarif"
}

summarise() {
  python3 - "$1" "$2" <<'PY'
import json, sys, collections
name, path = sys.argv[1], sys.argv[2]
run = json.load(open(path))["runs"][0]
# SARIF puts the human-readable rule text in the driver's rules table, not on
# each result, so build the id -> description map once.
rules = {r["id"]: r.get("shortDescription", {}).get("text", "") for r in
         run.get("tool", {}).get("driver", {}).get("rules", [])}
rows = []
for res in run.get("results", []):
    rid = res.get("ruleId", "?")
    for loc in res.get("locations", [])[:1]:
        pl = loc.get("physicalLocation", {})
        f = pl.get("artifactLocation", {}).get("uri", "?")
        line = pl.get("region", {}).get("startLine", 0)
        rows.append((rid, f, line))
print(f"\n---- {name}: {len(rows)} finding(s) ----")
for rid, n in collections.Counter(r[0] for r in rows).most_common():
    print(f"  {n:3d}  {rid}    {rules.get(rid,'')}")
if rows:
    print()
    for rid, f, line in sorted(rows, key=lambda r: (r[0], r[1], r[2])):
        tag = " [submodule]" if f.startswith("extern/") else ""
        print(f"  {rid}\t{f}:{line}{tag}")
PY
}

if [ "$WHICH" = host ] || [ "$WHICH" = all ]; then
  analyse host "$ROOT_DIR/build_codeql" \
    "cmake -S tests -B build_codeql -DCMAKE_C_COMPILER=gcc \
       -DCMAKE_BUILD_TYPE=Debug -DCMAKE_SYSTEM_NAME=Linux \
       -DVAIOS_TEST_SANITIZE=OFF && cmake --build build_codeql --parallel"
fi

if [ "$WHICH" = arm ] || [ "$WHICH" = all ]; then
  if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "SKIP arm: arm-none-eabi-gcc not installed"
  else
    analyse arm "$ROOT_DIR/build_codeql_arm" \
      "export srctree=$ROOT_DIR/extern/NavHAL && \
       cmake -S . -B build_codeql_arm -DNAVHAL=ON -DEXAMPLES=ON \
         -DVAIOS_EXAMPLE=MPU_USER_DEMO && \
       cmake --build build_codeql_arm --parallel"
  fi
fi

echo
echo "SARIF + databases under $OUT"
exit "$rc"

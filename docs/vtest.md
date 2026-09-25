# Running vaios's suites with an installed vtest

vtest can now be installed once per machine instead of compiled by every repo.
This page is the change `tools/vtest.sh` needs to use it: **an installed `vtest`
if there is one, otherwise build the pinned `extern/vtest` exactly as today.**
Nothing else changes -- `vtest.conf` already loads under the current vtest.

## Install vtest (once per machine)

```sh
git clone https://github.com/ragnar-vallhala/vtest.git ~/src/vtest   # or use extern/vtest
cd ~/src/vtest
cmake -S . -B build && cmake --build build && cmake --install build --prefix ~/.local
vtest --version          # e.g. vtest v1.3.0-7-g7f22748
```

This puts `vtest` and `vtest-loc` (vtest's `loc.sh`) in `~/.local/bin`, which
must be on `PATH`. Re-run the same three commands after pulling vtest.

## `tools/vtest.sh`

Replace the build block with the lookup below. The `srctree` export stays --
NAVHAL builds inside the suites need it no matter which vtest runs them.

```sh
set -eu
cd "$(dirname "$0")/.."

# An installed vtest if there is one (VTEST=path forces a specific binary);
# otherwise build the pinned submodule, only when its source is newer.
if [ -n "${VTEST:-}" ]; then
  :
elif command -v vtest >/dev/null 2>&1; then
  VTEST=vtest
  VTEST_LOC=vtest-loc
else
  if [ ! -f extern/vtest/vtest.c ]; then
    echo "no vtest installed and extern/vtest is empty -- install vtest" \
         "(see docs/vtest.md) or run: git submodule update --init extern/vtest" >&2
    exit 2
  fi
  VTEST=build/vtest
  if [ ! -x "$VTEST" ] || [ extern/vtest/vtest.c -nt "$VTEST" ]; then
    mkdir -p build
    ${CC:-cc} -std=c11 -O2 -Wall -Wextra extern/vtest/vtest.c -o "$VTEST"
  fi
  VTEST_LOC=extern/vtest/loc.sh
fi
export VTEST_LOC="${VTEST_LOC:-vtest-loc}"
export srctree="${srctree:-$PWD/extern/NavHAL}"
exec "$VTEST" "$@"
```

## `vtest.conf`: the `[loc]` check

`[loc]` calls `extern/vtest/loc.sh`, which is missing when the submodule was
never initialised -- exactly the case an installed vtest is for. Take the path
from the launcher instead, falling back to the installed name when `vtest` is
run directly:

```ini
[loc]
adapter = check
cmd     = ${VTEST_LOC:-vtest-loc} kernel include portable tests tools examples
```

## Which vtest am I running?

```sh
command -v vtest && vtest --version    # the installed one
VTEST=build/vtest tools/vtest.sh ...   # force the submodule build
```

CI has no installed vtest, so it takes the submodule branch: the `extern/vtest`
pin stays the version CI tests against. A local installed vtest newer than the
pin behaves differently in these ways, all deliberate:

- A suite whose runner crashes, or exits non-zero while every parsed case
  passed, is **FAILED** (older vtest could print ALL PASSED).
- Ctrl-C (and a CI cancel) kills the running suite -- Renode and QEMU included
  -- instead of leaving it running after vtest exits.
- An unknown key or adapter in `vtest.conf` is an error, so an older vtest
  reading a newer conf says so instead of guessing.

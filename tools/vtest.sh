#!/usr/bin/env sh
# Copyright (C) 2026 NAVRobotec Pvt Ltd
# Author: Ragnar Vallhala
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Build and launch the vtest orchestrator (extern/vtest) over vaios's suites,
# which are declared in ./vtest.conf.
#
#   tools/vtest.sh            interactive TUI
#   tools/vtest.sh --run      everything, batch (host + gates + QEMU + SITL)
#   tools/vtest.sh --list     the catalog, without running anything
#
# Rebuilds only when the source is newer than the binary. NAVHAL builds inside
# the suites need $srctree, so export it here rather than in every suite.
set -eu
cd "$(dirname "$0")/.."
BIN=build/vtest

if [ ! -f extern/vtest/vtest.c ]; then
  echo "extern/vtest is empty — run: git submodule update --init extern/vtest" >&2
  exit 2
fi
if [ ! -x "$BIN" ] || [ extern/vtest/vtest.c -nt "$BIN" ]; then
  mkdir -p build
  ${CC:-cc} -std=c11 -O2 -Wall -Wextra extern/vtest/vtest.c -o "$BIN"
fi
export srctree="${srctree:-$PWD/extern/NavHAL}"
exec "$BIN" "$@"

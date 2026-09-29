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

# Launch the installed vtest orchestrator over vaios's suites, which are declared
# in ./vtest.conf.
#
#   tools/vtest.sh            interactive TUI
#   tools/vtest.sh --run      everything, batch (host + gates + QEMU + SITL)
#   tools/vtest.sh --list     the catalog, without running anything
#
# vtest is a TOOL, not a dependency of vaios: this used to carry it as the
# extern/vtest submodule and compile vtest.c into build/vtest on every run, which
# meant vaios pinned a version of its own test runner and rebuilt it to look at
# its own test list. It is installed like cmake or renode now, and a repo that
# does not have it says so rather than building one.
#
# Skips rather than fails when it is absent, matching every other optional tool
# here: CI does not run vtest at all (it runs the underlying scripts directly),
# so a machine without it is a machine that simply cannot use this entry point.
#
# NAVHAL builds inside the suites need $srctree, so export it here rather than in
# every suite.
set -eu
cd "$(dirname "$0")/.."

if ! command -v vtest >/dev/null 2>&1; then
  echo "SKIP: 'vtest' not installed."
  echo "  Install it from https://github.com/ragnar-vallhala/vtest (it puts"
  echo "  vtest and vtest-loc on PATH), or run the suites directly — vtest.conf"
  echo "  names the script behind each one, and that is what CI invokes."
  exit 0
fi

export srctree="${srctree:-$PWD/extern/NavHAL}"
exec vtest "$@"

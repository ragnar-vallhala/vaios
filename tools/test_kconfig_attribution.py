#!/usr/bin/env python3
"""Host test: which Kconfig symbols tools/kconfig.py puts in vaios_autoconf.h.

When NavHAL's or a consumer app's Kconfig is sourced into vaios's tree, their
symbols must stay out of vaios's header (NavHAL has navhal_target.h, the app its
own), while a vaios symbol the app merely extends must stay in. A file belongs
to the innermost of vaios's dir, NavHAL's dir and the app's dir, and the trees
nest: NavHAL under vaios/extern, vaios under <app>/.nav/deps. This builds that
nesting in a temp dir (a copy of vaios's Kconfig files and kconfig.py, so the
script's own root is the nested one) with stub NavHAL and app Kconfigs, and
checks the header, plus the same with NavHAL reached through a symlink.

Needs kconfiglib (the same Python the build uses). No NavHAL checkout needed.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KCONFIGS = ["Kconfig", "examples/Kconfig",
            "portable/armv7e-m/Kconfig", "portable/host/Kconfig"]

# A vaios symbol (defined in vaios's Kconfig) that the app extends.
EXTENDED = "VAIOS_MODULE_SHELL"
APP_ONLY = "MYAPP_ONLY_FEATURE"
NAVHAL_ONLY = "NAVHAL_ONLY_FEATURE"

APP_PLAIN = f"config {APP_ONLY}\n\tbool \"app only\"\n\tdefault y\n"
# Same, and also adds a second definition site to a vaios symbol.
APP_EXTENDS = APP_PLAIN + f"\nconfig {EXTENDED}\n\tdefault y if {APP_ONLY}\n"
NAVHAL_STUB = f"config {NAVHAL_ONLY}\n\tbool \"navhal only\"\n\tdefault y\n"

failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def make_nested_vaios(dest):
    for rel in KCONFIGS:
        os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
        shutil.copy(os.path.join(ROOT, rel), os.path.join(dest, rel))
    os.makedirs(os.path.join(dest, "tools"))
    shutil.copy(os.path.join(ROOT, "tools", "kconfig.py"),
                os.path.join(dest, "tools", "kconfig.py"))


def run(tmp, vaios, name, navhal_dir=None, app_kconfig=None):
    """Run the nested kconfig.py the way the build does; return the header text."""
    env = dict(os.environ)
    for var in ("NAVHAL_KCONFIG", "APP_KCONFIG", "srctree"):
        env.pop(var, None)
    if navhal_dir:
        env["NAVHAL_KCONFIG"] = os.path.join(navhal_dir, "Kconfig")
        env["srctree"] = navhal_dir
    if app_kconfig:
        env["APP_KCONFIG"] = app_kconfig
    header = os.path.join(tmp, name + ".h")
    subprocess.run(
        [sys.executable, os.path.join(vaios, "tools", "kconfig.py"),
         "--config", os.path.join(tmp, name + ".config"),
         "--header-out", header],
        env=env, check=True, stdout=subprocess.DEVNULL)
    with open(header) as f:
        return f.read()


def defines(text, sym):
    return f"#define {sym} " in text


def main():
    if shutil.which(sys.executable) is None:
        print("no python?")
        return 2
    try:
        import kconfiglib  # noqa: F401
    except ImportError:
        print("kconfiglib is not installed; install it (pip install kconfiglib)")
        return 2

    with tempfile.TemporaryDirectory(prefix="vaios_kconfig_attr_") as tmp:
        app = os.path.join(tmp, "app")
        vaios = os.path.join(app, ".nav", "deps", "vaios")
        navhal = os.path.join(vaios, "extern", "NavHAL")
        make_nested_vaios(vaios)
        write(os.path.join(navhal, "Kconfig"), NAVHAL_STUB)
        write(os.path.join(app, "Kconfig"), APP_EXTENDS)
        plain = os.path.join(tmp, "app_plain", "Kconfig")
        write(plain, APP_PLAIN)

        print("nested: app/.nav/deps/vaios/extern/NavHAL")
        base = run(tmp, vaios, "base", navhal_dir=navhal)
        check(defines(base, EXTENDED), f"no app: vaios symbol {EXTENDED} is emitted")
        check(not defines(base, NAVHAL_ONLY), "no app: NavHAL symbol is not emitted")

        withapp = run(tmp, vaios, "with_app", navhal_dir=navhal,
                      app_kconfig=os.path.join(app, "Kconfig"))
        check(not defines(withapp, APP_ONLY), "app-only symbol is not emitted")
        check(defines(withapp, EXTENDED),
              f"a vaios symbol the app extends ({EXTENDED}) is still emitted")
        check(not defines(withapp, NAVHAL_ONLY), "NavHAL symbol is not emitted")

        # An app that only adds its own symbols must not change vaios's output.
        plainapp = run(tmp, vaios, "plain_app", navhal_dir=navhal,
                       app_kconfig=plain)
        check(plainapp == base,
              "an app that adds only its own symbols leaves the header byte-identical")

        print("standalone (no NavHAL):")
        solo = run(tmp, vaios, "solo")
        soloapp = run(tmp, vaios, "solo_app", app_kconfig=plain)
        check(defines(solo, EXTENDED), "no NavHAL, no app: vaios symbols are emitted")
        check(soloapp == solo,
              "no NavHAL, plain app: header byte-identical to no app")

        print("NavHAL reached through a symlink:")
        real = os.path.join(tmp, "elsewhere", "NavHAL")
        write(os.path.join(real, "Kconfig"), NAVHAL_STUB)
        shutil.rmtree(navhal)
        os.symlink(real, navhal)
        linked = run(tmp, vaios, "linked", navhal_dir=navhal,
                     app_kconfig=os.path.join(app, "Kconfig"))
        check(not defines(linked, NAVHAL_ONLY),
              "symlinked NavHAL symbol is not emitted")
        check(not defines(linked, APP_ONLY), "app-only symbol is not emitted")
        check(defines(linked, EXTENDED), f"{EXTENDED} is still emitted")

    print("kconfig attribution: " +
          ("FAILED (%d)" % len(failures) if failures else "all checks passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

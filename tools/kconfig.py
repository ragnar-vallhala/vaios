#!/usr/bin/env python3
"""
tools/kconfig.py — vaios Kconfig -> C header + CMake generator.

Adapted from NavHAL's tools/kconfig.py (same kconfiglib backend). Emits:

  --header-out   vaios_autoconf.h : one `#define <SYM> <val>` per Kconfig
                 symbol, UNPREFIXED so it matches the macros the kernel already
                 uses (HEAP_SIZE, MAX_TASK_PRIORITY, VAIOS_MODULE_PERF, ...).
                 Bools emit as explicit 0/1 (not undefined) so `#if SYM == 1`
                 style guards keep working; force-included into every TU.
  --cmake-out    config.cmake : `set(CONFIG_<SYM> ...)` cache vars, for module
                 gating / source selection in CMake.
  --menuconfig   interactive configuration.

The single .config this reads may be a UNIFIED config that also carries NavHAL's
CONFIG_DRV_* symbols; kconfiglib's load_config ignores symbols not defined in
vaios's Kconfig tree, so the extra NavHAL entries are harmless here (NavHAL's
own run consumes them). See docs/plan/KCONFIG_CONFIG_SYSTEM_PLAN.md.
"""
import os
import sys
import argparse

try:
    import kconfiglib
except ImportError:
    sys.stderr.write(
        "FATAL: python module 'kconfiglib' is required for vaios config "
        "generation.\n       Install it with: python3 -m pip install kconfiglib\n"
    )
    sys.exit(1)


def parse_args():
    p = argparse.ArgumentParser(description="vaios Kconfig -> header/CMake generator")
    p.add_argument("--kconfig", default="Kconfig", help="Path to Kconfig file")
    p.add_argument("--config", default=".config", help="Path to .config file")
    p.add_argument("--header-out", default=None, help="Output vaios_autoconf.h")
    p.add_argument("--cmake-out", default=None, help="Output config.cmake")
    p.add_argument("--menuconfig", action="store_true", help="Interactive menuconfig")
    return p.parse_args()


def run_menuconfig(kconfig_file, config_file):
    try:
        from menuconfig import menuconfig
        os.environ["KCONFIG_CONFIG"] = config_file
        kb = kconfiglib.Kconfig(kconfig_file)
        menuconfig(kb)
        print("Configuration saved to", config_file)
    except ImportError:
        print("Error: 'kconfiglib' is required for menuconfig.", file=sys.stderr)
        sys.exit(1)


def load_kconfig(kconfig_path, config_path):
    os.environ["KCONFIG_CONFIG"] = config_path
    # The top Kconfig does `osource "$(NAVHAL_KCONFIG)"`. When NAVHAL is off the
    # build leaves it unset; point it at an empty source so osource is a genuine
    # no-op (osource "" would expand to $srctree/, a directory -> EISDIR).
    os.environ.setdefault("NAVHAL_KCONFIG", os.devnull)
    kb = kconfiglib.Kconfig(kconfig_path, warn=False)
    if os.path.exists(config_path):
        kb.load_config(config_path)  # ignores symbols not in this tree
        print(f"Loaded configuration from '{config_path}'")
    else:
        print(f"No .config at '{config_path}'; using Kconfig defaults.")
    return kb


def _navhal_root():
    """Absolute dir of the sourced NavHAL Kconfig, or None when not sourcing it."""
    kcfg = os.environ.get("NAVHAL_KCONFIG", "")
    if not kcfg or kcfg == os.devnull:
        return None
    return os.path.abspath(os.path.dirname(kcfg))


def _is_vaios_sym(sym, navhal_root, srctree):
    """True if the symbol has any definition outside the NavHAL subtree. When
    NAVHAL is sourced, kb.unique_defined_syms also holds NavHAL's ~100 DRV_*/
    ARCH_* etc.; those belong in navhal_target.h (NavHAL's own run emits them
    from the same unified .config), NOT unprefixed in vaios's outputs. NavHAL
    symbols carry $srctree-relative filenames (e.g. 'src/arch/...'), vaios's an
    absolute path — resolve both before comparing."""
    if navhal_root is None:
        return True
    for node in sym.nodes:
        fn = node.filename or ""
        absfn = fn if os.path.isabs(fn) else os.path.abspath(os.path.join(srctree, fn))
        if not absfn.startswith(navhal_root):
            return True  # a non-NavHAL (vaios) definition site
    return False


def _emit_define(f, name, sym):
    """Write one `#ifndef/#define/#endif` for <name> in the kernel's own
    spelling. The #ifndef guard yields to a command-line -D (CMake still owns
    the module macros during migration) and to vaios_app_config.h overrides,
    giving precedence: -D  >  Kconfig(this header)  >  static default header.
    Bools emit explicit 0/1 (not undefined) so `#if SYM == 1` guards work."""
    val = sym.str_value
    if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
        out = f"{1 if val == 'y' else 0}"
    elif sym.type == kconfiglib.STRING:
        out = f'"{val}"'
    else:  # INT / HEX — emit verbatim (hex keeps its 0x form)
        if val == "":
            return
        out = val
    f.write(f"#ifndef {name}\n#define {name} {out}\n#endif\n")


def generate_header(kb, output_path):
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "w") as f:
        f.write("/* Automatically generated by tools/kconfig.py. DO NOT EDIT. */\n")
        f.write("/* Source: vaios Kconfig + .config */\n\n")
        f.write("#ifndef VAIOS_AUTOCONF_H\n#define VAIOS_AUTOCONF_H\n\n")
        nav, src = _navhal_root(), os.environ.get("srctree", "")
        for sym in kb.unique_defined_syms:
            if sym.name and _is_vaios_sym(sym, nav, src):
                _emit_define(f, sym.name, sym)
        f.write("\n#endif /* VAIOS_AUTOCONF_H */\n")


def generate_cmake(kb, output_path):
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "w") as f:
        f.write("# Automatically generated by tools/kconfig.py. DO NOT EDIT.\n\n")
        nav, src = _navhal_root(), os.environ.get("srctree", "")
        for sym in kb.unique_defined_syms:
            if not sym.name or not _is_vaios_sym(sym, nav, src):
                continue
            key = f"CONFIG_{sym.name}"
            val = sym.str_value
            if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
                f.write(f'set({key} {"ON" if val == "y" else "OFF"} CACHE INTERNAL "")\n')
            elif sym.type == kconfiglib.STRING:
                f.write(f'set({key} "{val}" CACHE INTERNAL "")\n')
            elif val != "":
                f.write(f'set({key} {val} CACHE INTERNAL "")\n')


def main():
    args = parse_args()
    if args.menuconfig:
        run_menuconfig(args.kconfig, args.config)
        if not args.header_out and not args.cmake_out:
            return
    if not args.header_out and not args.cmake_out:
        print("No output paths specified.")
        return
    kb = load_kconfig(args.kconfig, args.config)
    if args.header_out:
        print(f"Generating C header -> '{args.header_out}'")
        generate_header(kb, args.header_out)
    if args.cmake_out:
        print(f"Generating CMake config -> '{args.cmake_out}'")
        generate_cmake(kb, args.cmake_out)
    print("vaios Kconfig generation complete.")


if __name__ == "__main__":
    main()

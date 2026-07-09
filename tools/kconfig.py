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

# vaios's own top Kconfig, absolute. Used as the --kconfig default so a bare
# `kconfig.py --menuconfig` opens the vaios tree even when $srctree is exported
# for a NAVHAL build — kconfiglib resolves a *relative* top Kconfig against
# $srctree, which would otherwise open extern/NavHAL/Kconfig instead.
_VAIOS_KCONFIG = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Kconfig")


def parse_args():
    p = argparse.ArgumentParser(description="vaios Kconfig -> header/CMake generator")
    p.add_argument("--kconfig", default=_VAIOS_KCONFIG,
                   help="Path to the top Kconfig (default: the repo's vaios Kconfig)")
    # Required on purpose: a bare default (".config") silently wrote a repo-root
    # config that the build never reads. The build always passes an explicit
    # path (VAIOS_CONFIG); use the cmake `menuconfig` target rather than running
    # this by hand.
    p.add_argument("--config", required=True, help="Path to the .config file")
    p.add_argument("--header-out", default=None, help="Output vaios_autoconf.h")
    p.add_argument("--cmake-out", default=None, help="Output config.cmake")
    p.add_argument("--menuconfig", action="store_true", help="Interactive menuconfig")
    p.add_argument("--example-override", default=None,
                   help="Activate examples/Kconfig's EXAMPLE_<TOKEN> entry (fires "
                        "its selects) after loading .config. No-op if the example "
                        "has no entry. Mirrors NavHAL's --sample-override.")
    return p.parse_args()


# TUI colour theme (kconfiglib MENUCONFIG_STYLE syntax, layered on "default").
# Elements that should sit on the terminal's own background OMIT bg: entirely
# (an unset bg is curses -1 = the terminal default / transparent; "default" is
# NOT a valid colour name and warns). fg white + bold = high-contrast text.
# Bars/frames are a dark navy and the highlighted row a dark cyan (#RRGGBB is
# snapped to the nearest terminal colour); white text stays on every bar.
_DARK_BLUE = "#002347"   # headline / frame / input-field bars
_DARK_CYAN = "#004d4d"   # highlighted (selected) row
_MENUCONFIG_STYLE = " ".join((
    "default",                                    # inherit unset elements
    f"path=fg:white,bg:{_DARK_BLUE},bold",        # breadcrumb headline bar
    f"separator=fg:white,bg:{_DARK_BLUE},bold",   # the headline bars
    f"frame=fg:white,bg:{_DARK_BLUE},bold",       # dialog title bars
    "list=fg:white",                              # menu body on terminal bg
    "body=fg:white",                              # dialog body on terminal bg
    "help=fg:white",                              # bottom help pane
    "show-help=fg:white",                         # full help dialog text
    "text=fg:white",                              # text-box contents
    f"selection=fg:white,bg:{_DARK_CYAN},bold",   # highlighted row, white text
    f"inv-selection=fg:red,bg:{_DARK_CYAN},bold",
    "inv-list=fg:red",
    f"edit=fg:white,bg:{_DARK_BLUE}",             # input fields
    f"jump-edit=fg:white,bg:{_DARK_BLUE}",
))


def run_menuconfig(kconfig_file, config_file):
    try:
        from menuconfig import menuconfig
        os.environ["KCONFIG_CONFIG"] = config_file
        # Same osource "$(NAVHAL_KCONFIG)" guard as load_kconfig(): unset
        # expands to $srctree/ (a directory -> EISDIR), so point it at an
        # empty source to make the osource a genuine no-op when NAVHAL is off.
        os.environ.setdefault("NAVHAL_KCONFIG", os.devnull)
        # Colour theme for the TUI (kconfiglib reads MENUCONFIG_STYLE): dark
        # navy headline bars over the terminal's own background, dark-cyan
        # selection, bright high-contrast text. setdefault so a user-exported
        # MENUCONFIG_STYLE still wins.
        os.environ.setdefault("MENUCONFIG_STYLE", _MENUCONFIG_STYLE)
        # menuconfig loads its starting values from this file (KCONFIG_CONFIG);
        # if it is missing, EVERY symbol opens at its default with no user
        # value -> shown as "(NEW)", and a save writes a fresh file here that
        # is disconnected from the build. Warn loudly instead of surprising.
        if not os.path.exists(config_file):
            print(f"WARNING: '{config_file}' does not exist — menuconfig will "
                  f"start from Kconfig defaults (every option shows '(NEW)') "
                  f"and save a new file here.\n"
                  f"         To edit the build config instead, pass "
                  f"--config <build-dir>/.config (or run the CMake "
                  f"'menuconfig' target).", file=sys.stderr)
        kb = kconfiglib.Kconfig(kconfig_file)
        menuconfig(kb)
        print("Configuration saved to", config_file)
    except ImportError:
        print("Error: 'kconfiglib' is required for menuconfig.", file=sys.stderr)
        sys.exit(1)


def apply_example_override(kb, token):
    """Activate examples/Kconfig's EXAMPLE_<TOKEN> choice entry so its `select`s
    cascade into the effective config. Applied AFTER load_config and transiently
    (never written back to .config), so the base .config stays a clean defconfig
    expansion while the example's feature closure is layered on for this build
    only. Mirrors NavHAL's --sample-override.

    Examples that need no non-default features have no EXAMPLE_ entry -> no-op."""
    token = (token or "").strip()
    if not token:
        return
    name = "EXAMPLE_" + token.upper()
    sym = kb.syms.get(name)
    if sym is None:
        return  # example has no examples/Kconfig entry; base config applies as-is
    sym.set_value("y")  # choice member -> becomes the choice selection; selects fire
    print(f"Example override: {name}=y (selects cascaded)")


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
    apply_example_override(kb, args.example_override)
    if args.header_out:
        print(f"Generating C header -> '{args.header_out}'")
        generate_header(kb, args.header_out)
    if args.cmake_out:
        print(f"Generating CMake config -> '{args.cmake_out}'")
        generate_cmake(kb, args.cmake_out)
    print("vaios Kconfig generation complete.")


if __name__ == "__main__":
    main()

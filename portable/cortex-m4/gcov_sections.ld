/* =============================================================================
 * tools/gcov_sections.ld — linker fragment for -DVAIOS_GCOV=ON builds.
 *
 * The on-target gcov dumper (portable/cortex-m4/gcov_semihost.c) needs to run
 * the compiler-emitted per-TU coverage constructors to discover every gcov_info,
 * but the vaios/NavHAL startup never runs .init_array. This fragment bounds
 * .init_array with __init_array_start/end and keeps it in FLASH right after
 * .text — ahead of NavHAL's .data, whose `*(.init*)` glob would otherwise
 * swallow .init_array into RAM (uninitialised, since it is outside the .data
 * copy region).
 *
 * Merged alongside NavHAL's board linker.ld via a second -T (the tool prepends
 * this one so it is parsed first; INSERT keeps NavHAL's script authoritative and
 * requires no submodule edit). The FLASH region is redeclared here so the
 * fragment is self-contained and the section lands at a real load address —
 * this emits one harmless "redeclaration of memory region FLASH" linker warning,
 * which is expected and confined to the opt-in coverage build.
 * ========================================================================== */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 512K
}

SECTIONS
{
  .gcov_init_array : ALIGN(4)
  {
    PROVIDE(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE(__init_array_end = .);
  } > FLASH
} INSERT AFTER .text;

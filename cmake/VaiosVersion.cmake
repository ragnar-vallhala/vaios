# The vaios version, single-sourced from nav.toml's [project].version.
#
# nav requires the release tag (vMAJOR.MINOR.PATCH) to equal that field, so it is
# the one place a release bumps. This reads it and generates vaios_version.h
# (from include/vaios_version.h.in) so C code sees the same number. Sets
# VAIOS_VERSION / VAIOS_VERSION_{MAJOR,MINOR,PATCH} as cache-internal variables:
# visible to a superproject that add_subdirectory()s vaios, without a project()
# call (a subdirectory build does not make one).
#
#   vaios_version_generate(<repo root> <dir to write vaios_version.h into>)
function(vaios_version_generate root out_dir)
  file(STRINGS "${root}/nav.toml" _lines REGEX "^version[ \t]*=")
  list(GET _lines 0 _line)
  if(NOT _line MATCHES "^version[ \t]*=[ \t]*\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"")
    message(FATAL_ERROR "vaios: no MAJOR.MINOR.PATCH [project].version in ${root}/nav.toml")
  endif()
  set(VAIOS_VERSION_MAJOR ${CMAKE_MATCH_1})
  set(VAIOS_VERSION_MINOR ${CMAKE_MATCH_2})
  set(VAIOS_VERSION_PATCH ${CMAKE_MATCH_3})
  set(VAIOS_VERSION "${VAIOS_VERSION_MAJOR}.${VAIOS_VERSION_MINOR}.${VAIOS_VERSION_PATCH}")
  configure_file("${root}/include/vaios_version.h.in" "${out_dir}/vaios_version.h" @ONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/nav.toml")
  foreach(_v VERSION VERSION_MAJOR VERSION_MINOR VERSION_PATCH)
    set(VAIOS_${_v} "${VAIOS_${_v}}" CACHE INTERNAL "vaios ${_v} (nav.toml)")
  endforeach()
endfunction()

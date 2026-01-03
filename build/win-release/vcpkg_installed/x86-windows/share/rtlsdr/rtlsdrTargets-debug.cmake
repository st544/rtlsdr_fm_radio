#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "rtlsdr::rtlsdr" for configuration "Debug"
set_property(TARGET rtlsdr::rtlsdr APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(rtlsdr::rtlsdr PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/debug/lib/rtlsdr.lib"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/bin/rtlsdr.dll"
  )

list(APPEND _cmake_import_check_targets rtlsdr::rtlsdr )
list(APPEND _cmake_import_check_files_for_rtlsdr::rtlsdr "${_IMPORT_PREFIX}/debug/lib/rtlsdr.lib" "${_IMPORT_PREFIX}/debug/bin/rtlsdr.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)

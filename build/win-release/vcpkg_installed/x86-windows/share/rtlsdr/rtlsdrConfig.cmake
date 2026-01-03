if(MSVC)
  include(CMakeFindDependencyMacro)
  find_dependency(PThreads4W)
endif()

get_filename_component(RTLSDR_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

if(NOT TARGET rtlsdr::rtlsdr)
  include("${RTLSDR_CMAKE_DIR}/rtlsdrTargets.cmake")
endif()

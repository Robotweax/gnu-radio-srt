find_package(PkgConfig)

PKG_CHECK_MODULES(PC_GR_ROBOTWEAX_SRT gnuradio-robotweax_srt)

FIND_PATH(
    GR_ROBOTWEAX_SRT_INCLUDE_DIRS
    NAMES gnuradio/robotweax_srt/api.h
    HINTS $ENV{ROBOTWEAX_SRT_DIR}/include
        ${PC_ROBOTWEAX_SRT_INCLUDEDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/include
          /usr/local/include
          /usr/include
)

FIND_LIBRARY(
    GR_ROBOTWEAX_SRT_LIBRARIES
    NAMES gnuradio-robotweax_srt
    HINTS $ENV{ROBOTWEAX_SRT_DIR}/lib
        ${PC_ROBOTWEAX_SRT_LIBDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
          /usr/local/lib
          /usr/local/lib64
          /usr/lib
          /usr/lib64
          )

include("${CMAKE_CURRENT_LIST_DIR}/gnuradio-robotweax_srtTarget.cmake")

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(GR_ROBOTWEAX_SRT DEFAULT_MSG GR_ROBOTWEAX_SRT_LIBRARIES GR_ROBOTWEAX_SRT_INCLUDE_DIRS)
MARK_AS_ADVANCED(GR_ROBOTWEAX_SRT_LIBRARIES GR_ROBOTWEAX_SRT_INCLUDE_DIRS)

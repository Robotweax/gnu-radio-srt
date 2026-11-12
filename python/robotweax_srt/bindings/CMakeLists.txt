# Copyright 2020 Free Software Foundation, Inc.
#
# This file is part of GNU Radio
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

########################################################################
# Check if there is C++ code at all
########################################################################
if(NOT robotweax_srt_sources)
    message(STATUS "No C++ sources... skipping python bindings")
    return()
endif(NOT robotweax_srt_sources)

########################################################################
# Check for pygccxml
########################################################################
gr_python_check_module_raw("pygccxml" "import pygccxml" PYGCCXML_FOUND)

include(GrPybind)

########################################################################
# Python Bindings
########################################################################

list(APPEND robotweax_srt_python_files
    config_python.cc
    srt_sink_python.cc
    srt_source_python.cc python_bindings.cc)

gr_pybind_make_oot(robotweax_srt ../../.. gr::robotweax_srt "${robotweax_srt_python_files}")

# copy bindings extension for use in QA test module
add_custom_command(
    TARGET robotweax_srt_python
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:robotweax_srt_python>
            ${PROJECT_BINARY_DIR}/test_modules/gnuradio/robotweax_srt/)

install(
    TARGETS robotweax_srt_python
    DESTINATION ${GR_PYTHON_DIR}/gnuradio/robotweax_srt
    COMPONENT pythonapi)

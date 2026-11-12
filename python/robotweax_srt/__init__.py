#
# Copyright 2008,2009 Free Software Foundation, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The presence of this file turns this directory into a Python package

'''
GNU Radio blocks that carry sample streams over Robotweax SRT.

  srt_sink, srt_source          the stream blocks
  transport_config, stream_config   their configuration
  make_transport_config(**kw), make_stream_config(**kw)
                                keyword constructors that reject unknown names
'''
import os

# import pybind11 generated symbols into the robotweax_srt namespace
# A missing or unloadable binding is an installation error, never ignored.
from .robotweax_srt_python import *

# import any pure python here
from .helpers import make_transport_config, make_stream_config


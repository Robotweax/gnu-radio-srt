#
# Copyright 2008,2009 Free Software Foundation, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The presence of this file turns this directory into a Python package

'''
This is the GNU Radio ROBOTWEAX_SRT module. Place your Python package
description here (python/__init__.py).
'''
import os

# import pybind11 generated symbols into the robotweax_srt namespace
# A missing or unloadable binding is an installation error, never ignored.
from .robotweax_srt_python import *

# import any pure python here
#

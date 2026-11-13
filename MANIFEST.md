title: gr-robotweax_srt
brief: GNU Radio blocks carrying sample streams over Robotweax SRT
tags:
  - sdr
  - srt
  - network
author:
  - Robotweax
copyright_owner:
  - Robotweax
license: GPL-3.0-or-later
gr_supported_version: 3.10
repo: https://github.com/Robotweax/gnu-radio-srt
---
Out-of-tree module that carries continuous numeric sample streams over an
impaired IP network using the Robotweax SRT public C API, with bounded
buffering and explicit discontinuity reporting.

Status: release candidate 0.1.0-rc2. Qualified on Ubuntu 24.04 with GNU Radio
3.10.9.2 and Robotweax SRT 0.3.2. Network behaviour was measured in emulation on
one machine only; not run on a real network, between two hosts or with
acquisition hardware.

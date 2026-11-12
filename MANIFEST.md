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

Status: tested on loopback only; not yet qualified under network impairment.

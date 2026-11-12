#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Keyword constructors for the block configuration.

Used by GNU Radio Companion and convenient in scripts. Unknown names and
unknown enumeration values raise instead of being ignored, so a typing
mistake can never silently leave a default in place.
"""

from . import robotweax_srt_python as _m

_ENUMS = {
    "role": _m.connection_role,
    "security": _m.security_mode,
    "format": _m.sample_format,
    "continuity": _m.continuity_policy,
}


def _fill(config, values):
    for name, value in values.items():
        if not hasattr(config, name):
            raise TypeError("%s has no parameter %r" % (type(config).__name__, name))
        enum = _ENUMS.get(name)
        if enum is not None and isinstance(value, str):
            if value not in enum.__members__:
                raise ValueError("%s must be one of %s, not %r" % (name, sorted(enum.__members__), value))
            value = enum.__members__[value]
        setattr(config, name, value)
    return config


def make_transport_config(**values):
    """transport_config with the given fields; enums may be given by name."""
    return _fill(_m.transport_config(), values)


def make_stream_config(**values):
    """stream_config with the given fields; enums may be given by name."""
    return _fill(_m.stream_config(), values)

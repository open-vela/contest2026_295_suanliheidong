#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the active virtualenv esptool without changing its image mode."""

import os
import sys


def find_real_esptool():
    configured = os.environ.get("CONTEST_REAL_ESPTOOL")
    if configured:
        return configured

    own_path = os.path.realpath(__file__)
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = os.path.join(directory, "esptool.py")
        if os.path.isfile(candidate) and os.path.realpath(candidate) != own_path:
            return candidate

    raise FileNotFoundError("real esptool.py is not available in PATH")


def main():
    args = sys.argv[1:]

    if os.environ.get("CONTEST_USE_EXTERNAL_ESPTOOL") == "1":
        real_esptool = find_real_esptool()
        os.execv(real_esptool, [real_esptool, *args])

    # The build wrapper activates openvela/myenv first.  Running the
    # module through sys.executable therefore selects that esptool version
    # and preserves esptool's default section-based ELF handling.
    os.execv(sys.executable, [sys.executable, "-m", "esptool", *args])


if __name__ == "__main__":
    main()

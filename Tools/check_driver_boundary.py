"""Reject direct client HAL imports from every architecture of a driver Mach-O.

This is a necessary production gate, not a proof of runtime safety. Dependency
wrappers and dynamically resolved calls also violate ARCHITECTURE.md and require
review. No grandfathered imports or prototype allowlist are accepted here.
"""

import re
import subprocess
import sys


def forbidden_imports(nm_output):
    # nm -u -j emits names alone, plus architecture headings for universal files.
    # Include legacy stream APIs as well as modern object/device/hardware APIs.
    return sorted(set(re.findall(
        r"^_?(Audio(?:Object|Device|Hardware|Stream)[A-Za-z0-9_]+)\s*$",
        nm_output, re.MULTILINE,
    )))


def main(argv):
    if len(argv) != 2:
        print("usage: check_driver_boundary.py DRIVER_MACH_O", file=sys.stderr)
        return 2
    try:
        result = subprocess.run(
            ["/usr/bin/nm", "-arch", "all", "-u", "-j", argv[1]],
            capture_output=True, text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Cannot inspect driver: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        return 2
    violations = forbidden_imports(result.stdout)
    if violations:
        print("FAIL: AudioServerPlugIn imports client HAL APIs:", file=sys.stderr)
        for symbol in violations:
            print(f"  {symbol}", file=sys.stderr)
        print("Move these calls outside the plug-in host; see ARCHITECTURE.md.",
              file=sys.stderr)
        return 1
    print("PASS: no direct client HAL imports in any driver architecture.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

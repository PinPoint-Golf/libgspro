#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""test_python_abi.py — the ctypes layout against the compiler's.  CT-X07.

⚠ WHY THIS TEST EXISTS.  python/gspro/_types.py declares every public struct
and enum a SECOND time.  One side changes, the other is left stale, and the
result is a plausible answer with no alarm anywhere — a `spin_axis` read at
`total_spin`'s offset gives every shot a wrong-but-believable spin axis.

gsp_abi_check() does not close it, and version.h says so itself: it compares
ELEVEN STRUCT SIZES and says nothing about where the fields inside them sit, or
about any enum at all.  A club enumerator one place out puts a launch monitor
into putting mode for a sand wedge.

So the binding is pinned against `gs_abi_table`, which reads sizes, offsets,
enumerator values and bounds straight out of the compiler.  ⚠ That tool writes
its own authority down honestly: see tools/gs_abi_table.c for the three checks
and the one shape all three miss.

Usage: test_python_abi.py <path to gs_abi_table> [<path to libgspro_ffi>]
"""

import ctypes
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

failures = 0
checks = 0


def check(condition, what):
    global failures, checks
    checks += 1
    if condition:
        print(f"[ ok ] {what}")
    else:
        print(f"[FAIL] {what}")
        failures += 1


def _tail(items):
    return f": {', '.join(items)}" if items else ""


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    table_tool = sys.argv[1]
    if len(sys.argv) > 2:
        # ⚠ The .so is passed explicitly rather than discovered.  This must pin
        # the layout of the object THIS build produced — a search that found
        # build/dev while testing build/san would report on the wrong artefact
        # and pass.
        os.environ["GSPRO_LIBRARY"] = sys.argv[2]

    out = subprocess.run([table_tool], capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr, file=sys.stderr)
        print("⚠ `gs_abi_table` FAILED its own self-check — the C table is out of")
        print("  step with the headers.  NOTHING WAS CHECKED.")
        return 1
    table = json.loads(out.stdout)

    from gspro import _types as T

    # ⚠ Importing at all runs gsp_abi_check().  If that raised we would never
    # reach here, so this line is not the check — it is the record that it ran.
    import gspro

    check(
        gspro.ABI_VERSION == table["abi_version"],
        f"ABI version agrees ({gspro.ABI_VERSION})",
    )
    check(
        gspro.VERSION == table["library_version"],
        f"library version agrees ({gspro.VERSION})",
    )

    # ------------------------------------------------------------------
    # Structs
    # ------------------------------------------------------------------
    c_structs = table["structs"]
    py_structs = T.PINNED_STRUCTS

    # ⚠ BOTH DIRECTIONS.  A struct in the C table with no ctypes counterpart is
    # a struct nothing compares; a ctypes struct with no C row is a layout
    # nobody checked against the compiler.  Neither may be quiet.
    missing_in_py = sorted(set(c_structs) - set(py_structs))
    missing_in_c = sorted(set(py_structs) - set(c_structs))
    check(not missing_in_py, f"every C struct is mirrored in ctypes{_tail(missing_in_py)}")
    check(not missing_in_c, f"every ctypes struct has a C row{_tail(missing_in_c)}")

    # ⚠ And that PINNED_STRUCTS really covers the module, so a Structure added
    # to _types.py and left out of the dict is not silently unchecked.
    declared = {
        name
        for name, obj in vars(T).items()
        if isinstance(obj, type)
        and issubclass(obj, (ctypes.Structure, ctypes.Union))
        and obj.__module__ == T.__name__
        and name != "gsp_event_payload"  # a member of gsp_event, pinned via `u`
    }
    check(
        declared <= set(py_structs),
        "PINNED_STRUCTS covers every struct in _types.py"
        f"{_tail(sorted(declared - set(py_structs)))}",
    )

    for name in sorted(set(c_structs) & set(py_structs)):
        _check_struct(name, c_structs[name], py_structs[name])

    # ------------------------------------------------------------------
    # Enums
    # ------------------------------------------------------------------
    c_enums = table["enums"]
    mirrored = {c_name for c_name, _prefix in T.PINNED_ENUMS.values()}
    unmirrored = sorted(set(c_enums) - mirrored)
    check(not unmirrored, f"every C enum is mirrored in Python{_tail(unmirrored)}")

    for py_enum, (c_name, prefix) in T.PINNED_ENUMS.items():
        _check_enum(py_enum, c_name, prefix, c_enums)

    # ------------------------------------------------------------------
    # Bounds
    # ------------------------------------------------------------------
    # ⚠ A BOUND IS PART OF A LAYOUT TOO.  A stale GSP_WRITE_MAX in Python reads
    # a write request short, and the struct-size check cannot see it because the
    # array field would still agree with itself.
    c_constants = table["constants"]
    py_constants = T.PINNED_CONSTANTS
    extra_py = sorted(set(py_constants) - set(c_constants))
    extra_c = sorted(set(c_constants) - set(py_constants))
    check(not extra_py, f"no Python bound is absent from C{_tail(extra_py)}")
    check(not extra_c, f"no C bound is absent from Python{_tail(extra_c)}")
    wrong = [
        f"{k}={py_constants[k]} vs {c_constants[k]}"
        for k in sorted(set(py_constants) & set(c_constants))
        if py_constants[k] != c_constants[k]
    ]
    check(not wrong, f"every bound matches{_tail(wrong)}")

    print(f"\n{checks - failures}/{checks} checks passed")
    if failures:
        print(
            "\n⚠ The binding and the headers disagree.  Fix python/gspro/_types.py\n"
            "  — the compiler is right and the transcription is not."
        )
        return 1
    return 0


def _check_struct(name, c_struct, py_struct):
    c_size = c_struct["size"]
    py_size = ctypes.sizeof(py_struct)
    # ⚠ THE SIZE IS WHAT ACTUALLY CATCHES A FORGOTTEN FIELD, because a field
    # added to a header changes sizeof even when the C table's own tiling check
    # cannot see it (tools/gs_abi_table.c).
    check(c_size == py_size, f"{name}: sizeof {py_size} == {c_size}")

    c_fields = {f["name"]: f for f in c_struct["fields"]}
    py_fields = {f[0]: f for f in py_struct._fields_}

    extra_py = sorted(set(py_fields) - set(c_fields))
    extra_c = sorted(set(c_fields) - set(py_fields))
    check(not extra_py, f"{name}: no ctypes field is absent from C{_tail(extra_py)}")
    check(not extra_c, f"{name}: no C field is absent from ctypes{_tail(extra_c)}")

    for field in sorted(set(c_fields) & set(py_fields)):
        descriptor = getattr(py_struct, field)
        c_field = c_fields[field]
        ok = descriptor.offset == c_field["offset"] and descriptor.size == c_field["size"]
        check(
            ok,
            f"{name}.{field}: offset {descriptor.offset}/{c_field['offset']}, "
            f"size {descriptor.size}/{c_field['size']}",
        )


def _check_enum(py_enum, c_name, prefix, c_enums):
    if c_name not in c_enums:
        check(False, f"{py_enum.__name__}: `{c_name}` is not in the C enum table")
        return

    c_members = {}
    for enumerator, value in c_enums[c_name].items():
        if not enumerator.startswith(prefix):
            check(False, f"{c_name}: `{enumerator}` does not start with `{prefix}`")
            return
        c_members[enumerator[len(prefix) :]] = value

    # ⚠ `__members__`, never iteration.  Iterating an IntFlag yields only the
    # canonical single-bit members, so a member covering two bits would vanish
    # from the comparison and read as agreement.
    py_members = {name: int(member.value) for name, member in py_enum.__members__.items()}

    extra_py = sorted(set(py_members) - set(c_members))
    extra_c = sorted(set(c_members) - set(py_members))
    check(not extra_py, f"{py_enum.__name__}: no member absent from C{_tail(extra_py)}")
    check(not extra_c, f"{py_enum.__name__}: no C member absent{_tail(extra_c)}")

    wrong = [
        f"{k}={py_members[k]} vs {c_members[k]}"
        for k in sorted(set(py_members) & set(c_members))
        if py_members[k] != c_members[k]
    ]
    check(not wrong, f"{py_enum.__name__}: every value matches{_tail(wrong)}")


if __name__ == "__main__":
    sys.exit(main())

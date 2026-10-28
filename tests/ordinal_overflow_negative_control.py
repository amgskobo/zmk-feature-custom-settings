"""Restore unchecked ordinal accumulation in an isolated production-source copy.

The owner settings-boundary keyspace-ordinal fixture must then fail on the real
settings loader. This operator does not change tests or shared dependencies.
"""

from pathlib import Path
import sys


def main() -> None:
    target = Path(sys.argv[1]).resolve()
    owner = Path(__file__).resolve().parents[1]
    if target.is_relative_to(owner):
        raise SystemExit("negative control requires an isolated source copy")
    if target.name != "custom_settings_keyspace.c":
        raise SystemExit("unexpected negative-control target")
    before = target.read_text(encoding="utf-8")
    checked = """        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;"""
    if before.count(checked) != 1:
        raise SystemExit("expected exactly one checked ordinal accumulator")
    target.write_text(
        before.replace(checked, "        value = value * 10 + (uint32_t)(*p - '0');"),
        encoding="utf-8",
    )
    print("Negative control: stored ordinal accumulation is unchecked")


if __name__ == "__main__":
    main()

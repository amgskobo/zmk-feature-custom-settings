"""Remove one stored array-name guard in an isolated production-source copy.

The unchanged owner real settings-loader assertions must fail. Assertions and
shared dependencies are never changed by this operator.
"""

from pathlib import Path
import sys


def main() -> None:
    target = Path(sys.argv[1]).resolve()
    fault = sys.argv[2]
    owner = Path(__file__).resolve().parents[1]
    if target.is_relative_to(owner):
        raise SystemExit("negative control requires an isolated source copy")
    if target.name != "custom_settings_array.c":
        raise SystemExit("unexpected negative-control target")
    before = target.read_text(encoding="utf-8")
    if fault == "overflow":
        guard = """        uint32_t digit = (uint32_t)(index_str[i] - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;"""
        replacement = "        value = value * 10 + (uint32_t)(index_str[i] - '0');"
    elif fault == "truncation":
        guard = """    if (name[name_len] != '\\0') {
        return false;
    }
"""
        replacement = ""
    else:
        raise SystemExit("unknown array ordinal negative-control fault")
    if before.count(guard) != 1:
        raise SystemExit("expected exactly one selected array-name guard")
    target.write_text(before.replace(guard, replacement), encoding="utf-8")
    print("Negative control: stored array ordinal " + fault + " guard removed")


if __name__ == "__main__":
    main()

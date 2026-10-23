"""Disable only the quiet notification guard in an isolated owner copy.

The unchanged native public-suppression fixture must then fail; this operator
does not provide assertions or alter shared ZMK/Zephyr dependencies.
"""

from pathlib import Path
import sys


def main() -> None:
    target = Path(sys.argv[1]).resolve()
    owner = Path(__file__).resolve().parents[1]
    if target.is_relative_to(owner):
        raise SystemExit("negative control requires an isolated source copy")
    if target.name != "custom_settings_handler.c":
        raise SystemExit("unexpected negative-control target")
    before = target.read_text(encoding="utf-8")
    guard = "if (ev->suppress_studio_notification || notify_suppressed())"
    if before.count(guard) != 1:
        raise SystemExit("expected exactly one quiet notification guard")
    target.write_text(
        before.replace(guard, "if (notify_suppressed())"), encoding="utf-8"
    )
    print("Negative control: quiet event still dispatches, Studio guard disabled")


if __name__ == "__main__":
    main()

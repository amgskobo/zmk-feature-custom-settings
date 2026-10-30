"""Remove only retained-view recycling protection in an isolated source copy."""

from pathlib import Path
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: notification_view_negative_control.py ISOLATED_ARRAY_SOURCE"
        )
    target = Path(sys.argv[1]).resolve()
    original = Path(__file__).resolve().parents[1] / "src/custom_settings_array.c"
    if target == original.resolve():
        raise SystemExit("refusing to mutate the owning source checkout")
    source = target.read_text()
    guard = "!recyclable && slot->in_use && slot->notification_refs == 0"
    if source.count(guard) != 1:
        raise SystemExit("expected exactly one retained-view recycling guard")
    target.write_text(source.replace(guard, "!recyclable && slot->in_use"))
    print("Removed only the retained-view recycling guard in the isolated source")


if __name__ == "__main__":
    main()

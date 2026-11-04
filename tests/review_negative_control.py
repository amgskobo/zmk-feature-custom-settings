"""Restore one reviewed defect in an isolated production source directory.

Run the unchanged owner scenario against the result; compilation failures are
not evidence that its regression assertion rejected the defect.
"""

import argparse
from pathlib import Path


def replace_once(source: str, before: str, after: str) -> str:
    if source.count(before) != 1:
        raise SystemExit("expected exactly one reviewed source guard")
    return source.replace(before, after)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument(
        "fault",
        choices=(
            "ordinal-lookup",
            "stored-shape",
            "scope-identity",
            "notification",
            "rpc-epoch",
        ),
    )
    args = parser.parse_args()
    root = args.source.resolve()
    owner = Path(__file__).resolve().parents[1]
    if root.is_relative_to(owner) or not (root / "custom_settings.c").is_file():
        raise SystemExit(
            "negative control requires an isolated production source directory"
        )
    handler = args.fault in ("notification", "rpc-epoch")
    target = root / (
        "studio/custom_settings_handler.c" if handler else "custom_settings.c"
    )
    source = target.read_text()
    if args.fault == "ordinal-lookup":
        start = source.index("        /* Storage names identify registered settings")
        end = source.index("        if (!setting) {", start)
        source = (
            source[:start]
            + (
                "        setting = (struct zmk_custom_setting *)"
                "zmk_custom_setting_find(custom_subsystem_id, next);\n"
            )
            + source[end:]
        )
    elif args.fault == "stored-shape":
        source = replace_once(
            source,
            "int ret = keyspace_validate_storage_blob_locked(setting, data, len);",
            "int ret = 0; /* Deliberately skip stored shape admission. */",
        )
    elif args.fault == "scope-identity":
        start = source.index(
            "                /* Selection and the operation share the paired guard"
        )
        end = source.index("                if (ret < 0 && first_error == 0)", start)
        source = (
            source[:start]
            + """                custom_settings_memory_lock();
                bool in_use = keyspace->slots[i].in_use;
                custom_settings_memory_unlock();
                if (!in_use) {
                    continue;
                }
                const struct zmk_custom_setting *slot_setting = &keyspace->slots[i].setting;
                if (!zmk_custom_setting_matches(slot_setting, custom_subsystem_id, key,
                                                key_prefix)) {
                    continue;
                }
                int ret = callback(slot_setting);
"""
            + source[end:]
        )
    elif args.fault == "notification":
        source = replace_once(
            source,
            "if ((value->has_value || value->has_default_value) &&",
            "if (false && (value->has_value || value->has_default_value) &&",
        )
        start = source.index("static bool encode_setting_scalar_value(")
        end = source.index("static bool encode_setting_default_value(", start)
        body = replace_once(
            source[start:end],
            "if (needs_unlock(setting->read_permission))",
            "if (false && needs_unlock(setting->read_permission))",
        )
        source = source[:start] + body + source[end:]
    else:
        source = replace_once(
            source,
            "keyspace_epoch != keyspace_identity_epoch_locked()",
            "false /* Deliberately admit a replacement slot. */",
        )
    target.write_text(source)
    print(
        "Restored only the selected reviewed defect in isolated production source:",
        args.fault,
    )


if __name__ == "__main__":
    main()

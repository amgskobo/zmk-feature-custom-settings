"""Keep raw native runs, require real exit status and each registered test result."""

from pathlib import Path
import re
import subprocess


def verify_native(output: Path, tests: Path):
    binaries = sorted(output.rglob("zmk.exe"))
    if not binaries:
        raise AssertionError(f"No native binary below {output}")
    for binary in binaries:
        symbols = subprocess.check_output(
            ["nm", "--defined-only", str(binary)], text=True
        )
        hooks = re.findall(r"\bowner_checked_(\w+)$", symbols, re.M)
        case = binary.parent.parent.name
        fixture = tests / case
        if not (fixture / "keycode_events.snapshot").is_file():
            raise AssertionError(f"Unknown native fixture: {case}")
        destination = binary.parent.parent / "verified-runs"
        destination.mkdir(exist_ok=False)
        for trial in range(1, 4):
            run = destination / str(trial)
            run.mkdir()  # independent empty initial-flash directory, same binary/input
            result = subprocess.run(
                [str(binary)],
                cwd=run,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
            raw = result.stdout
            (run / "raw.log").write_text(raw)
            if result.returncode:
                raise AssertionError(
                    f"{case}: native exit {result.returncode}; see {run}"
                )
            for hook in hooks:
                values = re.findall(
                    r"OWNER_TEST_RESULT " + re.escape(hook) + r"=(-?\d+)", raw
                )
                if values != ["0"]:
                    raise AssertionError(
                        f"{case}: {hook} completion {values}; see {run}"
                    )
            if re.search(
                r"(?:ASSERTION FAIL|ZEPHYR FATAL ERROR|PROJECT EXECUTION FAILED)", raw
            ):
                raise AssertionError(f"{case}: fatal assertion; see {run}")
            stripped = re.sub(r".*> ", "", raw)
            filtered = subprocess.run(
                ["sed", "-n", "-f", str(fixture / "events.patterns")],
                input=stripped,
                text=True,
                capture_output=True,
                check=True,
            )
            actual = run / "events.log"
            actual.write_text(filtered.stdout)
            diff = subprocess.run(
                ["diff", "-auZ", str(fixture / "keycode_events.snapshot"), str(actual)],
                text=True,
                capture_output=True,
            )
            (run / "snapshot.diff").write_text(diff.stdout + diff.stderr)
            if diff.returncode:
                raise AssertionError(f"{case}: unchanged snapshot mismatch; see {run}")
        print(
            f"Verified {case}: identical binary, raw ordered output, registered tests={len(hooks)}, repeats=3",
            flush=True,
        )


if __name__ == "__main__":
    import argparse
    import hashlib
    import json
    import shutil

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retained-build", required=True, type=Path)
    parser.add_argument("--new-output", required=True, type=Path)
    args = parser.parse_args()
    # Preserve old failures; reuse bytes only in a fresh independently logged copy.
    shutil.copytree(
        args.retained_build,
        args.new_output,
        ignore=shutil.ignore_patterns("verified-runs"),
    )
    hashes = {}
    for binary in args.new_output.rglob("zmk.exe"):
        binary.chmod(0o755)
        hashes[str(binary.relative_to(args.new_output))] = hashlib.sha256(
            binary.read_bytes()
        ).hexdigest()
    (args.new_output / "retained-binary-hashes.json").write_text(
        json.dumps(hashes, indent=2)
    )
    verify_native(args.new_output.resolve(), Path(__file__).resolve().parent)

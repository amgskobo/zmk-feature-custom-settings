"""Resolve the Studio schema selected by the caller's actual west workspace."""

from pathlib import Path
import subprocess


def resolve_studio_proto_dir(repo_root: Path) -> Path:
    command = ["west", "list", "zmk-studio-messages", "-f", "{abspath}"]
    try:
        result = subprocess.run(
            command,
            cwd=repo_root,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        raise RuntimeError("Cannot run west to resolve the Studio schema") from error
    if result.returncode:
        raise RuntimeError(
            "Cannot resolve zmk-studio-messages from the selected west workspace: "
            + result.stderr.strip()
        )
    projects = result.stdout.strip().splitlines()
    if len(projects) != 1 or not projects[0] or not Path(projects[0]).is_absolute():
        raise ValueError("west must return exactly one absolute Studio project path")
    project = Path(projects[0]).resolve(strict=True)
    schema = project / "proto" / "zmk"
    for name in ("studio.proto", "custom.proto"):
        if not (schema / name).is_file():
            raise FileNotFoundError("Selected Studio schema is missing " + name)
    return schema

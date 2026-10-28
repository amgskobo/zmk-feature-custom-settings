"""Owner schema-selection controls; fixtures are synthetic, not Studio schemas."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "owner_renode_proto_paths",
    Path(__file__).parent / "renode" / "proto_paths.py",
)
RESOLVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RESOLVER)


class RenodeProtoPathContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.owner = self.root / "owner"
        self.owner.mkdir()

    def schema(self, relative):
        project = self.root / relative
        directory = project / "proto" / "zmk"
        directory.mkdir(parents=True)
        for name in ("studio.proto", "custom.proto"):
            (directory / name).write_text("// Synthetic selection fixture.\n")
        return project, directory

    def response(self, output="", code=0, error=""):
        return subprocess.CompletedProcess([], code, output, error)

    def test_parent_workspace_uses_selected_project(self):
        project, expected = self.schema("modules/msgs/zmk-studio-messages")
        with patch.object(
            RESOLVER.subprocess,
            "run",
            return_value=self.response(str(project) + "\n"),
        ) as run:
            self.assertEqual(RESOLVER.resolve_studio_proto_dir(self.owner), expected)
        run.assert_called_once_with(
            ["west", "list", "zmk-studio-messages", "-f", "{abspath}"],
            cwd=self.owner,
            capture_output=True,
            text=True,
        )

    def test_isolated_workspace_remains_supported(self):
        project, expected = self.schema(
            "owner/dependencies/modules/msgs/zmk-studio-messages"
        )
        with patch.object(
            RESOLVER.subprocess, "run", return_value=self.response(str(project))
        ):
            self.assertEqual(RESOLVER.resolve_studio_proto_dir(self.owner), expected)

    def test_selected_path_with_spaces_is_literal(self):
        project, expected = self.schema("selected workspace/Studio messages")
        with patch.object(
            RESOLVER.subprocess, "run", return_value=self.response(str(project))
        ):
            self.assertEqual(RESOLVER.resolve_studio_proto_dir(self.owner), expected)

    def test_failed_selection_never_uses_stale_isolated_schema(self):
        self.schema("owner/dependencies/modules/msgs/zmk-studio-messages")
        with patch.object(
            RESOLVER.subprocess,
            "run",
            return_value=self.response(code=1, error="synthetic manifest failure"),
        ):
            with self.assertRaisesRegex(RuntimeError, "selected west workspace"):
                RESOLVER.resolve_studio_proto_dir(self.owner)

    def test_empty_multiple_and_relative_paths_are_refused(self):
        for value in ("", "\n", "relative/project", "/first\n/second\n"):
            with (
                self.subTest(value=value),
                patch.object(
                    RESOLVER.subprocess, "run", return_value=self.response(value)
                ),
            ):
                with self.assertRaises(ValueError):
                    RESOLVER.resolve_studio_proto_dir(self.owner)

    def test_missing_selected_project_does_not_fall_back(self):
        self.schema("owner/dependencies/modules/msgs/zmk-studio-messages")
        with patch.object(
            RESOLVER.subprocess,
            "run",
            return_value=self.response(str(self.root / "absent")),
        ):
            with self.assertRaises(FileNotFoundError):
                RESOLVER.resolve_studio_proto_dir(self.owner)

    def test_each_required_schema_file_is_checked(self):
        for name in ("studio.proto", "custom.proto"):
            with self.subTest(name=name):
                project, schema = self.schema(name + "/selected")
                (schema / name).unlink()
                with patch.object(
                    RESOLVER.subprocess,
                    "run",
                    return_value=self.response(str(project)),
                ):
                    with self.assertRaisesRegex(FileNotFoundError, name):
                        RESOLVER.resolve_studio_proto_dir(self.owner)

    def test_selection_is_independent_and_retryable(self):
        first, first_schema = self.schema("first/project")
        second, second_schema = self.schema("second/project")
        with patch.object(
            RESOLVER.subprocess,
            "run",
            side_effect=[
                self.response(code=1, error="synthetic first failure"),
                self.response(str(first)),
                self.response(str(second)),
            ],
        ):
            with self.assertRaises(RuntimeError):
                RESOLVER.resolve_studio_proto_dir(self.owner)
            self.assertEqual(
                RESOLVER.resolve_studio_proto_dir(self.owner), first_schema
            )
            self.assertEqual(
                RESOLVER.resolve_studio_proto_dir(self.owner), second_schema
            )

    def test_missing_west_is_a_setup_error(self):
        with patch.object(RESOLVER.subprocess, "run", side_effect=OSError("synthetic")):
            with self.assertRaisesRegex(RuntimeError, "Cannot run west"):
                RESOLVER.resolve_studio_proto_dir(self.owner)


if __name__ == "__main__":
    unittest.main()

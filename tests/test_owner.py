import platform
import os
import tempfile
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass, field

from native_verifier import verify_native

THIS_DIR = Path(__file__).resolve().parent.parent


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )


@dataclass
class NotFound:
    text: str


@dataclass
class ConfigAndDeviceTree:
    # Expected rows in .config
    config: list[str | NotFound]
    # Expected rows in devicetree_generated.h
    device: list[str | NotFound]
    # Expected byte strings in zephyr.elf
    binary: list[bytes | NotFound] = field(default_factory=list)


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(run_west(["topdir"]).stdout.strip())
        requested_output = os.environ.get("OWNER_TEST_OUTPUT")
        if requested_output:
            cls.BUILD_DIR = Path(requested_output).resolve()
            cls.BUILD_DIR.mkdir(parents=True, exist_ok=False)
        else:
            cls.BUILD_DIR = Path(tempfile.mkdtemp(prefix=THIS_DIR.name + "-tests-"))
        print(f"Owner test output: {cls.BUILD_DIR}", flush=True)

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        test_build_dir = self.BUILD_DIR / THIS_DIR.name

        result = run_west(["zmk-test", "tests", "-m", ".", "-d", str(test_build_dir)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        verify_native(test_build_dir, THIS_DIR / "tests")
        self.assertIn("PASS: test", result.stdout, result.stdout + result.stderr)
        self.assertIn("PASS: studio", result.stdout, result.stdout + result.stderr)
        self.assertNotIn("FAILED: ", result.stdout, result.stdout + result.stderr)

    @unittest.skipUnless(
        platform.system() == "Linux", "kernel boundaries require Linux"
    )
    def test_settings_kernel_boundaries(self):
        result = subprocess.run(
            ["bash", "tests/settings-boundary/run.sh"],
            env={
                **os.environ,
                "CUSTOM_SETTINGS_NATIVE_ARTIFACTS": os.environ.get(
                    "CUSTOM_SETTINGS_NATIVE_ARTIFACTS",
                    str(self.BUILD_DIR / "settings-boundary"),
                ),
            },
            capture_output=True,
            text=True,
            cwd=THIS_DIR,
        )
        (self.BUILD_DIR / "test_settings_kernel_boundaries.log").write_text(
            result.stdout + result.stderr, encoding="utf-8"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Completed core boundary scenarios=10 repeats=3", result.stdout)
        self.assertIn(
            "Completed features boundary scenarios=16 repeats=3", result.stdout
        )

    def test_zmk_build(self):
        self._test_zmk_build(
            {
                "custom_settings_board_feature_disabled": ConfigAndDeviceTree(
                    config=[
                        'CONFIG_ZMK_KEYBOARD_NAME="Module Test"',
                        "CONFIG_ZMK_USB=y",
                        "CONFIG_ZMK_BLE=y",
                        "# CONFIG_ZMK_CUSTOM_SETTINGS is not set",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_zmk_keymap",
                    ],
                ),
                "custom_settings_board_with_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_PROTOBUF=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES=y",
                        "CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128",
                        "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_RECORD=y",
                    ],
                    device=[],
                    binary=[
                        b"zmk_config_sample",
                        b"int32_value",
                        b"bool_value",
                        b"string_value",
                        b"bytes_value",
                        b"bytes_rpc_value",
                        b"array_value",
                        b"large_bytes_value",
                        b"pooled_bytes_value",
                    ],
                ),
                "custom_settings_board_without_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                        NotFound("CONFIG_ZMK_CUSTOM_SETTINGS_PROTOBUF"),
                        NotFound("CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC"),
                        "CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_RECORD=y",
                    ],
                    device=[],
                    binary=[
                        b"zmk_config_sample",
                        b"int32_value",
                        b"bool_value",
                        b"string_value",
                        b"bytes_value",
                        b"bytes_rpc_value",
                        b"array_value",
                        b"large_bytes_value",
                        b"pooled_bytes_value",
                    ],
                ),
                "custom_settings_board_minimal": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_PROTOBUF=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES=y",
                        # Feature gating: module + Studio RPC on, all five optional
                        # features explicitly disabled by this fixture. Each still
                        # has a Kconfig prompt (so
                        # Kconfig emits a "not set" comment rather than omitting the
                        # line entirely) - assert the comment form, not NotFound.
                        "# CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES is not set",
                        "# CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY is not set",
                        "# CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE is not set",
                        "# CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS is not set",
                        "# CONFIG_ZMK_CUSTOM_SETTINGS_RECORD is not set",
                    ],
                    device=[],
                    binary=[
                        b"zmk_config_sample",
                        b"int32_value",
                        b"bool_value",
                        b"string_value",
                        b"bytes_value",
                        # These samples are #if-guarded out in
                        # src/test/zmk_config_sample_settings.c when their gate is
                        # off - confirm the minimal build really strips them.
                        # (array_value is not checked here: it collides with an
                        # unrelated, always-present RPC oneof field name of the
                        # same spelling in the generated protobuf code.)
                        NotFound("bytes_rpc_value"),
                        NotFound("large_bytes_value"),
                        NotFound("pooled_bytes_value"),
                        NotFound("profile/"),
                    ],
                ),
                "custom_settings_split_peripheral_with_rpc_relay": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_PROTOBUF=y",
                        NotFound("CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC"),
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES=y",
                        "CONFIG_ZMK_SPLIT=y",
                        "CONFIG_ZMK_SPLIT_BLE=y",
                        "# CONFIG_ZMK_SPLIT_ROLE_CENTRAL is not set",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY=y",
                        "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_RECORD=y",
                    ],
                    device=[],
                    binary=[
                        b"zmk_config_sample",
                        b"int32_value",
                        b"bool_value",
                        b"string_value",
                        b"bytes_value",
                        b"bytes_rpc_value",
                        b"array_value",
                        b"large_bytes_value",
                        b"pooled_bytes_value",
                    ],
                ),
                "custom_settings_split_central_with_rpc_relay": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_PROTOBUF=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES=y",
                        "CONFIG_ZMK_SPLIT=y",
                        "CONFIG_ZMK_SPLIT_BLE=y",
                        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y",
                        "CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=1",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY=y",
                        "CONFIG_ZMK_STUDIO_TRANSPORT_UART=y",
                        "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_RPC_CONVERTERS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_RECORD=y",
                    ],
                    device=[],
                    binary=[
                        b"zmk_config_sample",
                        b"int32_value",
                        b"bool_value",
                        b"string_value",
                        b"bytes_value",
                        b"bytes_rpc_value",
                        b"array_value",
                        b"large_bytes_value",
                        b"pooled_bytes_value",
                    ],
                ),
            }
        )

    def _test_zmk_build(
        self, artifacts_and_expected_build_params: dict[str, ConfigAndDeviceTree]
    ):

        result = run_west(
            [
                "zmk-build",
                "tests/zmk-config",
                "-q",
                "-P",
                "1",
                "-d",
                str(self.BUILD_DIR),
            ]
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_build_params.items():
            artifact_dir = self.BUILD_DIR / artifact / "zephyr"
            config_path = artifact_dir / ".config"
            device_tree_path = (
                artifact_dir
                / "include"
                / "generated"
                / "zephyr"
                / "devicetree_generated.h"
            )
            self._test_strings_in_file(
                config_path, entries.config, f"{artifact} config"
            )
            if entries.device:
                self._test_strings_in_file(
                    device_tree_path, entries.device, f"{artifact} device tree"
                )
            self.assertTrue(
                (artifact_dir / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {artifact_dir}",
            )
            if entries.binary:
                self._test_binary_strings_in_file(
                    artifact_dir / "zmk.elf", entries.binary, f"{artifact} zmk.elf"
                )

    def _test_strings_in_file(
        self, file_path: Path, expected_strings: list[str | NotFound], hint: str
    ):
        self.assertTrue(file_path.exists(), f"{hint}: {file_path} is missing")
        file_text = file_path.read_text()

        for expected in expected_strings:
            if isinstance(expected, NotFound):
                if expected.text in file_text:
                    self.fail(
                        f"{hint}: {expected.text} found in {file_path}, but it should not be present"
                    )
            else:
                if expected not in file_text:
                    self.fail(f"{hint}: {expected} not found in {file_path}")

    def _test_binary_strings_in_file(
        self, file_path: Path, expected_strings: list[bytes | NotFound], hint: str
    ):
        self.assertTrue(file_path.exists(), f"{hint}: {file_path} is missing")
        file_bytes = file_path.read_bytes()

        for expected in expected_strings:
            if isinstance(expected, NotFound):
                needle = expected.text.encode()
                if needle in file_bytes:
                    self.fail(
                        f"{hint}: {needle!r} found in {file_path}, but it should not be present"
                    )
            else:
                if expected not in file_bytes:
                    self.fail(f"{hint}: {expected!r} not found in {file_path}")


if __name__ == "__main__":
    unittest.main()

# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Checks that the Rust Tauri crate, JavaScript API and CLI declarations, and
# locked Windows CLI binary all use the same pinned release version.

import json
from pathlib import Path
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[2]


class TauriVersionTests(unittest.TestCase):
    """Catch cross-language version drift before installer bundling."""

    def test_rust_js_and_windows_cli_are_pinned_together(self):
        """Require the desktop declarations and lock to match the Rust crate."""
        cargo = tomllib.loads((ROOT / "apps/desktop/src-tauri/Cargo.toml").read_text())
        package = json.loads((ROOT / "apps/desktop/package.json").read_text())
        lock = json.loads((ROOT / "apps/desktop/package-lock.json").read_text())
        rust = cargo["dependencies"]["tauri"]["version"]
        self.assertTrue(rust.startswith("="), "Rust Tauri must have an exact pin")
        expected = rust[1:]
        self.assertEqual(package["dependencies"]["@tauri-apps/api"], expected)
        self.assertEqual(package["devDependencies"]["@tauri-apps/cli"], expected)
        root_lock = lock["packages"][""]
        self.assertEqual(root_lock["dependencies"]["@tauri-apps/api"], expected)
        self.assertEqual(root_lock["devDependencies"]["@tauri-apps/cli"], expected)
        for name in ("api", "cli", "cli-win32-x64-msvc"):
            self.assertEqual(lock["packages"][f"node_modules/@tauri-apps/{name}"]["version"], expected)


if __name__ == "__main__":
    unittest.main()

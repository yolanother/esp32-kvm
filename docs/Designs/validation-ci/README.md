# Validation CI and native gates

The `validation` workflow runs five independent source gates:

| Job | Pinned environment | Check |
| --- | --- | --- |
| Rust | Windows Server 2022, Rust 1.93.1 | format, Clippy, full workspace tests including routing faults |
| Desktop | Windows Server 2022, Node.js 22.13.1 | lockfile install, TypeScript typecheck, Vite build |
| Protocol vectors | Windows Server 2022 | regenerate independent PowerShell CRC32C/COBS fixtures and require a clean diff, then run codec tests |
| Release inputs | Windows Server 2022, Python 3.12 | cross-language Tauri pin check and offline release packaging/icon tests |
| Firmware | Ubuntu 24.04, Espressif IDF Docker v5.5.1, ESP32-S3 | ESP-IDF build only |

The Rust job uses Windows because the workspace includes native Windows capture.
The Rust job explicitly runs the fake-clock routing fault suite before the full
workspace test command. The TypeScript package currently has no UI test script,
so the desktop job does not claim UI test coverage. The release-input job checks
the built-in Windows icon, deterministic packaging, and exact Rust/JavaScript
Tauri version pins without building an MSI. `Cargo.lock` and `apps/desktop/package-lock.json`
record resolved Rust and npm dependency versions. The workspace declares Rust
edition 2024 and minimum Rust 1.85; the CI toolchain matches the locally
verified Rust 1.93.1, including its newer locked dependencies.
`apps/desktop/package.json` pins React
19.2.4, TypeScript 5.9.3, Vite 7.3.6, and Tauri API/CLI 2.12.0. The repository
`LICENSE` is MIT and governs project code. The npm lockfile records these
direct packages and SPDX license values:

| Package | Locked version | License |
| --- | --- | --- |
| `react`, `react-dom` | 19.2.4 | MIT |
| `typescript` | 5.9.3 | Apache-2.0 |
| `vite` | 7.3.6 | MIT |
| `@tauri-apps/api`, `@tauri-apps/cli` | 2.12.0 | Apache-2.0 OR MIT |

This is a direct-dependency record, not a complete redistribution inventory.
Rust, transitive npm, and Espressif component licenses still require an
audited lockfile-based inventory before release.

The workflow does not flash the attached board. Physical CDC loopback, manual
BOOT recovery, BLE pairing/HID, 1,000 switches, and latency targets remain
unrun native gates. A source build or fault model cannot prove them. Record the
board revision, SDK version, Windows build, guest OS, BLE adapter, and test
instrumentation with each eventual native result. The board manifest and
recovery proof live in `docs/hardware/board-bringup.md`.

The firmware action follows [Espressif's CI action usage](https://github.com/espressif/esp-idf-ci-action)
with a specific [IDF Docker image tag](https://hub.docker.com/r/espressif/idf/tags).

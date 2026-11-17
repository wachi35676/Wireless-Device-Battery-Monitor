# About this fork

Fork of [krisgfx9/Wireless-Device-Battery-Monitor](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor).
The `all-features` branch (the default here) is upstream `main` (as of 17 November 2026) plus the
changes below that are still waiting upstream. Each one is also open as its own pull request:

| Feature | Upstream PR | Branch |
|---|---|---|
| Logitech G435 on its older Realtek receiver (`046D:0ACB`); longer wait for sleeping Logitech receiver mice | [#93](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/93) | `logitech-g435-and-receiver-wake` |
| Widget on secondary-monitor taskbars, with an on/off menu toggle | [#107](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/107) | `multi-monitor-taskbars` |

Already merged upstream, so included through `main`: phones linked with Microsoft Phone Link
([#96](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/96)) and GameSir controllers in
Xbox mode ([#106](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/106)).

## Building

- **Visual Studio 2022+** (C++ desktop workload): run `build-msvc.cmd`. `msvc_compat.h` maps one MinGW-only
  WinRT type name for MSVC; it is only used by this script.
- **MinGW-w64** (MSYS2): `make` or `build.bat`, as in the upstream README.

Then run `WirelessBattery.exe --list` to see what it finds, and `WirelessBattery.exe` for the taskbar widget.

## Settings

Stored under `HKCU\Software\WirelessBattery` and changed from the right-click menu: `TaskbarWidget`,
`OtherTaskbars`, `WidgetBackground`, `HiddenDevices`, `HideLastKnownDevices`. Defaults: widget on, next to
the tray, on all taskbars.

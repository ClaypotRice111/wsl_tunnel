# Native Windows build

C++17, Win32 common controls, Windows 10/11 x64. No .NET, PowerShell GUI host,
browser, DirectX runtime dependency or Python requirement for the Windows UI.
Runtime operations call wsl.exe, netsh.exe and usbipd.exe directly, with argument
quoting and timeouts. WSL reverse forwarding still needs socat in the chosen distro.

Download the Windows x64 UCRT archive from the official
[LLVM-MinGW releases](https://github.com/mstorsjo/llvm-mingw/releases), verify its
SHA256 against the release metadata, and extract under `.toolchain/`.
The initial build used `llvm-mingw-20261020-ucrt-x86_64`.

Run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Build-Native.ps1`.
Output: `dist/WSLTunnel-Native.exe`. The toolchain is only needed for builds.
`--self-test` checks core validation/quoting/parser behavior; `--smoke-test`
opens an off-screen native window and closes it automatically. Neither modifies
USB or forwarding configuration.

The manifest declares PerMonitorV2; WM_DPICHANGED recreates fonts and relays out
all controls at the current monitor DPI. Data is stored in
`%LOCALAPPDATA%/WSLTunnel/native-rules.ini`. Legacy PowerShell JSON rules are not
automatically imported. Existing system portproxy rules remain untouched until
explicit actions, and conflicting listeners are rejected. Inspect existing Windows portproxy rules before recreating old rules here; legacy program files have been removed.

Only one operation runs at a time; work executes on a background thread and the
UI handles results. Closing during an operation is deferred until it finishes.
The saved operation record does not claim live connection health.

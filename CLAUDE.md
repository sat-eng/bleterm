# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

bleterm is an interactive Bluetooth LE terminal: C++17, CMake, SimpleBLE v1.4.0 (fetched by `FetchContent` at configure time; Business Source License 1.1, non-commercial). README.md documents the user-facing commands.

## Build and run

Run these from the project root (the directory holding `CMakeLists.txt`, one level above `src/`):

```sh
cmake -S . -B build -G Ninja && cmake --build build && ./build/bleterm
```

- The first configure clones SimpleBLE, so it needs network access. After that, `cmake --build build` is enough.
- `-DBLETERM_OWN_BLUETOOTH_IDENTITY=ON` embeds `Info.plist` in the binary (a `__TEXT,__info_plist` section) so bleterm requests Bluetooth permission itself. This is needed only in Apple's Terminal.app, and macOS asks for approval again after every rebuild. By default the permission comes from the app that launches bleterm (iTerm2, CLion).
- libedit is optional. When it's found, `HAVE_EDITLINE` is defined and you get line editing, history and tab completion; without it, input falls back to plain `std::getline`. Keep both code paths compiling.
- The project has no tests and no lint setup. To exercise it without a real terminal, pipe commands in, e.g. `printf '/list 3\n/quit\n' | ./build/bleterm` (stdin isn't a TTY, so the non-interactive output path is used). Using it needs a real Bluetooth adapter and permission.

## Architecture

Almost everything is in `src/main.cpp`:

- **`Terminal`** holds all BLE state (adapter, last scan results, the connected peripheral, a flattened `chars_` list of characteristics) and dispatches `/commands` in `handle()`. Users refer to characteristics by 1-based index into `chars_`. Lines that don't start with `/` are written to the `selected_` characteristic.
- **Nordic UART auto-mode**: `autoSelectUart()` runs on connect. If the NUS service is present, it selects RX for writing, subscribes to TX as a stream, and sets the EOL to LF unless the user already chose one with `/eol` (`eolChosen_`).
- **Output and threading**: SimpleBLE callbacks (notifications, disconnects) run on other threads. All stdout writes must hold the global `outMutex`. Use `say()` for output on the main thread and `sayAsync()` from callbacks; `sayAsync()` clears the prompt line, prints, and redraws the prompt with any half-typed input. The prompt may only be redrawn while `inReadline` is true, otherwise a stale `> ` line is left behind.
- **`StreamOutput`** joins UART-style notification chunks (~20-byte packets) into continuous text. A worker thread redraws the prompt after 150 ms of quiet and swallows a device's trailing prompt (e.g. `> `, `$ `). `readLine()` calls `waitUntilQuiet()` before showing the prompt. `StreamOutput` is declared before `Terminal` in `main()` so it outlives the notification callbacks.
- **Tab completion**: `completeLine()` completes command names from `COMMANDS` and the first argument through `Terminal::argumentCompletions()`. When you add a command, update `HELP`, `COMMANDS`, `handle()`, and the README command table together.
- **Platform split**: `bluetoothProblem()` (declared in `src/bt_status.h`) explains why Bluetooth is unavailable. `bt_status_mac.mm` uses CoreBluetooth to tell a missing permission apart from Bluetooth being off (SimpleBLE reports both as "not enabled"). It is compiled as Objective-C++ with ARC via per-file compile options rather than by enabling the OBJCXX language. `bt_status_other.cpp` is the non-Apple fallback.

## Constraints

- Command history is kept in memory only and must never be written to disk, because it can contain Wi-Fi passwords sent to devices.
- On macOS the device "address" is a per-Mac UUID, not a MAC address. An RSSI of 127 means unknown.

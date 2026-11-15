# bleterm

An interactive Bluetooth LE terminal for the command line. Scan for devices, connect, browse their characteristics, and read, write or subscribe to them. Devices that use the Nordic UART service behave like a serial port.

Written in C++17 with CMake and [SimpleBLE](https://github.com/simpleble/simpleble) 1.4, which is fetched at configure time.

## Requirements

- CMake 3.21 or newer, a C++17 compiler, and Ninja (or any other CMake generator)
- Network access the first time you configure, to fetch SimpleBLE
- macOS: nothing else (CoreBluetooth and libedit are built in)
- Linux: DBus development headers (needed by SimpleBLE), plus `libedit-dev` for line editing and history (optional)

## Building

Open this folder in CLion and run the `bleterm` target, or build from a shell:

```sh
cmake -S . -B build -G Ninja && cmake --build build && ./build/bleterm
```

Tab completes commands (press it twice to list the options), `/hex` and `/eol` values, and device names or addresses from the last scan for `/connect`. Up/down arrows recall earlier commands (via libedit; history lasts for the session only and is never saved, because it can contain Wi-Fi passwords). Line editing needs a real terminal: in CLion, tick **Emulate terminal in the output console** in the run configuration.

**Bluetooth permission (macOS):** bleterm uses the Bluetooth permission of the app you run it from. iTerm2 and CLion support this. Turn that app on in System Settings > Privacy & Security > Bluetooth, and the permission keeps working across rebuilds. Apple's Terminal.app can't grant Bluetooth to programs it runs; to use bleterm there, configure with `-DBLETERM_OWN_BLUETOOTH_IDENTITY=ON`, which embeds `Info.plist` so bleterm asks for itself (`local.bleterm`). macOS then needs approval again after every rebuild, because ad-hoc signed builds look like new apps.
macOS hides MAC addresses, so the "address" shown is a per-Mac UUID.

## Commands

| Command | |
|---|---|
| `/list [seconds]` | scan (default 5 s) |
| `/connect <#\|address\|name>` | connect to a device from the last scan (name matches partially) |
| `/disconnect` | |
| `/services` | numbered characteristics with R/W/N flags and their 0x2901 descriptions |
| `/use <#>` | characteristic that plain typed lines are written to |
| `/read [#]`, `/write <#> <text>` | |
| `/notify <#> [stream]`, `/unnotify <#>` | print incoming notifications/indications, one labelled line each, or as continuous text with `stream` |
| `/hex [on\|off]` | show and type data as hex |
| `/eol none\|lf\|cr\|crlf` | line ending appended to typed text (default none) |
| `/help`, `/quit` | |

Devices with the Nordic UART service are picked up automatically, like a serial port: typed lines go to RX (with an LF line ending unless you chose one with `/eol`), and TX notifications are joined into continuous text, since devices split output into ~20-byte packets. For other UART-style characteristics use `/notify <#> stream`.

## Example: setting up the network clock

```
> /list
> /connect NetClock
> /services
   [ 1] 6e4c0002-...  RW-  Wi-Fi SSID
   [ 2] 6e4c0003-...  -W-  Wi-Fi password (write only)
   ...
> /write 1 HomeNet
> /write 2 my-wifi-password
> /read 4
```

## License

bleterm is released under the MIT License; see [LICENCE](LICENCE).

It depends on SimpleBLE, which has its own licence, the Business Source License 1.1: free for personal and non-commercial use. Commercial use of a bleterm build needs a SimpleBLE commercial licence.

# Astrion PC Unlock

[![CI](https://github.com/dckiller51/astrion-custom-dashboard-pc-unlock/actions/workflows/ci.yml/badge.svg)](https://github.com/dckiller51/astrion-custom-dashboard-pc-unlock/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/dckiller51/astrion-custom-dashboard-pc-unlock?style=flat-square)](https://github.com/dckiller51/astrion-custom-dashboard-pc-unlock/releases/latest)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Buy_me_a_coffee-F16061?style=flat-square&logo=ko-fi&logoColor=white)](https://ko-fi.com/dckiller)

An **ESP32-S3 plugged into your gaming PC as a USB keyboard**, that types your **Windows PIN** when the [Astrion remote](https://github.com/dckiller51/astrion-custom-dashboard) asks. Pick a game on the remote while the PC sleeps: it wakes, Windows unlocks, the game starts, all from the couch, with your PIN still on.

- **The PIN stays on the ESP.** It never travels over the network and shows in no log: the remote only sends "unlock", with a 6-digit code.
- **A real keyboard** for Windows: the sign-in screen takes it like any other, no software on the PC, nothing for the antivirus to flag.
- **Local only**: the remote talks to it on your network.

Made for Astrion's [Playnite](https://github.com/dckiller51/astrion-custom-dashboard-playnite) support, with the extension 0.1.2 or later (it tells the remote when Windows is locked).

## What you need

- An **ESP32-S3** with a native USB port. The **[M5Stack AtomS3U](https://docs.m5stack.com/en/homeassistant/kit/atoms3u)** plugs straight into a USB-A port, nothing else needed. Any other ESP32-S3 board works too (ESP32-S3 SuperMini, DevKitC…), on its *USB* port.
- A Windows PIN (Windows Hello), digits and/or letters.
- Astrion 1.6.3 or later, and the Playnite extension 0.1.2 or later.

## Install

**[Open the installer](https://dckiller51.github.io/astrion-custom-dashboard-pc-unlock/)** in Chrome or Edge on a computer.

1. **Download mode**: on the AtomS3U, hold the reset button about 2 seconds until the small green LED lights, then plug it in. On another board, hold **BOOT** while plugging its *USB* port.
2. Click your board, pick the port, install, unplug.
3. Plug it into the **gaming PC**. The LED turns purple: from a phone, join the `Astrion-Unlock-xxxx` Wi-Fi, open `http://192.168.4.1`, and fill in:
   - your **Wi-Fi** network and password;
   - your **Windows PIN** and the PC's **keyboard layout** (AZERTY, QWERTY, QWERTZ, or the numeric keypad);
   - note the **code for the remote**, then **Save**. It restarts and joins your Wi-Fi (as `astrion-unlock`).
4. In **Astrion**: the remote's web page › *Devices* › your PC › **🔓 Unlock ESP**: its IP address and the code.

The firmwares are also on each [release](https://github.com/dckiller51/astrion-custom-dashboard-pc-unlock/releases) (`…factory.bin`, to flash at `0x0` with esptool).

## How it works

| When | What the remote does |
| :--- | :--- |
| A game is picked while the PC sleeps | Wake-on-LAN, and asks the ESP to wake the PC through USB too; once Playnite says Windows is locked, asks the ESP to unlock; then starts the game |
| The PC starts with no session open | Playnite can't answer before you sign in: the remote has the PIN typed at the sign-in screen 40 s after the wake (then 80 s, 120 s) |
| The PC is on but locked | **Turn on** (or picking a game) unlocks it first |

To unlock, the ESP presses **Ctrl** (it shows the PIN field without typing anything), waits a moment, then types the PIN. The PIN is typed **three times at most** per wake, so a wrong one never runs into Windows' own limit.

## Its page and button

`http://<its IP>` shows its state: Wi-Fi, the PC's USB port (`active`, `suspended` when the PC sleeps, `off`), whether a PIN is saved.

- **Settings** are closed in normal use: **press the button**, then open its page within 3 minutes (the LED turns purple). The PIN and the Wi-Fi password are never shown, only replaced.
- **Hold the button 10 seconds**: everything is erased, back to the setup Wi-Fi.
- If it can't join your Wi-Fi within 90 seconds (a new password?), the setup Wi-Fi comes back.

LED: purple, settings open; blue, joining the Wi-Fi; green flash, PIN typed; red flash, a wrong code. Dark otherwise: it sits in your PC.

## Good to know

- **The ESP is powered by the PC.** If the PC's USB ports are off when it sleeps, the ESP is off too: it comes back a few seconds after the wake. Some BIOSes can keep USB powered in sleep (*USB power in S3/S5*).
- **Waking through USB** works when Windows lets the keyboard wake the PC (*Device Manager › Keyboards › Properties › Power Management › Allow this device to wake the computer*). Otherwise Wake-on-LAN does it.
- **Keyboard layout**: Windows types what the layout says: an AZERTY PC needs **AZERTY** (its digits are shifted). The numeric keypad needs Num Lock on at the sign-in screen.
- **Windows signs in by itself** (no PIN at the sign-in, or *"require sign-in: never"* after sleep)? Then you don't need this ESP: it would type the PIN into whatever is open.
- **Security**: anyone who can use your Astrion remote, or knows the ESP's code and is on your network, can unlock the PC. Five wrong codes block it for a minute. The PIN can't be read back from its page or from the remote.

## API

Every call but `GET /` and `GET /info` carries the code in an `X-Astrion-Code` header.

| | |
| :--- | :--- |
| `GET /info` | `{"app":"astrion-unlock","version","name","pin":bool,"usb":"active"\|"suspended"\|"off"}` |
| `POST /check` | the code is right, nothing typed |
| `POST /wake` | wakes the PC from sleep through USB, when it allows it |
| `POST /unlock` | wakes it if asleep, shows the sign-in field, types the PIN. `409` when the PC isn't on or no PIN is saved |

## Build it yourself

[PlatformIO](https://platformio.org): `pio run -e atoms3u` (or `-e esp32s3`), then `pio run -e atoms3u -t upload` in download mode.

## License

GPL-3.0

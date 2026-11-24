# Changelog

## [0.1.0] - 2026-11-24

First version, for Astrion 1.6.3 and its Playnite extension 0.1.2.

- A USB keyboard (ESP32-S3, TinyUSB) that types the Windows PIN kept on it, on the remote's `POST /unlock` with its 6-digit code; Ctrl first to show the PIN field
- Wakes the PC from sleep through USB (`POST /wake`) when Windows allows the keyboard to
- AZERTY, QWERTY, QWERTZ and numeric keypad layouts; Enter after the PIN as an option
- Setup Wi-Fi on the first start; settings opened by a press on the button for 3 minutes; 10 s held erases everything
- Five wrong codes block it for a minute
- M5Stack AtomS3U (LED and button) and any ESP32-S3 board; browser installer

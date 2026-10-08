# Prebuilt bundle

`dillydally-1.0.0-system-update.irisfw` is the whole application, built from the
source in this repository. Flash it and skip the toolchain entirely.

From a Vibe workspace root, with the board on USB

```powershell
.\m.ps1 iris system-update --bundle path\to\prebuilt\dillydally-1.0.0-system-update.irisfw
```

It writes the partition table, the application and the UI resources, then
validates all three. Vibe Mode lives in the `factory` partition and is left
alone, so you keep a way back.

Then give it a network. See the main README under
[First boot](../README.md#first-boot).

## Verify it first

```powershell
Get-FileHash .\dillydally-1.0.0-system-update.irisfw -Algorithm SHA256
```

```
1740481619550b28fd4c2a0c6272065b907e5938d8c376bb09aa0fd593ab4971
```

## What is inside

| Component | Offset | Bytes |
| --- | --- | --- |
| `partition-table.bin` | `0x8000` | 4096 |
| `ota_0.bin`, the application | `0x210000` | 3553136 |
| `ui_apps.bin`, scene, faces, fonts | `0xF00000` | 727376 |

Built at version 1.0.0 against ESP-IDF master for ESP32-S31.

## Why there are no raw bin files here

High Speed USB on this board belongs to ESP-Iris. Writing the flash directly
with esptool fights it, so the supported route is the bundle above. The bundle
carries every image with its hash and its offset, and it is written atomically.

No credentials are in this bundle. None are compiled in anywhere.

# HotSync server

FujiNet acts as the desktop side of a Palm OS HotSync. A device that syncs with
it gets the apps and databases queued on the SD card, and its databases are
backed up to the SD card. It is a service, not a bus: it runs on its own thread
beside whatever platform the firmware was built for.

A C++ port of the protocol stack in [palm-sync](https://github.com/jichu4n/palm-sync).

## Using it

Add to `fnconfig.ini` (an SD card is required):

```ini
[HotSync]
enabled=1
user=FujiNet          ; given to a device that has never synced
backup=flagged        ; none | flagged (backup bit set, like Palm Desktop) | all
netsync_port=14238    ; network HotSync; 0 turns it off
emulator_port=6416    ; serial-over-TCP for POSE / CloudpilotEmu; 0 turns it off
serial_port=          ; a cradle: "bus" on the ESP32, a device path on FujiNet-PC
```

`serial_port=bus` gives the cradle the UART the platform bus would use, so the
bus is not started.

The SD card layout, under `/palm`:

| Folder              | Contents                                               |
|---------------------|--------------------------------------------------------|
| `install/`          | `.prc`, `.pdb`, `.pqa` files to install on next sync   |
| `installed/`        | files moved here after a successful install           |
| `backup/<user>/`    | databases read back from the device                    |

## The cradle

A HotSync always opens at 9600 baud; Palm apps that talk to FujiNet use
115200 (`PalmAppChannel.h`). The cradle alternates between the two rates every
1.5 s. Both the device's WAKEUP and an app's request are repeated for longer
than one cycle, so each lands in its own window.

## Layout

| File                    | Layer                                                    |
|-------------------------|----------------------------------------------------------|
| `HotSyncLink.h`         | raw byte link (UART, socket, test fixture)              |
| `Slp.*`                 | Serial Link Protocol framing and CRC                    |
| `PadpTransport.*`       | PADP fragments, ACK/retry, and the CMP handshake        |
| `NetSyncTransport.*`    | NetSync framing and handshake                           |
| `Dlp.*`, `DlpClient.*`  | Desktop Link Protocol encoding and typed commands       |
| `PalmDatabase.*`        | `.pdb`/`.prc` files                                     |
| `HotSyncSession.*`      | one sync: identify, install, back up, stamp user info   |
| `PalmAppChannel.*`      | the text channel for Palm apps                          |
| `HotSyncStorage.h`, `HotSyncFsStorage.*` | files on the SD card                   |
| `HotSyncLinks.*`, `HotSyncService.*`     | FujiNet sockets, serial, and thread    |

Everything up to `HotSyncSession` depends only on `include/global_types.h`, so
`tests/HotSyncTests.cpp` drives it with frames captured from a real device.

## Not done yet

- Two-way record sync (Memo, Address, Date Book conduits). Backups are one way.
- A web UI page for the settings and the last sync result.
- Installs and backups hold a whole database in RAM, which is fine on boards
  with PSRAM but tight without it.

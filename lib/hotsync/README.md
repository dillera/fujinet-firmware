# lib/hotsync

The desktop side of a Palm OS HotSync: a device that syncs with FujiNet gets the apps and databases
queued for it on the SD card, its Date Book gets the events of a Google or iCalendar calendar, and its
databases are backed up there.

## Layout
| File | Defines |
|---|---|
| `HotSyncLink.h` | `HotSyncLink`, the raw byte link a transport runs over (UART, socket, test fixture) |
| `Slp.h`, `Slp.cpp` | Serial Link Protocol framing and CRC |
| `PadpTransport.h`, `PadpTransport.cpp` | PADP fragments, ACK/retry, and the CMP handshake |
| `NetSyncTransport.h`, `NetSyncTransport.cpp` | NetSync framing and handshake |
| `DlpTransport.h` | `DlpTransport`, the request/response interface both transports implement |
| `Dlp.h`, `Dlp.cpp` | Desktop Link Protocol request and response encoding |
| `DlpClient.h`, `DlpClient.cpp` | typed DLP commands |
| `PalmDatabase.h`, `PalmDatabase.cpp` | `.pdb`/`.prc` files |
| `HotSyncSession.h`, `HotSyncSession.cpp` | one sync: identify, install, Date Book, back up, stamp user info |
| `Datebook.h`, `Datebook.cpp` | Date Book records |
| `DatebookConduit.h`, `DatebookConduit.cpp` | copying calendar events into the Date Book |
| `HotSyncCalendar.h` | `HotSyncCalendar` and `HotSyncEvent`, the events a session copies |
| `HotSyncNetCalendar.h`, `HotSyncNetCalendar.cpp` | `HotSyncCalendar` read through GCAL: and ICAL: |
| `HotSyncStorage.h` | `HotSyncStorage`, the storage a session reads and writes |
| `HotSyncFsStorage.h`, `HotSyncFsStorage.cpp` | `HotSyncStorage` on a `FileSystem`, under `/palm` |
| `HotSyncLinks.h`, `HotSyncLinks.cpp` | `HotSyncLink` over `fnTcpClient` and over an `IOChannel`, and `HotSyncCradleLink`, the settle, deadline and count any serial cradle needs |
| `HotSyncSharedCradle.h` | `HotSyncSharedCradle`, a cradle on a line the bus owns and lends for each HotSync window |
| `HotSyncService.h`, `HotSyncService.cpp` | `HotSyncService`, the thread that listens and runs sessions |

## How it fits
- `src/main.cpp` starts one `HotSyncService` after the bus when the `[HotSync]` section of
  fnconfig.ini has `enabled=1` and an SD card is mounted. It serves network HotSync (TCP 14238),
  serial-over-TCP for POSE and CloudpilotEmu (TCP 6416), and a serial cradle: on FujiNet-PC the
  host device named by `serial_port`, on RS232 builds `serial_port=bus`, the bus line itself.
  The same settings are in the web UI's **Palm HotSync** panel; they take effect when FujiNet
  restarts.
- On the bus line, `rs232HotSync` in [lib/device/rs232/](../device/rs232/) is the
  `HotSyncSharedCradle`. A HotSync opens at 9600 baud, which the bus cannot read at its own rate, so
  while no host has sent FujiBus for a minute the service claims the line for 1.5 s windows and
  listens for a WAKEUP; bytes that are not FujiBus end that minute early. The
  `fujinet-rs232-s3-palm` board defaults to it; the cradle goes on the DB-9 through a null modem.
- `[HotSync] calendar` is a devicespec for FujiNet's own calendar protocols, without a view:
  `GCAL:///` for the calendars shown in Google Calendar, `GCAL://Work/` for one of them, or
  `ICAL://host/feed.ics`; `calendar_days_back` and `calendar_days_ahead` set the window. GCAL uses the
  Google grant made under **Google Account** in the web UI. Every ten minutes the service fetches
  the window, so a sync does not keep the Palm waiting, and each sync copies the events into
  DatebookDB, one way:
  - an event becomes an appointment; an all-day event an untimed one, repeating daily when it
    spans days; one that runs past midnight stops at 23:59; the location goes in the note;
  - an event that changes is rewritten, and one that is cancelled is deleted;
  - appointments made on the Palm are never touched, and neither is a copied one that was edited
    on the Palm and then cancelled in the calendar.
  Times are shown in `[General] timezone`; with none set, the service reads the Palm's offset from
  UTC off its clock.
- On the SD card, `/palm/install/` holds files to install on the next sync, `/palm/installed/`
  the ones already installed, `/palm/backup/<user>/` the backups, and `/palm/state/<user>/datebook.map`
  the Date Book records the calendar made.
- Everything up to `HotSyncSession` depends only on `include/global_types.h` and `fn_time`;
  `tests/HotSyncTests.cpp` drives it with frames captured from a Palm OS 3.3 device and with a fake
  device.
- `tools/hotsync-fakepalm/fake_palm.py` is a stand-in Palm that syncs over network HotSync, for
  testing a FujiNet or FujiNet-PC end to end without a device in a cradle.
- Design and the bus hand-off: `docs/hotsync.md`.
- Ported from [palm-sync](https://github.com/jichu4n/palm-sync) 0.2.1 (Apache-2.0); each header
  names the palm-sync source it follows.

## Build
ESP: every board, through the `lib/hotsync` glob in `src/CMakeLists.txt`; idle unless enabled.
PC: listed in `fujinet_pc.cmake`; `tests/CMakeLists.txt` also builds `hotsync_tests`.

## Notes
- The Date Book gets the calendar's events, but appointments made on the Palm do not go back to
  the calendar; Memo and Address are only backed up.
- The web UI does not show the last sync's result.
- Installs and backups hold a whole database in RAM.

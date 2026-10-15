# Trimui-GameSync

A focused game-library manager for the **TrimUI Brick Pro** and **TrimUI Smart Pro S**
handhelds (stock OS). It keeps the games on your microSD card in sync with your
private Google Drive game archive — nothing more, nothing less.

## Features (v1)

- **NLK intro** splash on every launch (any key skips; disable with `GS_NO_INTRO=1`,
  an `intro.off` / `.no-intro` file, or `"intro": false` in config)
- **Game library**: browse the games on your handheld by system, with search
- **Drive sources**: add one or more Google Drive folder links
  (`DriveLinks.txt`, or in-app later); no OAuth — a simple Drive API key is enough
  for link-shared folders
- **Compare & sync**: see what is missing or different between the handheld and
  Drive, then download with progress, pause/resume and MD5 verification
- **Fix misplaced files**: detect game files sitting in the wrong system folder
  and move them back, with an undo log
- **Silent auto OTA updates**: Ed25519-signed manifest, checked 30s after launch
  and every 10 minutes while the app is open (disable with `TRIMUI_GS_NO_OTA=1`)
- **About screen**: version (read from `VERSION` at runtime), donate QR

Out of scope on purpose: uploading to Drive, IPTV, music, web portals, cover-art
scraping. One job, done well.

## Layout on the SD card

```
/mnt/SDCARD/Apps/Trimui-GameSync/
├── bin/trimui-gamesync   # the app binary
├── assets/               # fonts, donate QR
├── certs/cacert.pem      # Mozilla CA bundle (stock OS has none)
├── config/               # system_map.json (Drive name -> Roms/ folder), user-editable
├── data/                 # settings, API key, DriveLinks.txt (never touched by OTA)
├── launch.sh             # dual-device launcher (Brick Pro / Smart Pro S)
├── ota-update.sh         # silent updater
└── VERSION               # current version, read at runtime
```

Games live in `/mnt/SDCARD/Roms/<SYSTEM>/` (`GBA`, `PS`, `MD`, …).
The default Drive-folder → system mapping is in `config/system_map.json`;
`GAMECUBE` is synced for storage but flagged as unsupported on the handheld.

## Build

Requires the TG5050 aarch64 SDK (same one used for the other TrimUI apps):

```sh
./build.sh        # cross-compiles to files/bin/trimui-gamesync
./package.sh      # zips dist/Trimui-GameSync-vX.Y.Z.zip for the SD card
```

## Third-party code

- `src/thirdparty/json.hpp`: [JSON for Modern C++](https://github.com/nlohmann/json)
  (nlohmann/json v3.11.3, single header, MIT License) — used only for
  parsing Google Drive API responses.

## OTA releases

Releases are published on GitHub; only the newest release is kept and marked
Latest. The update manifest (`ota/manifest.json`) is signed with an offline
Ed25519 key (`kid=gs1`); the binary embeds only the public key and verifies the
signature before trusting anything. User data (`data/`, `config/`, logs) is never
overwritten by an update.

## Controls

D-Pad to move, **A** to open/confirm, **B** to go back, **X**/**Y** for
context actions (per screen), **START** for options. Keyboard equivalents work
when testing on PC.

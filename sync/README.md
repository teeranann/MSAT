# MSAT Sync

Download titration runs from the MSAT device to a PC over WiFi, with optional
cleanup and folder protection. Windows front‑end (`msat-sync.bat`) drives the
worker (`msat-sync.sh`, runs under **Git Bash**).

## Requirements
- Windows + [Git for Windows](https://git-scm.com/download/win) (provides Git Bash)
- PC on the same WiFi as the device (default `192.168.1.200`)

## Use
Double‑click **`msat-sync.bat`** and pick from the menu:
```
[1] Sync files MSAT -> PC          (download new/updated runs)
[2] Clean up space on MSAT         (delete files older than N days)
[4] PROTECT MSAT-Output folder     (block accidental delete/rename)
[5] UNPROTECT MSAT-Output folder   (restore normal permissions)
[Q] Quit
```
- **Sync** writes runs into `MSAT-Output/` next to the script.
- **Cleanup** only deletes device files that are already backed up locally
  (size check) and older than the chosen days; you must type `DELETE` to confirm.
- **Protect** uses Windows `icacls` so files can be read/copied but not
  deleted/renamed (no admin needed — you own the folder).

## CLI flags (advanced)
`msat-sync.sh` accepts: `--mode=sync|cleanup|both`, `--cleanup-days=N`,
`--no-manifest` (default), `--unsafe-delete-unbacked`.

## How it gets the file list
The device exposes `/listfiles_raw?limit=all` (reads a cached index that the
firmware keeps current by appending each finished run). Sync downloads each
missing/updated file directly (no temp‑rename, so it works under folder
protection).

---
Copyright © 2026 Burapha University · PolyForm Noncommercial 1.0.0 · Patent pending No. 2603001145

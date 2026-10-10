# Failing components tracker (tissot 6.12 bringup)

Living list. Every entry names its evidence. Updated as work lands.
Last update: 2026-10-10 (snapshot58 run1).

## Snapshot58 run1 — what changed

Build: `out-snapshot58` via `scripts/build-ack-tissot.sh`, `6.12.93-4k-gf6c975d4f141`.
Image `porting/artifacts/snapshot58/boot.img` (36763648 bytes, SHA256
`c24dcb4e…1099e`), base ramdisk read off the phone (`boot_a`, 661 cpio
records). RAM-only `fastboot boot`; nothing flashed. USB capture is empty
(host reader lost the port), logdump + SD agree on 39500 bytes.

**Two config gates were silently dropping the whole wireless stack.**
`scripts/tissot-ack.config` asked for `CFG80211/MAC80211/WCN36XX =y` but
`gki_defconfig` carries `CONFIG_RFKILL=m`, and `CFG80211` has
`depends on RFKILL || !RFKILL` — that evaluates to *m*, capping every
dependent at `=m`. Separately `# CONFIG_WLAN_VENDOR_ATH is not set` hides
the whole `if WLAN_VENDOR_ATH` block in `drivers/net/wireless/ath/Kconfig`,
so `WCN36XX` was not a visible symbol at all. Fixed by adding
`CONFIG_RFKILL=y` and `CONFIG_WLAN_VENDOR_ATH=y` to the fragment. This is
the same class of failure `BUILD-snapshot42.md` documents for the other 17
symbols, and `merge_config.sh` reports success while it happens.

## Status key
- FIXING — patch in tree, awaiting build/boot verdict.
- BLOCKED — needs something external (rebuild, firmware harvest).
- SCOPED — cause + path known, work not started.
- PARKED — needs driver rewrite/port; see `BACKLOG-driver-rewrites.md`.
- WATCH — flaky/unexplained, monitoring across boots.
- CLOSED — not a bug / wontfix, with reason.

## In-tree (kernel) components

| Component | Symptom (evidence) | Status | Next action |
|---|---|---|---|
| ADSP FastRPC channel | s58: `adsp.mdt` fails `-2` — firmware missing, so no channel and no alias this run. The s57 label fix is untested | BLOCKED | Unblock needs the ADSP firmware harvest; the `qdsp_device` label patch stays in tree for when it lands |
| Touchscreen `edt-ft5406` probe | `-110` at 300/500 ms (s50/s52); re-toggle + 500 ms succeeds (s51); bounded retry clean s53–s56 | CLOSED | Fix verified 4 boots; keep in tree |
| `pclk0 update_config` WARN | `clk-rcg2.c:133` ×2 in 12/16 runs (s48, s49 r4–r6, s51–s57 except s55 r2). Cause: RCG CMD_UPDATE cannot latch while the DSI PLL is unprepared | **CLOSED** | `CLK_OPS_PARENT_ENABLE` (`4c7eb3f0d`) verified: 0 occurrences in s58 run1. Keep in tree |
| WLAN `wcn36xx` + pronto PIL | s58: `CFG80211/MAC80211/WCN36XX` now genuinely `=y` (was `=m`, silently). `wcnss.mdt` still fails `-2`, so remoteproc1 powers up with no firmware | FIXING | Config gates fixed; the remaining blocker is the firmware harvest, same as ADSP |
| QSEE listener registration | SMC `0x32000206` `ret=-5` ×8/boot; 8 siblings succeed same path | CLOSED | TZ refuses those IDs; tolerated; no kernel fix applies |
| `mdp5 INTF1_UNDER_RUN` | Single `errors: 04000000` at first modeset, panel healthy after | CLOSED | Benign bringup transient |
| Coldboot time | 34.3 / 11.1 / 13.7 / 13.7 / 12.6 / 16.4 s vs stock 2.8 s (s50–s56) | WATCH | ~11–18 s band; s50 34 s the outlier. Bisect if a run exceeds ~25 s |
## Proprietary (vendor blobs / downstream drivers)

| Component | Symptom (evidence) | Status | Next action |
|---|---|---|---|
| GPU 3D (`kgsl`, EGL blob `libGLESv2_adreno.so`) | SF fatal `no suitable EGLConfig`, tombstones (every run) | PARKED | kgsl rewrite, backlog #1 |
| ADSP firmware (`adsp.mdt`+segs) | remoteproc `-2`; absent from `/vendor/firmware` (18 files, no adsp) | BLOCKED | Harvest from modem partition or payload.bin (backlog #2 pattern) |
| WCNSS firmware (`wcnss.mdt`+`.bXX`) | Not in vendor; stock serves from `/firmware/image` (modem partition) | BLOCKED | Same harvest as ADSP; pronto fails fast until staged |
| Camera stack (`msm_camera`, CPP, sensors, HAL) | Nothing probed; blobs (`cpp_firmware_*`) present but no kernel side | PARKED | CAMSS/Venus 8953 variant + sensor/HAL project (backlog #4) |
| Fingerprint (`goodix_fp`) | Stock `/dev/goodix_fp`; no in-tree driver, no DT node | SCOPED | Downstream driver port + DT (backlog #7) |
| Modem packet path (IPA/RMNET) | QRTR signalling works; no IPA v2 in-tree, RMNET off | SCOPED | IPA-v2 backport (backlog #6) |
| EGL/GLES userspace | Present on vendor, aborts without `/dev/kgsl-3d0` | PARKED | Unblocked by kgsl rewrite, nothing to do meanwhile |

## Working (do not regress)
wled OVP IRQ pairing (0 `enable_irq` WARN s50–s52) · GPU fw load
(`qcom/*.fw` from ramdisk) · DSI/panel (`otm1911` status `0x9c`) ·
eMMC HS400 + SD SDR104 · USB adb handoff · QSEE SMC
(`LAST=SMC_RET -> died outside SMC`) · ramoops/logdump/SD/USB evidence
channels · Bluetooth `BT_QCA` built (unexercised) · `max98927` probes.

Verified again in s58 run1: `ft5x06` probes clean (`input: generic ft5x06
(3b) as /devices/platform/soc@0/78b7000.i2c/i2c-1/1-0038/input/input2`),
SD comes up at SDR104, `dsi_err_worker: status=5` only, and both logdump
slots (`seq=165`, `seq=166`, console `DBGC`) plus the SD mirror agree
byte-for-byte on 39500 bytes. Boot stops in the userspace bringup: the
recorder is denied `{write}` on `/kmsg` and `/mmcblk0p48` by SELinux, then
the log ends at ~37 s — before `sys.boot_completed`, so the 360 s watchdog
reboots the phone back to ROM.

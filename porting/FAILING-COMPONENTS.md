# Failing components tracker (tissot 6.12 bringup)

Living list. Every entry names its evidence. Updated as work lands.
Last update: 2026-10-09 (snapshot53 run1).

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
| ADSP FastRPC channel | FW boots + APR live; channel binds, `/dev/fastrpc-adsp` live; alias `/dev/adsprpc-smd` created (s57) but daemon avc-denied it (tmpfs context) | FIXING | Stock identity (`0664 system:system qdsp_device`) now applied in-tree; needs build + boot |
| Touchscreen `edt-ft5406` probe | `-110` at 300/500 ms (s50/s52); re-toggle + 500 ms succeeds (s51); bounded retry clean s53–s56 | CLOSED | Fix verified 4 boots; keep in tree |
| `pclk0 update_config` WARN | `clk-rcg2.c:133` ×2 in 12/16 runs (s48, s49 r4–r6, s51–s57 except s55 r2); ×0 in s49 r1–r3, s50, s55 r2. Fires in `dsi_link_clk_set_rate_6g` before link clocks/DSI PLL are on | FIXING | `CLK_OPS_PARENT_ENABLE` on gcc-msm8953 byte/pclk RCGs; verify 0 WARNs over ≥3 boots |
| WLAN `wcn36xx` + pronto PIL | Pronto boots, NV acked, `WCNSS Version 1.5 1.2`, cold boot never completes; sysmon enabled s56, still silent (no crash — FW waiting, not dead) | FIXING | Next suspects: iris/RF power evidence, caldata; wcn36xx still channel-starved |
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

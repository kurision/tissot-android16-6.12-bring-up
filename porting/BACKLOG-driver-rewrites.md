# Driver-rewrite backlog (display parked 2026-10-09)

> **Current state: CURRENT-STATE.md.** The entries below are the
> running log and are ordered newest first; the top entry is not
> necessarily the current state. Build, package and flash:
> [BUILD_TISSOT.md](../BUILD_TISSOT.md).
>
> The per-run evidence this document cites (boot images, kernel directories,
> init binaries, logdumps) is no longer in the repository; it is
> git-ignored and disposable, and the boot images are in
> ../ack-kgsl-artifacts.

Things that cannot be fixed without a driver rewrite or a userspace project.
Kernel-side small fixes continue under the snapshot runs; this list only
collects the big ones with their unblock plans.

## 1. kgsl (`/dev/kgsl-3d0`) — display EGL/HWC
- Symptom (snapshot49 run6): adreno 506 probes, all firmware loads
  (`qcom/a530_pm4.fw`, `qcom/a530_pfp.fw`, `qcom/a506_zap.mdt`), `renderD128`
  live — but vendor EGL (`libGLESv2_adreno.so`, 2020 QC blob) aborts with
  `open(/dev/kgsl-3d0): ENOENT` → `EGL_BAD_ALLOC`. sdm HWC is the same era.
- Cost: downstream kgsl is 74,380 lines / 88 files (`kernel_xiaomi_msm8953_fork`).
  Blockers on 6.12: ION removed (→ dma-buf heaps), sync-framework removed
  (→ dma_fence), msm_bus removed (→ interconnect), IOMMU core churn, exact
  ioctl-ABI preservation for the 2020 blobs. Estimate 4–8 engineer-weeks,
  phone-boot-only validation.
- Alternative: mesa freedreno + drm_hwcomposer (kernel side already done;
  similar-size userspace project, upstream-maintained).
- Decision: stay headless for kernel bringup. All evidence channels work
  with surfaceflinger looping.

## 2. ADSP firmware (`adsp.mdt`) — audio/sensors
- Symptom: `remoteproc0: Direct firmware load for adsp.mdt failed (-2)`,
  adsprpcd restart loop. Kernel driver (`qcom_q6v5_pas`) is fine; only the
  blobs are missing from the ramdisk search path.
- Unblock (no code needed): extract `adsp.mdt` + segments from
  `../EvolutionX-*-tissot-*.zip` `payload.bin` → modem image → inject with
  `package-diag-boot.py --firmware-dir` (same mechanism as GPU fw, proven).
  Watch 64 MB image limit (currently ~35 MB).
- Defer until audio/sensors matter; harmless `-ENOENT` meanwhile.

## 3. DSI pixel clock (`pclk0 update_config` WARNING)
- Symptom: `clk-rcg2.c:133` WARN ×2 per boot from
  `dsi_link_clk_set_rate_6g ← msm_dsi_host_power_on`; display still binds.
- Effectively persistent: ×2 in 12/16 runs (s48, s49 r4–r6, s51–s57 except
  s55 r2), always before `clk_disable_unused`. Cause: RCG CMD_UPDATE cannot
  latch while the DSI PLL is unprepared (rates set before
  `dsi_link_clk_enable_6g`).
- FIXING: `CLK_OPS_PARENT_ENABLE` on gcc-msm8953 byte0/byte1/pclk0/pclk1
  RCGs (dispcc-qcm2290/sm6115 pattern); verify 0 WARNs over ≥3 boots.

## 4. Camera (msm_camera stack: CAMSS/VFE, VENUS, sensors, ISP)
- Symptom: nothing probed — `CONFIG_VIDEO_QCOM_CAMSS`/`VENUS` unset, no DT
  nodes; snapshot50 `kernel.config` + `msm8953-xiaomi-tissot.dts` confirm.
- Kernel side is bounded: CAMSS already covers six SoCs (`camss.c:2447-52`,
  up to sdm845) and Venus nine (`core.c:937-45`) — an 8953 variant follows
  the sdm660 precedent (new compatible + data tables). The real project is
  sensor/actuator drivers plus the Camera HAL.
- Unblock: out of scope for boot bringup; park until display (1) is live,
  then size the port (expect multi-week, phone-camera validation).

## 5. WLAN (upstream wcn36xx candidate — NOT a Prima port, verify first)
- Correction (2026-10-09): in-tree `drivers/net/wireless/ath/wcn36xx`
  exists and `wcn36xx.h:33` references `wlan/prima/WCNSS_qcom_wlan_nv.bin`
  — the exact file stock ships in `/vendor/firmware/wlan/prima/`. Path is
  enablement (WCNSS PIL + pronto remoteproc + DT + `CONFIG_WCN36XX`),
  per `porting/plan/04-phase4-wlan.md`. Not a rewrite unless the WCN rev
  proves incompatible.
- Caveat: that plan's config section names ATH10K/ATH10K_PCI — suspect on
  a phone (no PCI WLAN); expect `WCN36XX`, verify at build time.

## 6. Modem data path (IPA v2 backport + RMNET)
- In-tree `drivers/net/ipa` starts at v3.1/msm8998 (`ipa_main.c:649-682`);
  no 8953/IPA-v2 match, no DT nodes. QRTR signalling works; packet path
  needs an IPA-v2 backport plus RMNET enablement. Port, not config.

## 7. Fingerprint (Goodix, downstream `goodix_fp` port)
- Stock exposes `/dev/goodix_fp`; no in-tree driver, no DT node. Needs the
  downstream driver ported plus DT (SPI/platform wiring TBD). Small port,
  blocked behind nothing kernel-side.

## Closed (not kernel bugs)
- `dpmd` SIGABRT `ProcessState already initialized with a different mmap
  size`: vendor-userspace libbinder mismatch; kernel binder proven healthy
  (keymaster/servicemanager IPC + SMC traces).
- GPU `vdd/vddcx dummy regulator`: upstream norm — no 8953 board wires them;
  power is via `OXILI_GX_GDSC` + rpmpd OPPs, both live. Cosmetic.
- `udc failed to start g1: -19` + configfs `EBUSY`/`EEXIST`, `vold` slot
  failure, audio-hal/qrtr missing-service noise: diag-coexistence and
  vendor/ROM mismatch, benign for capture.
- `[Firmware Bug] Kernel image misaligned at boot`: bootloader load address,
  cosmetic.
- QSEE listener `REGISTER_LISTENER` SMC `0x32000206` `ret=-5` (EIO) ×8 per
  boot (snapshot50 `run1-logdump.bin`, same 8 since snapshot28): TZ refuses
  those listener IDs while 8 sibling registrations through the identical
  HLOS path succeed (`listener N registered sb 0xff0xxxxx`). Faithful
  forward, tolerated by userspace — TZ-side, no kernel rewrite applies.

# Driver-rewrite backlog (display parked 2026-10-09)

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
- Display-path only, non-fatal for headless runs, unvalidatable without (1).
  Parked with display.

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

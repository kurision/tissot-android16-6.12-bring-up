# Open components and daily-driver blockers — tissot

Camera/status and the overnight failure audit updated **2026-10-24**; other
subsystem observations remain dated **2026-10-22**. This is the current work queue, not a chronological bring-up
log. See [CURRENT-STATE.md](CURRENT-STATE.md) for evidence and limits,
[README.md](README.md) for the overview, and [BUILD_TISSOT.md](../BUILD_TISSOT.md)
for build/packaging instructions.

Latest camera inspection: **`6.12.93-4k-gd13339db2abb #44`**. Source is
`ack-camera`, with JPEG compatibility fix and test committed as
**`4646f53406c3`**. That fix is host-tested, **not built or boot-tested**.
Older #83 audio/power/unplug evidence below has not been revalidated on #44.
Daily-driver readiness remains open.

The [2026-10-24 failure inventory and repair ledger](findings/2026-10-24-live-failure-inventory.md)
records current dpmd/peripheral/FastRPC failures, modem fatal/recovery evidence,
USB resume errors and all captured warning/error/fatal loggers. Raw logs,
line-level indexes, provenance labels and checksums are retained locally under
`porting/artifacts/snapshot60/failure-audit-20261024/`. Counts describe retained
windows, not a complete overnight trace or measured battery attribution.

## Active repairs — one component at a time

| Component | Repair / evidence | Current status |
|---|---|---|
| dpmd / HIDL — current repair | Reproduced mmap-size abort on run401 #46; hash-checked `porting/tools/patch-dpmd-hidl.py` aligns dpmd's explicit request with the installed HIDL default. Nine focused host tests passed. | Temporary candidate active on run401: two service starts stable for 120.647 s and 60.695 s, verified executable hashes/domain and no new matching aborts. Original file retained; reboot removes binding. Persistent integration, data-service qualification and battery benefit remain open. |
| USB / DWC3 — source candidate | Retained prefix reports QUSB2 PLL failure / resume -16; live UDC not attached. Added two donor suspend quirks under board `&usb3_dwc3`. | Strict checkpatch/whitespace passed; not built or boot-tested. No proven PLL root cause or recovery yet. |
| Crash-loop stop guards | Proposed dpmd/peripheral guards were removed after review. | Withdrawn; restoring the original file is not a crash fix. No current guard-based repair claim. |
| Peripheral manager / proxy | Missing SSR discovery root and repeated manager/proxy exits. | Open; compare donor discovery and lifecycle before compatibility changes. |
| FastRPC / adsprpcd | Thousands of listener initialization errors and 25 ms retries; ENOSPC reported. | Open; trace failing operation/allocation. Not evidence of full storage; earlier ioctl fixes do not establish recovery. |
| Modem firmware / remoteproc | Earlier captured summary includes IPA assertion and remoteproc recovery, `handling crash #10`. | Open; full raw repeat and transaction correlation required. Counter is not ten independently retained reproductions. |
| Camera LED flash / torch | Run401 HAL reports no flash on either camera; no torch LED node. Donor PMI8950 flash/torch resources and camera link are omitted from the port. | Confirmed missing hardware integration, not an app setting. Needs a separately reviewed/approved port; no source edit or LED activation performed. |

Existing camera/audio fixes and qualification requirements below remain open.

## Current issues

| Priority | Component / symptom | What is established | Next check |
|---|---|---|---|
| 1 | Unresponsive phone after charger removal | User unplugged, then needed a long Power press. Previous console ends at 4323.729 s; last battery reading was 87%, 4.223 V, 25.7°C. No recorded panic/orderly shutdown; reset slot is stale. | Reproduce unplug / screen-off / wake with live logs and boot identity. Do not assign a Wi-Fi, heat, battery or suspend cause without a failing trace. |
| 1 | Panic / freeze observability | Ramoops console retention works. Panic persistence on #83 is unverified. Android disables hung-task detection; buddy mode cannot detect all-CPU freezes. | Review detection coverage; a separately approved controlled panic/recovery is needed to qualify capture on the exact image. Do not infer a broken writer solely from missing panic files. |
| 1 | Earlier #83 audio regression | #83 contains legacy-format experiment `c23444468`; newer source reverts it in `555056679` after unsupported playback-open responses and donor include-chain correction. Audio has not been requalified on #44. | Verify playback on a user-built image containing the revert. Do not re-enable the legacy property based on the wrong donor include chain. |
| 1 | Voice-call audio | Modem bring-up is working, but CVP channel-info command `0x1133d` is rejected with `ADSP_EBADPARAM`. Voice front ends and extra diagnostics are committed. | Compare the actual donor tissot channel/topology configuration and firmware replies; working modem registration is not a call-audio verdict. |
| 1 | Corrupt camera stills / JPEG hardware reset | #44 creates `/dev/jpeg0`, but both fresh rear/front captures hit `legacy_msm_jpeg_ioctl_reset:962` and return `-EFAULT`; the HAL falls back to hybrid encoding. Both saved JPEGs lack DRI and reproduce decoder warnings. `4646f53406c3` fixes six reset/buffer/event compatibility paths and passes host checks. | Build/test the fix; confirm hardware reset and capture complete without fallback, and decode new rear/front files without warnings. Full photo recovery is unverified. |
| 2 | Audio EOS close timeout / low-latency underruns | Timeouts repeated even with successful APR TX; some rendered-EOS events arrived. Deep-buffer playback ran. EOS traces were reverted in `833a2ad73`; timeout errors remain. | Reproduce on the current audio source, correlate session/stream completion and buffer state, and compare donor behavior. Do not increase the timeout speculatively. |
| 2 | Suspend / resume reliability | Earlier USB/ADB and Wi-Fi wake failures remain incompletely qualified. `67818f827` fixes a separately captured QUP/touch resume SError; it does not explain the latest stop. | Repeat screen-off/wake and charger transitions, including with and without an active video session; capture failed suspend/resume paths. |
| 2 | Storage permissions after sdcardfs transition | Screen recording failed to save with `EACCES` in a mode-2700 `ScreenRecords` directory. Changing only it to 2775 produced a saved 26,553,766-byte MP4. Donor used sdcardfs; port MediaProvider sees F2FS backing storage. | Determine how restrictive metadata arose and whether other existing directories are affected. No general migration fix or ROM/kernel attribution is proven. |
| 2 | Battery efficiency / thermal behavior | Earlier CPU idle and charging observations remain valid for their captured image; a brief 78.2°C compilation peak was not an idle baseline. Camera bring-up currently re-enables periodic ramoops disk snapshots, so the older diagnostic-cleanup claim does not describe current overhead. | Measure the actual packaged configuration, screen-off drain, wakeups, charging trend and sustained mitigation after background dexopt; account for active camera diagnostics. |
| 2 | Thermal HAL / vendor policy integration | Earlier Android thermal service reported `HAL Ready:false` while legacy thermal-engine ran. Thermal zones/cooling devices are registered and donor-compatible names are restored. | Recheck the exact image's HAL/daemon integration and sustained CPU/GPU mitigation. Do not invent a case-sensor mapping. |
| 2 | Long-run GPU/display stability | Earlier GPU retirement stalls remain a historical open issue; the new media fixes do not establish that every GPU hang is resolved. No retirement-stall reproduction was captured in this session. | Exercise sustained graphics and inspect retired timestamps, pending fences and fault logs if the display hangs; keep this separate from the unplug event. |
| 2 | Camera orientation / capture qualification | #43 has working front/rear preview with upside-down rear orientation. QRD mount-angle correction `b74883f50` is in #44's reported source ancestry; visual orientation has not been revalidated. ACK exposed two cameras versus three on the donor. | Check rear orientation/motion and retain front as control; separately qualify zoom, extra sensor/flash coverage and repeated preview/capture handoff. |
| 2 | Initial camera failure on run401 / LED flash unavailable | User reports camera failed initially and then worked. Failure evidence saved before dpmd mutation; exact cause unverified. HAL explicitly reports no flash, and the port omits the donor PMI8950 flash path. | Retain the transient launch issue separately from the confirmed flash integration gap. Diagnose a repeated launch failure; trace donor flash resources/driver/ABI before proposing edits. |
| 3 | Hardware video encode / performance | Software AVC selection matches the same ROM on donor 4.19; OMX encoders are absent from the framework list on both. Port VIDC encoder/decoder nodes answer QUERYCAP, and #43 has hardware HEVC decode evidence. QUERYCAP does not prove a completed hardware encode. | Treat encoder registration/selection as a ROM/framework follow-up; qualify hardware encoding with a raw V4L2 test if needed. Sustained decode, stop/error cleanup and the observed MP4 timestamp warning remain unqualified. Secure playback is out of scope. |
| 3 | Fingerprint / haptics / IR / remaining sensors | Last dedicated inventory lists unported device-specific paths; no fresh end-to-end validation in this session. | Use [the hardware inventory](TRACKER-tissot-hardware.md) and verify each path before changing its status. |

## Fixes already landed; avoid reopening them from old reports

| Commit / change | Result / remaining limit |
|---|---|
| `5cff6896e` — ION partial cache sync | Restore physical SG boundaries and original entry counts; old production helper fails focused host tests, patched helper passes. Later recording did not repeat the captured ION oops; repeated hardware-decode coverage remains necessary. |
| `defe506a4` — VIDC debugfs | Atomic instance names, error-pointer checks, failed-creation cleanup and unavailable-parent handling. Later recording attempt survived the previously captured debugfs-oops trigger. |
| `c2b2e78a9` — rotator MDP clock vote | Present in later media images; the README's old "committed, not built" status was stale. Do not confuse the old blue-frame failure with the separately captured ION/debugfs crashes. |
| `0ba1142af`, `37d207627`, `ec8ac3f59`, `906b00d7f` — camera ABI and CPP | Restore time32 layouts, CPP resources/power/probe lifetime and vendor entity-revision semantics. Combined changes reached working preview on user #43; individual commits were not boot-tested in isolation. |
| `aa6d022dc` — JPEG hardware resources / power | Restores encoder DT, GCC domain and APPS SMMU bank `0x13`. #44 has a bound driver and `/dev/jpeg0`; successful hardware encoding is still blocked by the compatibility fault above. |
| `d13339db2`, `6aba140ac` — software AVC comparison | Record matches-stock encoder selection and framework-list evidence. No new kernel defect is established by software selection; hardware encode remains unqualified. |
| `19ab86119` — per-core power collapse | `psci_idle`/`menu` and accepted residency verified on all eight CPUs. This is not a battery-savings measurement or cluster/system-collapse validation. |
| `1fcfd0efe`, `2992ab58e`, `84cec3f4f` | Vendor irqbalance sleep interface, unchanged RPM-vote suppression and GPU latency-vote affinity. Benefits need runtime measurement. |
| `97afb7703`, `f431b1110` — EOS diagnostics | Used for investigation, then reverted together by `833a2ad73`; they were not EOS fixes. |
| Earlier diagnostic cleanup through `07ad1d695` | Removed QSEE/trace overhead and bounded init capture. Current camera source has periodic ramoops snapshots enabled again; do not use this older cleanup as proof of current idle overhead. |

## Validation and evidence rules

The media regression suite passed with GCC/Clang ASan/UBSan; strict checkpatch
and whitespace checks passed for the fixes. No kernel build, flash or controlled
panic was performed by this session. The user supplied the observed images.
The latest JPEG change passes **42 camera host tests plus the routing test**;
its original-code control fails, while patched ASan/UBSan/leak checks pass.
Strict driver checkpatch is clean; the new test has only the generic
MAINTAINERS reminder. No kernel compile, Sparse or patched-device test ran.

Selected captures are preserved under
`porting/artifacts/snapshot60/status-20261022/` with `SHA256SUMS`. These files
include **untracked text and gitignored binary captures**, not committed evidence.
Do not delete the only copy during artifact cleanup. CPU/charging sample numbers
are session observations, not a retained
benchmark or a donor comparison.
Retained camera evidence and the newer #44 session observations are detailed
in [CURRENT-STATE.md](CURRENT-STATE.md); the #43 camera handoff predates them.

The shared CPU policy follows the supported clock/voltage arrangement. Low
aggregate utilization plus a high shared frequency is not by itself a defect:
one busy core, a boost or a real-time task can request that performance level.
Efficiency tuning needs measured workload and wakeup evidence.

Existing dated reports/plans are retained as history. Their run-specific
"current", "unbuilt" or "working" labels do not override these live docs.

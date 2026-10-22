# Open components and daily-driver blockers — tissot

Updated **2026-10-22**. This is the current work queue, not a chronological
bring-up log. See [CURRENT-STATE.md](CURRENT-STATE.md) for evidence and limits,
[README.md](README.md) for the overview, and [BUILD_TISSOT.md](../BUILD_TISSOT.md)
for build/packaging instructions.

Last inspected image: **`6.12.93-4k-gc23444468a04 #83`**, slot b. Source at this
update: **`07ad1d695`**. Newer source changes must not be presented as verified
on that flashed image. Daily-driver readiness remains open.

## Current issues

| Priority | Component / symptom | What is established | Next check |
|---|---|---|---|
| 1 | Unresponsive phone after charger removal | User unplugged, then needed a long Power press. Previous console ends at 4323.729 s; last battery reading was 87%, 4.223 V, 25.7°C. No recorded panic/orderly shutdown; reset slot is stale. | Reproduce unplug / screen-off / wake with live logs and boot identity. Do not assign a Wi-Fi, heat, battery or suspend cause without a failing trace. |
| 1 | Panic / freeze observability | Ramoops console retention works. Panic persistence on #83 is unverified. Android disables hung-task detection; buddy mode cannot detect all-CPU freezes. | Review detection coverage; a separately approved controlled panic/recovery is needed to qualify capture on the exact image. Do not infer a broken writer solely from missing panic files. |
| 1 | Audio regression in the last inspected image | #83 contains legacy-format experiment `c23444468`; newer source reverts it in `555056679` after unsupported playback-open responses and donor include-chain correction. | Verify playback on the next user-built image containing the revert. Do not re-enable the legacy property based on the wrong donor include chain. |
| 1 | Voice-call audio | Modem bring-up is working, but CVP channel-info command `0x1133d` is rejected with `ADSP_EBADPARAM`. Voice front ends and extra diagnostics are committed. | Compare the actual donor tissot channel/topology configuration and firmware replies; working modem registration is not a call-audio verdict. |
| 2 | Audio EOS close timeout / low-latency underruns | Timeouts repeated even with successful APR TX; some rendered-EOS events arrived. Deep-buffer playback ran. EOS traces were reverted in `833a2ad73`; timeout errors remain. | Reproduce on the current audio source, correlate session/stream completion and buffer state, and compare donor behavior. Do not increase the timeout speculatively. |
| 2 | Suspend / resume reliability | Earlier USB/ADB and Wi-Fi wake failures remain incompletely qualified. `67818f827` fixes a separately captured QUP/touch resume SError; it does not explain the latest stop. | Repeat screen-off/wake and charger transitions, including with and without an active video session; capture failed suspend/resume paths. |
| 2 | Storage permissions after sdcardfs transition | Screen recording failed to save with `EACCES` in a mode-2700 `ScreenRecords` directory. Changing only it to 2775 produced a saved 26,553,766-byte MP4. Donor used sdcardfs; port MediaProvider sees F2FS backing storage. | Determine how restrictive metadata arose and whether other existing directories are affected. No general migration fix or ROM/kernel attribution is proven. |
| 2 | Battery efficiency / thermal behavior | CPU idle states are used on all eight cores. Charging telemetry works. Compilation caused a brief 78.2°C sensor peak; later readings cooled. Source removes diagnostic I/O, but the last image predates cleanup. | Verify the packaged cleanup, then measure screen-off drain, wakeups, charging trend and sustained mitigation. Use a quiet baseline after background dexopt. |
| 2 | Thermal HAL / vendor policy integration | Earlier Android thermal service reported `HAL Ready:false` while legacy thermal-engine ran. Thermal zones/cooling devices are registered and donor-compatible names are restored. | Recheck the exact image's HAL/daemon integration and sustained CPU/GPU mitigation. Do not invent a case-sensor mapping. |
| 2 | Long-run GPU/display stability | Earlier GPU retirement stalls remain a historical open issue; the new media fixes do not establish that every GPU hang is resolved. No retirement-stall reproduction was captured in this session. | Exercise sustained graphics and inspect retired timestamps, pending fences and fault logs if the display hangs; keep this separate from the unplug event. |
| 3 | Hardware video encode / performance | Successful screen recording used software `c2.android.avc.encoder`, not proof of Venus encode. Hardware AVC decode was exercised with 1920×816/24 fps content; user reports 1080p works. | Test hardware encoder selection independently; qualify full 1920×1080 and other supported formats, stop/error cleanup and sustained playback. Secure playback is out of scope. |
| 3 | Cameras | CAF camera stack, sensor/power and vendor camera ABI are not ported. | Identify the installed sensors and port the donor camera path as its own task; screen recording does not validate cameras. |
| 3 | Fingerprint / haptics / IR / remaining sensors | Last dedicated inventory lists unported device-specific paths; no fresh end-to-end validation in this session. | Use [the hardware inventory](TRACKER-tissot-hardware.md) and verify each path before changing its status. |

## Fixes already landed; avoid reopening them from old reports

| Commit / change | Result / remaining limit |
|---|---|
| `5cff6896e` — ION partial cache sync | Restore physical SG boundaries and original entry counts; old production helper fails focused host tests, patched helper passes. Later recording did not repeat the captured ION oops; repeated hardware-decode coverage remains necessary. |
| `defe506a4` — VIDC debugfs | Atomic instance names, error-pointer checks, failed-creation cleanup and unavailable-parent handling. Later recording attempt survived the previously captured debugfs-oops trigger. |
| `c2b2e78a9` — rotator MDP clock vote | Present in later media images; the README's old "committed, not built" status was stale. Do not confuse the old blue-frame failure with the separately captured ION/debugfs crashes. |
| `19ab86119` — per-core power collapse | `psci_idle`/`menu` and accepted residency verified on all eight CPUs. This is not a battery-savings measurement or cluster/system-collapse validation. |
| `1fcfd0efe`, `2992ab58e`, `84cec3f4f` | Vendor irqbalance sleep interface, unchanged RPM-vote suppression and GPU latency-vote affinity. Benefits need runtime measurement. |
| `97afb7703`, `f431b1110` — EOS diagnostics | Used for investigation, then reverted together by `833a2ad73`; they were not EOS fixes. |
| Latest diagnostic cleanup through `07ad1d695` | Removes periodic/QSEE/trace overhead and bounds init capture. Source-landed; not verified on the last inspected #83 image or its ramdisk. |

## Validation and evidence rules

The media regression suite passed with GCC/Clang ASan/UBSan; strict checkpatch
and whitespace checks passed for the fixes. No kernel build, flash or controlled
panic was performed by this session. The user supplied the observed images.

Selected captures are preserved under
`porting/artifacts/snapshot60/status-20261022/` with `SHA256SUMS`. These files
include **untracked text and gitignored binary captures**, not committed evidence.
Do not delete the only copy during artifact cleanup. CPU/charging sample numbers
are session observations, not a retained
benchmark or a donor comparison.

The shared CPU policy follows the supported clock/voltage arrangement. Low
aggregate utilization plus a high shared frequency is not by itself a defect:
one busy core, a boost or a real-time task can request that performance level.
Efficiency tuning needs measured workload and wakeup evidence.

Existing dated reports/plans are retained as history. Their run-specific
"current", "unbuilt" or "working" labels do not override these live docs.

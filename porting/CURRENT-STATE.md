# Current state of the tissot port

Camera checkpoint updated **2026-10-24**; other subsystem status below
remains dated **2026-10-22**. This is the detailed live status; the
[README](README.md) is the overview and [failing-components table](FAILING-COMPONENTS.md)
is the work queue. Old dated reports describe their own runs, not this state.

## Camera milestone, 2026-10-24

The user's `6.12.93-4k-g6b7fe2b3628d-dirty #43` reaches CPP hardware
initialization (`0x40030003`) and working front/rear preview. Rear preview
is upside down, saved JPEGs are corrupt, and the captured videos use
software `c2.android.avc.encoder`. Camera bring-up is not complete.

Source through `906b00d7f` commits the combined CPP/ABI work used for that
milestone. `b74883f50` separately corrects the donor QRD mount angles;
that newer correction is host-tested and **not boot-tested**. The dirty
#43 identity is not an exact source or packaged-image hash. No build,
flash or phone boot was run by the agent for this checkpoint.

Start the next session with [the camera handoff](HANDOFF-camera-2026-10-24.md).
It links the stock and ACK captures, focused checks, commits and next steps.
All local capture originals remain under `artifacts/snapshot60/`; large
artifacts are not included in these commits and must be retained separately.
The older main-image provenance and other subsystem statements below have
not been revalidated by this camera checkpoint.

## Earlier main image and source provenance, 2026-10-22

| Item | Evidence / state |
|---|---|
| ROM / device | Evolution X / Android 17, Xiaomi Mi A1 (`tissot`, MSM8953), serial `2819f2320604` |
| Last observed running image | `6.12.93-4k-gc23444468a04 #83`, built 2026-10-22 20:56:07 +0545; boot slot b |
| Last inspected boot ID | `4aba5d4d-433d-4266-97cc-191fbf70f3b1`; inspected after the user recovered an unresponsive phone |
| Earlier media verification image | `#80`, `g97afb7703a3c-dirty`; recording/permission repair observations below |
| Source at this update | `07ad1d695` on `main`; ahead of the last inspected image |
| Root / transport | KernelSU-Next; USB ADB and wireless ADB both used for read-only inspection |

The user built/flashed the observed images. No kernel build, flash, forced
panic or host-initiated reboot was performed for the latest investigations.
Configuration/source inspection does not establish runtime behavior on a
new image. In particular, #83 still contains the audio experiment that newer
source reverts, and predates the latest diagnostic-overhead cleanup.

## Working paths and their limits

| Area | Latest supported statement |
|---|---|
| Android / display / input | Boots to Android UI with vendor graphics, touch, navigation keys and root. Earlier GPU/display faults are not a blanket explanation for the new unplug event. |
| CPU frequency | One `schedutil` policy for CPUs 0–7, 480–2784 MHz; frequency residency demonstrates both low and high rates. |
| CPU idle | `psci_idle` and `menu` active. Accepted standalone power-collapse residency increases on all eight CPUs. Energy savings, cluster/system collapse and screen-off endurance are not established. |
| GPU | Vendor GLES on KGSL works. Available frequencies span 100–725 MHz; latest light-load samples reported 320 MHz. This readout alone does not establish clock-gating or power consumption. |
| Video decode | Hardware OMX AVC decoding observed; user confirms 1080p playback. Captured sample: 1920×816, 24 fps, approximately 23.95 delivered fps over 109 seconds, Venus at 114.3 MHz. Full 1920×1080, other codecs and secure playback are not qualified; secure playback remains out of scope. |
| Video stop | The inspected stop sequence removed the decoder and released Venus clocks/power domains. It does not prove every error path releases resources. |
| Screen recording | Later recording attempts reached encoding and video-instance closure without either captured #79 oops. The successful save used software `c2.android.avc.encoder`; hardware encode remains unverified. |
| Audio playback | Speaker playback was demonstrated on earlier builds. Deep-buffer 48 kHz stereo 16-bit counters advanced; low-latency underruns and EOS close timeouts remain. Audio quality has not been benchmarked. |
| Connectivity | Wi-Fi association, Bluetooth ON/operation and modem bring-up demonstrated. Voice-call audio and reliable suspend recovery remain open. |
| Battery / charger | Battery and charger interfaces report values and distinguish charger removal. Reported health is not a battery-wear or capacity test. |

## Media fixes and screen-recording save repair

Two distinct #79 failures were captured; they must not be merged into one
root-cause claim.

* **ION partial synchronization (`5cff6896e`).** YouTube/video buffer queueing
  oopsed in `dcache_clean_poc_nosync` through
  `ion_sgl_sync_range -> ion_dma_buf_end_cpu_access_partial -> msm_vidc_qbuf`.
  A merged IOVA segment's 0x6c0000-byte synchronization was treated as a
  contiguous physical span, faulting at `0x81800000`. The fix restores physical
  SG-chunk boundaries, passes `orig_nents` through partial/VMA callers and
  validates ranges before synchronization.
* **VIDC debugfs (`defe506a4`).** A screen-recording attempt oopsed at uptime
  427.740 s in `msm_vidc_debugfs_init_inst` after a duplicate
  `inst_0000000000000000` directory returned `ERR_PTR(-EEXIST)`. Permission-
  dependent `%pK` names are replaced with atomic instance numbers; error
  pointers, partial cleanup and unavailable parents are handled safely.

Host regressions extract the production C. Both old failures reproduce under
GCC and Clang; patched tests pass with ASan/UBSan. Strict checkpatch and
`git diff --check` passed. Host checks do not replace repeated hardware tests.

On #80, recording no longer reproduced those oopses, but saving failed in
`ScreenMediaRecorder.save()` / `ContentResolver.openOutputStream()` with
`EACCES`. MediaStore row 83511 pointed to
`Movies/ScreenRecords/screen-20261022-200904-1792679010401.mp4`.

The underlying directory was UID/GID 1023, mode **2700**. MediaProvider ran
as UID 10366 with group 1023 and no DAC-bypass capability; the group had no
directory access. With explicit user approval, only this directory changed:

```sh
chmod 2775 /data/media/0/Movies/ScreenRecords
```

A subsequent recording saved as
`screen-20261022-203939-1792680827218.mp4`: **26,553,766 bytes**, **673 encoded
frames**, MediaStore row 83512. The boot ID remained
`832d3a15-2bee-4fc7-a1ac-f6f0d80218cf`, and no new oops appeared in the capture.
The user confirmed saving. Playback/content quality of the saved file was not
independently inspected.

This metadata repair normally persists on the filesystem. Its original cause
is unknown. Saved donor logcat shows sdcardfs being selected, while the port's
MediaProvider mount namespace exposes an F2FS backing bind at `/storage/emulated`.
That supports investigating the storage transition; it does not prove the ROM
set the wrong mode or that the port originally changed it. No recursive chmod,
chown, SELinux change or general storage migration fix was applied.

## Audio: unresolved behavior and source changes

* `CMD_EOS` close timeouts occurred repeatedly. Diagnostic #79/#80 captures
  show successful APR TX and, for some streams, a rendered-EOS RX; other
  streams still timed out. The timeout cause remains unverified.
* `f431b1110` and `97afb7703` added PCM/ASM/APR traces; **`833a2ad73` reverts
  both in current source**. The original timeout error remains. Do not expect
  those diagnostic messages in an image built after the revert.
* Voice calls reach a CVP channel-info rejection (`0x1133d`,
  `ADSP_EBADPARAM`); working modem registration is not working call audio.
* `c23444468` tried legacy ADSP format fallbacks. **`555056679` reverts it**:
  donor include-chain review showed that tissot does not enable that property,
  and playback open failed with `ADSP_EUNSUPPORTED` in the experiment.
  The last inspected #83 contains the experiment, not its revert.
* `c3ec18206` restores legacy voice front ends; `455203f9f` adds CVP
  channel-info diagnostics. Neither establishes a resolved call path.

## CPU, GPU, thermals and charging observations

The CPU groups share a PLL and coordinated voltage/CCI transaction. The donor
assigns all eight CPUs the same clock; our APCC aliases both CPU clock IDs and
uses one shared OPP policy. Separate CPUFreq policies are not an established
hardware capability. Tuning governor behavior, boosts, task limits and idle
wakeups remains possible within the shared policy.

Read-only charging samples on #83 observed:

| Measurement | Observation | Limit |
|---|---|---|
| Battery / charger | 42%, USB_DCP online; 4.07–4.08 V, 28–28.5°C; battery charging-current samples 373–471 mA; reported input limit 1.8 A | Instantaneous readings, not charging-rate or capacity validation |
| CPU compilation load | Gboard `dex2oat64`, reason `bg-dexopt`, about four CPU equivalents in repeated top samples | Background work while charging, not an idle baseline |
| SoC / battery temperatures | Sensor peak 78.2°C during that work, later about 42°C; battery reached 32.5°C | Short observation, not sustained-load or trip qualification |
| Later CPU interval | 5.1 s: about 26.5% aggregate CPU busy, 66.3% residency at 2784 MHz; low-frequency residency also increased | A busy core can raise the shared policy; this alone does not prove a governor defect |
| GPU | Reported 320 MHz, low activity; some busy counters returned 0/0 | Those 0/0 samples cannot yield utilization; sustained GPU performance untested |

These numeric CPU/charging samples came from session tool output, not a
retained benchmark artifact. Do not present them as endurance results.

CPU/GPU cooling devices and TSENS zones are registered. The vendor thermal
zone names were restored in `6afd89291` / `6a81997cf`. An earlier inspection
had legacy thermal-engine running but Android thermal service `HAL Ready:false`;
that readiness has not been rechecked on #83. Latest cooling-state snapshots
were zero; sustained mitigation effectiveness is not established by them.
Earlier run304 evidence documented CPU cooling state 4 limiting the shared
policy to 2304 MHz (history correction `bb7a4ed9e`). That verifies the earlier
CPU passive path, not current-image endurance or every renamed thermal zone.
Do not fabricate a `case_therm` sensor mapping: the corresponding donor zones
are disabled. The single CPU policy itself is not a defect.

Relevant power changes already in source include `19ab86119` (per-core idle
states), `1fcfd0efe` (vendor irqbalance deferred-timer interface), `2992ab58e`
(skip unchanged RPM bandwidth votes) and `84cec3f4f` (GPU latency vote affinity).
Their presence does not quantify battery savings.

## Unexpected stop after unplugging: cause unverified

The user unplugged the charger, later found the phone unresponsive and held
Power to bring it back. The retained console ends with:

| Uptime | Observation |
|---|---|
| 4305.293 s | healthd changes from charging to discharging; charger field empty |
| 4317.505 s | Battery 87%, 4.223 V, 25.7°C, discharging |
| 4320.689–4323.729 s | Four `wcn36xx: WARNING Bad TX complete indication` messages; then retained console ends |

No panic, oops or orderly shutdown was found in that retained tail. Android's
`SYSTEM_LAST_KMSG` contains the same ending. This does **not** prove the phone
powered off at that timestamp or establish a Wi-Fi, thermal, battery or suspend
cause. The next inspected boot is #83 / `4aba5d4d-...`.

The `ACKRESET1` slot says `command=bootloader` at 554.735 s, but its complete
64 KiB slot is byte-identical to captures preceding both `0cafbdb6-...` and
`4aba5d4d-...`; it is **stale**, not this event. Raw snapshot slots contain no
valid panic/oops dump. The recovered reset-path page has no valid panic,
emergency-restart, PSCI-reset or PS_HOLD marker; missing records do not prove
those paths were not executed.

## Panic capture: configured, incompletely validated on #83

| Runtime item | Verified reading |
|---|---|
| Backend / reserved memory | `ramoops`, address `0x86800000`, 1 MiB |
| Record / console / pmsg sizes | 4 KiB / 512 KiB / 32 KiB; `max_reason=4` |
| Existing persistent files | `console-ramoops-0` (524,276 bytes), `pmsg-ramoops-0` (750 bytes) |
| Panic / oops | `panic=60`, `panic_on_oops=1` |
| Lockup detectors | watchdog enabled, CPUs 0–7; hardlockup and softlockup panic enabled; buddy hardlockup detector |
| Detection gaps | `hung_task_timeout_secs=0`, `hung_task_panic=0`, `panic_on_rcu_stall=0` |

`/system/etc/init/hw/init.rc:336` disables hung-task detection; llkd init
rules can enable it, but the inspected runtime values were zero. Buddy mode
cannot detect an all-CPU freeze. A hang that never reaches `panic()` cannot
produce a panic-triggered record.

The source corresponding to #83 calls `ramoops_panic_snapshot()` before
stopping sibling CPUs, then performs the normal panic kmsg dump. The disk
snapshot is bounded and best-effort; successful console retention does not
prove this complete path works. The older run215/run216 controlled panic
validated its own marker/snapshot retention, not #83. No new controlled panic
was authorized or triggered. Do not claim that every freeze leaves a logdump.

## Newer source cleanup: not verified on the last inspected image

| Commit | Change |
|---|---|
| `833a2ad73` | Revert PCM/ASM/APR EOS tracing; retain timeout errors |
| `58bab2d79` | Remove QSEE stall watchdog, canary and per-call trace writes |
| `c77ba4dfa` | Disable periodic ramoops disk snapshots and process trace probes; retain explicit panic/reset capture |
| `92d6fedf9` | Remove bring-up prints in reboot, sysrq, UDC and PON |
| `55d0d347c`, `7b70afd48`, `a8da8979c`, `2ba894be6`, `bad9216ae` | Remove legacy SCM/display/IOMMU/GPU-IRQ/FastRPC diagnostics |
| `07ad1d695` | Use CLOCK_BOOTTIME and a 180 s Android boot-capture deadline; build init without always-on early-logcat services |

These changes target diagnostic overhead, not a verified fix for the unplug
failure. #83 still showed periodic snapshot heartbeats; do not confuse the
new source's parked worker with the old flashed behavior. Ramdisk changes
also require the corresponding init to be packaged; a kernel commit alone
does not update the ramdisk.

## Evidence and next verification

Selected original captures are hash-verified under
`porting/artifacts/snapshot60/status-20261022/`:

| Directory | Contents / use |
|---|---|
| `video-crash-79/` | Full captured video/ION oops stack |
| `record-crash-79/` | Before/after boot identity and live debugfs-oops capture |
| `record-test-80/` | #80 identity, recording logs, software encoder selection and failed save |
| `record-save-repair/` | Successful save capture after the one-directory mode repair |
| `unexpected-stop-83/` | Identity, raw 64 MiB logdump, pstore archive/console, previous-kmsg DropBox text, decoder reports and power readings |

`SHA256SUMS` covers these copies. Text files are untracked and binary captures
are gitignored; none are committed evidence. Preserve them before cleanup.
Original dated reports and
plans remain historical and are no longer the current work queue.

Next checks are listed in [FAILING-COMPONENTS.md](FAILING-COMPONENTS.md).
Use [BUILD_TISSOT.md](../BUILD_TISSOT.md) for an explicitly requested build,
and read [the boot reference](REFERENCE-boot-testing.md) before any boot test.

Cross-checks: [AOSP storage mounting](https://android.googlesource.com/platform/system/vold/+/refs/heads/main/model/EmulatedVolume.cpp),
[CPUFreq policy semantics](https://docs.kernel.org/admin-guide/pm/cpufreq.html),
[ART background compilation](https://source.android.com/docs/core/runtime/configure/art-service),
[ramoops persistence](https://docs.kernel.org/admin-guide/ramoops.html), and
[watchdog limits](https://docs.kernel.org/admin-guide/lockup-watchdogs.html).

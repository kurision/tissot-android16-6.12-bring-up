# Failing components tracker (tissot 6.12 bringup)

Living list. Every entry names its evidence. Updated as work lands.
Last update: 2026-10-13 (snapshot60 run90 verdict recorded).

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

## Snapshot58 runs 2–3 — adb over the 6.12 kernel

run2 (base: stock `boot_a` ramdisk + guard files staged by
`scripts/stage-guard-inputs.py`) recovered the guard chain and the USB
capture: `bootdevice guard installed`, `USB handoff rc appended to
/vendor/etc/init/hw/init.qcom.usb.rc errno=0`, 138256 bytes. Boot still
died at ~37 s, right after `init: Loading SELinux policy`.

run3 (base: `boot-ack-snapshot32-spill-carveout-tail-sd-screen.img` from
git, 665 cpio records, SHA256 `ef6c5400…9da06`) is the fix. The difference
is not any kernel change: the port's ramdisk carries a **patched SELinux
policy** — `userdebug_plat_sepolicy.cil`, 3304221 bytes — plus a
`force_debuggable` trigger. Neither exists in the stock ROM ramdisk and
nothing in the tree regenerates them.

**adb works.** Evidence, in order:
- init reaches `processing action (zygote-start)`; run1/2 never got past
  `post-fs`.
- `apexd: Parsing file /apex/com.android.adbd.capex/etc/init.rc` — the adbd
  APEX is installed and its rc parsed.
- The USB ACM stream ends at 463746 bytes with
  `reader-error: ... returned no data`. That is the handoff, not a fault:
  the diag ACM function releases the UDC and adbd claims it. The host then
  enumerates `18d1:4ee7 Google Inc. Nexus/Pixel Device (charging + debug)`
  and `adb devices` reports `device`.

Remaining crash is the one `86cf5e369` already predicted: `Service
'vendor.hwcomposer-2-1' received SIGSEGV` (6 occurrences). The vendor sdm
HWC needs the legacy MDSS fbdev interface; `init.c` has a
`-DANDROID_HEADLESS` stub for it that is not enabled in this build.

logdump/SD pulls for run3 returned 0 bytes: the collector ran before the
stock ROM finished coming back after the 360 s watchdog reboot. USB is the
better channel here anyway and captured 463746 bytes.

## Snapshot58 run4 — headless stubs remove the crash loop

Same base and kernel as run3; only the init changed. Built with
`-DANDROID_HEADLESS` alongside `-DANDROID_CAPTURE`
(`init_sha256 a2a6326e…`), which binds comment-only stubs over
`android.hardware.graphics.composer@2.1-service.rc` and
`vendor.lineage.livedisplay-service.sdm.rc` before policy load
(`init.c:820`). Both report `errno=0`.

| | run3 | run4 |
|---|---|---|
| SIGSEGV / SIGABRT | 6 (`vendor.hwcomposer-2-1`) | **0** |
| `vendor.hwcomposer-2-1` | started, crashed | never defined |
| services started | — | 73 |
| init stages | to `zygote-start` | to `zygote-start` |
| USB bytes | 463746 | 417507 |
| adb | `18d1:4ee7` | `18d1:4ee7` |

`sys.boot_completed=1` is still never logged, so the 360 s watchdog still
reboots. The USB stream ends at the adbd handoff, so everything after that
point is invisible to the current evidence channels — that, not a crash, is
the next thing to close. Non-fatal exits remaining: `vendor.qrtr-ns` status
1, `vendor.per_mgr` status 255, `misctrl` status 1, matching the QRTR
findings in `86cf5e369`.

## Snapshot58 run5 — adb on the port kernel; boot_completed cause found

`BOOT_DEADLINE_SECONDS` is `#ifndef`-guarded (`init.c:47`), so the watchdog
was raised to 1800 s with `-DBOOT_DEADLINE_SECONDS=1800`. Detection is
unchanged: the watchdog still reads `/dev/kmsg` for the
`processing action (sys.boot_completed=1` line (`init.c:1760`).

This is what closed the post-handoff evidence gap. With the phone held up,
adb is reachable **on the port kernel itself** — `uname -r` reports
`6.12.93-4k-gf6c975d4f141-dirty`, not the CAF 4.19 ROM. USB capture
440942 bytes, handoff at ~144 s.

**`sys.boot_completed` never fires because surfaceflinger aborts in a
loop**, visible only now that adb works on the port kernel:

```
Fatal signal 6 (SIGABRT) in tid 675 (surfaceflinger), pid 648
>>> /system/bin/surfaceflinger <<<
Abort message: 'no suitable EGLConfig found, giving up (format: 1, vendor:
Android, version: 1.4 Android META-EGL, ... Client API: OpenGL_ES)'
```

Four occurrences in the crash buffer, three distinct pids. SF never stays
up, so the boot never finishes and the property is never set. This is the
`kgsl` PARKED row below, now confirmed as the single blocker standing
between the port and a completed boot — the EGL blob needs
`/dev/kgsl-3d0`, which needs the kgsl rewrite.

Secondary: `dpmd` also aborts, in
`android::hardware::ProcessState::init` — it dies setting up its scheduler
state, not a display issue. Not yet triaged.

## Snapshot60 run87–90 — post-zygote reset attribution (run90 verdict: not a process death)

Runs87-89 all end in a reset roughly 4 s after zygote launch; the log tail
around the cutoff is missing, so no process-exit/signal evidence exists.
Display PLL (`pclk0 update_config`) and GPU mapping defects are real but
unresolved and do **not** explain the reset by themselves: run89 logdump
(`run89-logdump.log:1859-1885,1928`) shows MDP/DSI init plus successful
framebuffer registration around 76.8 s, i.e. deferred probing completes and
Android starts. "Deferred probe blocks userspace forever" is contradicted
by that chronology. Likewise `kgsl_mmu.c:730-761` falls back to NOMMU
after the legacy IOMMU probe fails (`kgsl_iommu.c:2734-2740`), which is not
proof the GPU platform probe failed.

Run90 adds opt-in kernel evidence, not a DT/firmware/display/GPU fix
(`a58d7a0`): `fs/pstore/ram.c` registers `signal_generate` (11 fatal-ish
signals, non-kthreads) and `sched_process_exit` (group leaders) probes
gated by the existing `ramoops.reset_capture=1`, emitting `DIAG
signal_generate` / `DIAG process_exit` records to kmsg (hence logdump).
No signal-delivery, exit-path, clock, GPU/MMU, firmware or display changes.
Build `6.12.93-4k-ga58d7a094211` (clang/LLD 22.1.8): `Image.gz`
`3ddcae2c…`, DTB unchanged `71951fb1…`, `Image.gz-dtb` `322b1149…`,
`boot-run90.img` `efaa1c91…` (665 CPIO records, init byte-identical to
run89 — only the kernel payload differs).

Boot method: RAM-only `fastboot boot`, 4 s ADB timeline with numbered
exclusive logcat segments; logdump precleared with verified zero readback
(`run90-preclear.status`). No 6.12 ADB window; stock 4.19 back ~25 s after
the 143.5 s USB capture (`run90-usb.log`, 476311 bytes).

Verdict (`run90-logdump.log`, `DIAG process capture active` at line 163):
84 records (39 `signal_generate`, 45 `process_exit`) show only routine init
lifecycle — SIGKILLs to one-shot services (`aconfigd`, `derive_*`,
`odsign`, `apexd`, `netd`, `recovery-persis`, mostly `result=1` as targets
already exited) and exits with ordinary one-shot codes (`0x100`, `0x500`,
`0x600`, `0xfe00`, `0x9`). Zero SIGSEGV/SIGABRT in the whole boot; zero
`Fatal signal` in early-logcat; no new tombstone. Init did start `zygote`
and `zygote_secondary` (services forked ok ~106.9 s), but neither zygote
process ever emitted output — the log runs on through HAL startup to
~112.0 s (`starting vendor.wifi_hal_legacy`, max timestamp at line 3748;
later file lines carry earlier stamps from decode interleave). No
`ACKRESET1` (no orderly reboot), no panic/oops, snapshot 2 s cadence held
to the tail (seq 53 @ 111.95 s). Two prior hypotheses closed: storage-flush
failure is out (dump → sync exit 0 → streaming reader all proven, /data
gained +720 logcat lines to ~1 s before death) and the run89 "dies during
first sync" correlation is refuted (sync exited 0 at 105.4 s, 6.6 s before
death). The reset is abrupt with healthy userspace progression — not
caused by any process death or signal. Remaining candidates: firmware /
bootloader watchdog, power, or traceless kernel halt. No DT/GPU/display
change is supported by this evidence; those defects stay open but
non-attributed. Next-run hygiene: pull postreset files/state for the
tombstone delta and delete the on-device early-logcat rotation pre-boot
(run90 appended at line 776/777). Next diagnostic if pursued: per-HAL
gating A/B of the usb-hal/wifi-hal bring-up window on an unused tag.

Run91 result (HAL gate, `init-run91` `6bc15d4a…`, same `322b1149…` kernel):
the five death-window HALs (sensors, usb, wifi, lights, memtrack) were
provably stubbed — zero mentions in `run91-usb.log`/`run91-logdump.log`
vs present in run90 — yet the boot died identically at 122.3–122.6 s,
mid-`modprobe rmnet_ctl`, silent and abrupt. 98 more DIAG records (45
signals, 53 exits; the one-time `capture active` line itself was lost to
the early kmsg overrun, the callbacks demonstrably ran), zero SIGSEGV,
zero `Fatal signal`, no `ACKRESET1`, snapshots on 2 s cadence to 121.8 s.
The gated set is exonerated and our init change is innocent (no
panic/oops/init failure). The resetter sits below the HAL layer:
firmware/bootloader watchdog, power, or traceless kernel halt.

Run92 result (cmdline A/B: run90's kernel `322b1149…` + init `639ae66c…`
plus only `pd_ignore_unused`): still no 6.12 ADB (6th miss) — the cmdline
is exonerated for the ADB loss, which now points at the run86→87 kernel /
init delta (source archaeology pending). But death moved 112 s → ~57 s
(tail mid-early-init, `qcom-post-boot`/atrace), same silent profile (9
between a power-domain-related trigger and an outlier.

Run93 result (byte-identical run92 image, new tag): death at ~136.0 s
(`run93-logdump.log` max timestamp; snapshots on cadence to 135.9 s),
silent and abrupt, no `ACKRESET1`. Run92's 57 s death was the outlier;
`pd_ignore_unused` has no consistent timing effect. Identical-image tails
now read 57 / 136 s, and the all-runs set spans 57–136 s with five deaths
clustered 107–123 s — the trigger is not deterministic in time. Seven
silent deaths, zero 6.12 ADB since run86, kernel scheduling to each tail.

Run94 result (old `1fb377fb` kernel + new `639ae66c…` init, std cmdline):
still no ADB (7th miss) — kernel-only ADB hypothesis dead. The log's last
line (122.564 s) is a `clk-rcg2.c:133 update_config` WARNING from
`kgsl_worker_thr`, with its `Call trace:` never printed: death mid-unwind
in the GPU worker's clock path (bus stall → watchdog bite fits). But the
signature is old-kernel-only (zero hits runs 90–93; the RESETSM revert
for the ADB loss — testing `init-run84` against the new kernel next.

Run95 result (new `322b1149…` kernel + old `8a878b34…` init): still no ADB
(8th miss) — init-only hypothesis dead. Longest survival yet, 156.8 s
(snapshots to 156.0 s), tail mid-`tui_comm-1-0`, same silent profile, zero
`clk-rcg2.c:133`, no `ACKRESET1`. Every variable swapped alone with no
ADB; only the full run86 combination
reconstructs it exactly to test determinism vs run86-luck.

Run96 result (bit-identical run86: kernel, init, cmdline, ramdisk all
equal): still no 6.12 ADB (9th miss) — run86's ADB was marginal luck, not
image content. The entire ADB-chase arc is closed: cmdline, kernel, init,
and full combination all fail to reproduce it. Death at ~76.6 s
(prng_seeder socket creation; SD pull failed this once, USB+raw retained),
same silent profile, no `ACKRESET1`. Tally: 57 / 77 / 107 / 111 / 112 /
122 / 123 / 123 / 136 / 157 s across ten boots. Pending: `1cd68d721`
prints raw `PON_REASON1` at qcom-pon probe (`DIAG pon_reason`); needs a
Linux rebuild — the next boot's line names run96's resetter.

Run97 PON result: `DIAG pon_reason reason1=0x21 err=0` at 13.7 s
(mechanism proven end to end), but bit0 is SOFT — our own `adb reboot
bootloader` sets it on the way in, so under last-cause semantics this
names the entry reboot, not run96's death. Under sticky-accumulate
semantics (nothing clears the register on probe), run96's death added no
watchdog/PON1 bit, which leans against a PMIC-watchdog death. Run98
(same image, new tag) decides: new bits beyond `0x21` means deaths leave
a PON signature; bare `0x21` again means deaths are PON-silent.

Run98 result: bare `0x21` again — run97's 74 s death added no PON bit.
Deaths are PON-silent; the PMIC watchdog path is exonerated (no PON1 bit
across two deaths), no bark traces in either log. Tail at 126.1 s shows
`mdss_fb_open by android.hardwar` — the display stack engaging fb, then
the same silent cut. Remaining: APPS hard hang with PS_HOLD drop (leaves
no PON trace), RPM/modem/XBL watchdog, or power transient.

Run99 result (headless `aa7317a1…` init, composer+livedisplay stubbed):
stubs held — bind errno=0, zero service definitions (the SELinux
`Could not set context` line is non-fatal noise; init parsed stub
content). Death at ~64.4 s during early init (`init.rc:116` action),
before zygote, HALs, or display engagement — same silent profile, no
`ACKRESET1`. Display engagement does not trip the reset; the earlier
the death phase, the further below Android the resetter sits. Tally:
57 / 64 / 77 / 107 / 111 / 112 / 122 / 123 / 123 / 136 / 157 s, eleven
boots. SD channel flaked twice (96, 99); USB+raw retained both times.

Run100 result (`68f757b9b` kernel, all three probes): qseecomd regressed
— `Service 'vendor.qseecomd' exited with status 255` 31 times from 242 s
onward (runs 86-99: zero occurrences). `qseecom_scm_call2 cmd 0xa ->
0` still succeeds at probe (14.5 s); the failures start at qseecomd
userspace. Nothing in the two new commits touches qseecom; both were
built into the same kernel, so attribution between `1cd68d721` and
`68f757b9b` needs a split build. GPU never probed (`gpu_ahb_clk`
missing as before), so `DIAG kgsl_pwrstate` stayed silent;
`DIAG pon_reason reason1=0x21` again. Boot survived to ~990 s then
reset; post-return ADB stalled ~5 min before recovering.

Run101 result (`58bcef215` = kgsl log reverted, PON kept): qseecomd
still exits `255`, 64 times from ~228 s — the revert did not fix it, so
both new commits are exonerated as code. The regression therefore comes
from the build/merge shape between run86's `gc0655a13e` and the
`b37ecaa78` merge line (snapshot44-46 qseecom fixes were already in
tree). Kernel side is clean: `cmd 0xa -> 0`, QSEE 0x1001000, CE clocks
ready at probe; qseecomd dies before any listener SMC appears. Death
log tail at ~549.7 s; no `ACKRESET1`; `pon_reason=0x21` again. GPU
still unprobed (`gpu_ahb_clk`), so no kgsl pwrstate lines. Next: diff
qseecom-relevant build inputs (fragment, .config delta) run86 vs now.

Root-cause chain for the qseecomd regression (all local evidence):
1. Embedded IKCONFIG identical across run84/87/101 (md5 `9e1fd9c8…`,
   2545 symbols incl. `CONFIG_QSEECOM=y`, `CONFIG_ION_LEGACY=y`) —
   config exonerated.
2. Run100/101 show `mmc0/1: Card stuck being busy! __mmc_poll_for_busy`
   + `error -110 whilst initialising SD card` at ~4.4-17 s (runs 86-99:
   zero occurrences). The SD card slot (7864900.mmc, the 32 GB logging
   card) wedges the MMC bus poll; eMMC (HS400) survives.
3. `init: wait for '/dev/block/platform/soc/..' timed out and took
   5009ms`, then five `stat failed: ...by-name/...: No such file` +
   20 s-per-partition `Skipping during mount_all` retries — mount_all
   --early ballooned to 100 s (run97: 1.07 s, run86: 0.52 s).
4. Everything downstream of `fs` shifted ~170 s; qseecomd started at
   242 s (run97: 70 s) into a storm of prior service deaths; its
   listener SMCs never registered (run97: five listeners at 70.5 s).
So: SD-card stuck-busy wedges early-mount, the 100 s stall cascades
into the HAL storm, and qseecomd is collateral damage — not a qseecom
regression at all. Run94's kgsl WARN at 122 s and run100's 990 s
survival both consistent: the SD poll wedge is timing-luck per boot.

Run102 result (same run101 kernel, init SD mirror disabled): silent cutoff
at 64.504747 s during deferred MDP re-probe (`run102-logdump.log:1498-1513`).
The SD controller still reports stuck-busy/-110 at 5.902 s (498-499):
removing userspace SD logging does not disable or repair kernel card
initialization. No keymaster/gatekeeper service starts, no ACKRESET1,
no observed port ADB; stock returned (`run102-adb-timeline.log:6-17`).
Thus these HALs cannot explain every early return.

Late-cutoff hypothesis: gatekeeper's first QSEE ioctl precedes most
107-157 s cutoffs by 0.9-3.3 s; the PID-attributed comparison is in
`TRACKER-kgsl-port.md`, "HAL timing comparison through run102".
Run97 instead ends during keymaster activity before gatekeeper starts.
Neither correlation establishes a trigger. Approved run103 probe gates
only keymaster+gatekeeper with the existing halgate memfd overlays;
same run102 kernel/DTB/cmdline, init rebuilt locally, RAM boot only.

Run103 result (paired HAL gate, init `8b07e948…`): both overlays held
(errno=0 at `run103-logdump.log:879-880`); neither HAL starts and no
`qsee_send_desc` appears. Kernel/DTB/cmdline unchanged. Kernel logging
continues to 226.511 s (3107), snapshots through 226.576 s. **User
forced the reboot because the same messages repeated; not a spontaneous
reset.** No ACKRESET1; no observed port ADB.

The repeated output is missing keymaster / keystore2 waits. Init blocks
in `vdc --wait cryptfs enablefilecrypto` during post-fs-data (2266-2279);
no zygote or late-HAL startup. Surviving the 107-157 s window therefore
does not confirm gatekeeper as trigger: this probe stops progression
before that path. The gatekeeper-only follow-on is run104 below;
run103 itself performed no second boot. Reset attribution stays WATCH.
SD stuck-busy persists but mount_all --early takes only 505 ms (1964)
and eight QSEE listeners register (2027-2104), so the card failure does
not invariably produce run100/101's downstream stall.

Run104 result (gatekeeper-only, init `d749d6fb…`): override errno=0
(`run104-logdump.log:880`), no gatekeeper start. Keymaster runs 35
send commands, fscrypt completes at 132.501 s (2555), zygote starts
156.342 s (3679). Same kernel/DTB/cmdline; one RAM-only boot.

USB captures a **synchronous external abort** at 161.390266 s, CPU 0,
PID 640, comm `cat`, ESR `0x96000010` (`run104-usb.log:3835-3839`).
No PC/FAR/abort stack survived; raw logdump stops before the header,
snapshots through 159.371 s, no ACKRESET1. Stock returns at host
elapsed 228.23 s; no host-initiated reboot after image entry, no
observed port ADB. Android early-logcat recovered; tombstones unchanged.
Gatekeeper is not necessary for this failure; no basis to confirm it
as the trigger or equate all prior silent returns with this abort.

[INFERENCE] The cat context could be ack-sync-loop's second uptime
read (first retained at 155.41 s, five-second sleep after sync), but
parent/argv and faulting PC were not captured. Do not suppress the
collector or alter drivers on that correlation. Next diagnostic todo:
obtain PC/FAR/stack and identify the read path. No further boot here.

Run105 probe: kernel-mode `do_sea` prints compact
ESR/FAR/FnV/PC/LR/PID/PPID/comm and the fault-context backtrace before
the verbose oops dump. No fixup, user-mode, reset or driver behavior
changes. Probe committed/pushed as `6b11c2e1c`; user rebuilt on Linux.
Pulled `snapshot60/run105/` hashes and embedded configuration verified.
Kernel banner matches the commit; compiled SEA diagnostic present.
Image packaged with run104's unchanged init/ramdisk/DTB/cmdline;
one RAM-only boot completed. Silent early return: raw log tail at
76.997781 s during MDP/SMMU/DSI probing (`run105-logdump.log:1489-1506`);
snapshots through 75.637 s. No keymaster/QSEE sends/zygote/mount_all,
no SEA or abort header in USB or full raw dump, no ACKRESET1.
Gatekeeper bind held (539), but this boot did not reach run104's
post-zygote abort path. On-device SEA emission and read-path attribution
remain unverified; no MDP/SMMU cause is established by the tail.
Early-logcat and sync probe are byte-identical run104 carry-over;
tombstone contents unchanged. Stock boot-completed/root/enforcing
confirmed after return, no observed port ADB. No second boot here.
Next: reproduce the SEA using the same image and obtain matching
vmlinux/System.map for offline symbolization. Keep early silent returns
separate from the confirmed run104 external abort.

Run106 result (byte-identical run105 image, fresh tag): gatekeeper bind
held (errno=0, `run106-logdump.log:878`), keymaster executes 35 sends,
zygote starts at 125.077 s (3712), then silent cutoff at 130.025225 s
(3837). No SEA/abort header in USB or full raw/snapshot dump, no ACKRESET1.
Valid snapshots through 127.977 s; no port ADB; stock returned without
a host-initiated reboot after image entry. Tombstone contents unchanged.
New Android logcat adds 88127 bytes/683 lines after the archived prefix;
sync probe adds a row at 123.64 s. Gatekeeper is not necessary for the
late silent profile either; external-abort read-path attribution is open.

MDSS error assessment: optional clock failures do not fail probe
(`mdss_mdp.c:1939-1958`), missing NRT VBIF mapping is tolerated
(3049-3054), missing reset offset/fudge factors retain disabled/default
settings, and absent min/max FPS defaults to panel frame rate
(`mdss_dsi_panel.c:2380-2418`). Run106 registers its framebuffer at
74.781 s (1875) after those same errors. Do not fabricate DT resources
or suppress messages as a reset fix. This does not exonerate every
later access: required resources, clock/power sequencing and splash
handoff need targeted review. Bootloader display is off in runs104/106
but on in early-return run105; that selects different handoff paths
(`mdss_mdp.c:3189-3215`), not yet a proven cause. No MDSS code change.

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
| GPU 3D (`kgsl`, EGL blob `libGLESv2_adreno.so`) | s58 run5: `surfaceflinger` SIGABRT ×4, 3 pids, `no suitable EGLConfig found` — **the confirmed reason `sys.boot_completed` never fires** | PARKED (blocker) | kgsl rewrite, backlog #1. Confirmed reachable via adb on the port kernel; no longer a guess |
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

## Reset logging repair — orderly persistence verified in run107

Late PARTLABEL lookup is repaired; generic pstore-blk is not a panic-safe
replacement for the existing ramoops/logdump owner. Raw snapshot recovery
exports two actual run106 console samples, no valid panic/oops dump.
CRC32-committed snapshots, separate periodic printk tails and a 60 MiB
userspace prefix cap now protect the reserved diagnostic ranges.

Host regressions and ARM64 prefix smoke pass, including physical Tissot.
User's Linux build of `be63c5ea5` RAM-booted in run107; the controlled 45 s
deadline invoked the orderly notifier, flushed a genuine ACKRESET1 record,
and returned to stock. Both console snapshots and periodic printk tails
passed header/payload CRCs after return. Earlier spontaneous reset cause and
final panic/watchdog persistence remain unproven.
See `TRACKER-kgsl-port.md`, "Run107", and `logging/README.md` for evidence.

## Run108 — early silent reset tied to MDP deferred re-probe

Removing only run107's deadline reproduced a spontaneous return at 66.54 s:
no ACKRESET1, no panic/oops, four CRC-valid snapshots. The log ends 8
records after the late MDP re-probe kick. Runs 102/105/108 all die within
15-37 records of that kick; all other runs pass it. Suspect an unclocked
MDSS register access hanging the bus (`[INFERENCE]`; access unidentified).
Next: eMMC markers around each re-probe hardware access. Late 107-160 s
resets stay open. Details: `TRACKER-kgsl-port.md`, "Run108".

## Run109 — SD removed; resets track MDSS first use

SD removal cleared the stuck-busy/-110 wedge and the boot passed MDP
re-probe, then died silently at 125.3 s, 0.4 s after `display-color-hal`
start. Eleven late runs die 0.1-2.3 s after display HAL / surfaceflinger
start or `mdss_fb_open`; early runs die in MDP re-probe. MDSS is the prime
suspect (`[INFERENCE]`). Next: init-only display-HAL gate. Details:
`TRACKER-kgsl-port.md`, "Run109".

## Run110 — display gate held; reset is below the HAL layer

Stubbing hwcomposer, display-color-hal and surfaceflinger (three DIAG
overrides errno=0, zero starts) did not prevent the silent reset: run110
died at 62.7 s inside the kernel's deferred MDP re-probe, with no display
userspace ever running. The resetter sits in the kernel MDSS re-probe
path, not the display HALs. Next: eMMC markers around each re-probe
register access. Details: `TRACKER-kgsl-port.md`, "Run110".

## Run112 — re-probe passes by luck; marker writes land nowhere

Marker kernel (`0f72ccf3…`) booted fine and the deferred re-probe passed
(framebuffer 68.6 s); silent reset at 132.4 s with the display gate
still held. Marker code ran (pr_infos present) but all eMMC slots zero
— the `/dev` open path fails in kernel context. Next: multi-path open
with error logging + readback verify. Details: `TRACKER-kgsl-port.md`,
"Run112".

## Run113 — opens fail ENOENT: kworker has no fs_struct

Writer-fix kernel (`d644bb5a…`) proves every absolute `filp_open` from
the deferred probe fails ENOENT: kworkers carry no fs_struct, so path
lookup has no root. Init opens the same nodes fine. Silent reset at
153.9 s, gate held. Next: `file_open_root` on init's borrowed root.
Details: `TRACKER-kgsl-port.md`, "Run113".


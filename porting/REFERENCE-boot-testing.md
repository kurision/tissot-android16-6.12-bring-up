# Reference: tissot boot testing + kernel-log evidence channels

Status as of 2026-10-10 (run2/run3/run4 of snapshot60). Written after the
kgsl port's first two failed diag boots produced **zero** kernel logs while
the identical flow on the last known-good kernel worked. Every claim below
was observed this session; commands assume adb serial `2819f2320604`.

## The toolchain (all in `porting/logging/`)

| Tool | What it does | Gotchas |
|---|---|---|
| `usb-boot-capture.py` | adb reboot bootloader → `fastboot boot` (RAM only) → stream `/dev/ttyACM0` (diag gadget `18d1:9008` serial `tissot-diag`) until the EOF marker | Needs **pyserial** (`/tmp/kgsl-capture-venv` was made with `uv venv` + `uv pip install pyserial`; system python has no pip). The ACM stream **dies when adbd takes over the USB** — a `reader-error ... device disconnected` with 0 bytes can still be a *successful* boot; the durable evidence is the logdump/pstore, not ACM. Never poll the job; it has no host deadline. |
| `package-diag-boot.py` | Builds the boot image: base header/ramdisk from the previous snapshot, new kernel from a `kdir/`, new `init` cpio record only. Asserts every other record/header byte survives. | Refuses to overwrite outputs. `--keep-stock-console` (added 2026-10-10) keeps `console=tty0` instead of forcing `console=ttynull` — required for panel-visible panic capture. `kdir/` must contain `Image.gz`, `Image.gz-dtb`, `msm8953-xiaomi-tissot.dtb`, and a matching `SHA256SUMS`. |
| `clear-logdump.sh <tag>` | Zeroes the 64 MiB logdump partition pre-boot and proves readback-zero | Needs `su`; works on the stock ROM, exits **127 on the port kernel**. Run from the snapshot dir; writes `<tag>-preclear.*`. |
| `pull-run-evidence.sh <run> <image>` | Pulls `/dev/block/by-name/logdump` (67108864 B, sha-verified) → `<tag>-logdump.bin` + readable `-logdump.log` + `-sd.log`; validates pstore slots (magic `RAMO`, DBGC console) | Run from a `snapshot<N>/` dir; needs `su` + adb `shell_v2`. Refuses to overwrite existing evidence. Exit 1 if nothing valid — that is the "no kernel log" signal. |

Snapshot layout: `porting/artifacts/snapshot<N>/{base.img, init, kdir/, boot-run<N>.img, package-run<N>.json, run<N>-*}`.
One change per boot; clear logdump before every boot (run2/run4 proved the value — empty slots are how you tell "died before pstore").

## Evidence channel map — where a kernel log can come from

| Channel | Available when | On this setup |
|---|---|---|
| diag USB gadget ACM (`ttyACM0`) | userspace reached (diag init creates the gadget) | run1/run3 control yes; run2/run4 no — died pre-userspace |
| `pstore_blk` on `PARTLABEL=logdump` | **`late_initcall` (level 7)** registered *and* the partition resolved — which on this kernel it never is | every log carries `pstore_blk: failed to resolve 'PARTLABEL=logdump'!` then `failed to open ...: -2!`; the eMMC appears ~1.2 s after its probe. It records nothing. The four `pstore_blk.*` cmdline tokens are inert and are dropped as of run25 |
| panel console (`console=tty0` + fbcon) | DRM/KMS + fbcon up, and cmdline keeps `console=tty0` | the packager forces `console=ttynull`; use `--keep-stock-console` to get oops text on the phone screen |
| stock 4.19 ramoops | level 3, always | device runs `4.19.325-cip136-...`: `pstore: Registered ramoops ... attached 0x400000@0x91400000` — this is why the 4.19 *always* has logs |

### The 6.12 evidence blackout (root cause of the run2/run4 silence)

Two independent mechanisms hide a failing driver probe. Neither is fixed by
initcall ordering alone:

1. **`pstore_blk` never opens the logdump partition on this kernel.** Every log
   shows `pstore_blk: failed to resolve 'PARTLABEL=logdump'!` followed by
   `failed to open 'PARTLABEL=logdump': -2!` (run24 log line 290,
   `t=2.756 s`) — the eMMC is only probed ~1.2 s later. The backend is
   registered but has no device, so it stores nothing regardless of initcall
   level. The four `pstore_blk.*` cmdline tokens cannot fix this; they are
   dropped as of run25 (`package-diag-boot.py --drop-cmdline`) purely to free
   cmdline bytes for the console choice.
2. **`console=ttynull` hides the console.** The packager forces it unless
   `--keep-stock-console` is passed.

What survives is the **level-2 ramoops node** at
`msm8953-xiaomi-tissot.dts:118` (0x86800000, 1 MiB): its backend registers
at `t=0.713 s` and its logdump snapshotter is ready at `t=5.06 s`
(`run24-usb.log:422`). Any death before ~5 s leaves no record anywhere, so a
truly early death needs the TTL `earlycon` (`console=ttyMSM0,115200`).

Initcall level still matters for what ramoops can catch — check it with
`nm out/vmlinux | grep '__initcall.*<suspect>'`; the suffix digit is the level
(2/6/7). Moving a suspect driver to `late_initcall` puts it behind level-6
probes, not behind a working `pstore_blk`.

Levers, in order of cost:
1. `--keep-stock-console` — photo the panel for the oops (needs fbcon up first).
2. `--drop-cmdline` for inert tokens, `--append-cmdline ' earlycon
   console=ttyMSM0,115200'` to get the TTL.
3. `initcall_blacklist=<name>` on the cmdline — A/B whether that initcall is
   the killer without a rebuild (blacklist takes initcall names, e.g.
   `kgsl_3d_init`).
4. ramoops read-back: boot a kernel with the same DT (any 6.12 diag image) and
   pull `/sys/fs/pstore/console-ramoops-0` via `adb shell su -c` **before**
   that boot's own shutdown overwrites it; the stock 4.19's ramoops is a
   different region (0x91400000) and has no `/dev/mem`, so it cannot read it.

### The UDC-handoff capture cutoff (tooling defect, not a boot defect)

The diag init's `guard_usb_handoff` (`scripts/initramfs-diag/init.c:777-818`)
unbinds the ACM function at the vendor stack's configfs switch. `write_all`
then fails, and `write_capture` used to return -1 for **both** sinks, ending
`dump_log` for the SD mirror too — at `t≈23.6 s`, 504 lines / 39 KB. Control
boots cut off identically (run16: 491 lines, last `seq=433, t=23.6 s`), which
is how to tell this apart from a driver fault: a kgsl-only cutoff is suspect,
but an identical cutoff on a known-good control is the tooling.

Fixed by making the sinks independent — `write_capture` now returns a bitmask
(bit 0 primary, bit 1 mirror), `dump_log` retires a dead primary (`out = -1`)
and keeps the SD mirror running to its 900 s deadline, and `sinks_dead()`
stops the loop only when nothing is left. Covered by
`sink_retirement_selftest` in `init.c`. Everything past the handoff exists
only in the SD log afterwards; on the panel it is `console=tty0`, over UART
only with an adapter attached.

Poll adb every **4 s** for the first 150 s of a boot
(`snapshot60/run25-adb-timeline.log.py`). Slower polls land entirely after the
device resets and report the stock 4.19 kernel, which is what made adb look
broken in earlier runs.

## Pitfalls that cost real cycles (2026-10-10)

- **A 0-byte `-usb.log` does NOT mean the boot failed.** Controls (run3, run6,
  run8, run10) all captured 0 ACM bytes yet booted fully — the adbd handoff
  terminates the CDC-ACM stream, and pyserial then errors out. The durable
  truth is `pull-run-evidence.sh` output: `SD success: ... common extent
  matches` + valid `RAMO` slots = the diag userspace ran; `SD unverified` with
  a stale byte count = it never ran. Never conclude from the ACM log alone.
- The stock 4.19's ramoops region (0x91400000) is not the port's (0x86800000)
  — its `/sys/fs/pstore` reads its own kernel's records only.
- `clear-logdump.sh`/`pull-run-evidence.sh` need `su`; the device's current
  4.19 build has it (SELinux denies `/sys/fs/pstore` to shell, but `su -c`
  reads logdump fine). `dmesg` needs `su -c`; `adb root` did not stick.
- **An empty logdump never exonerates an initcall.** `pstore_blk` is not
  recording at all on this kernel (see above), so "no record" means "no
  backend", not "no panic". Only a control boot of a known-good image settles
  it.

## Fault-isolation recipe used for the kgsl port (repeat this next time)

1. Pre-clear logdump, boot the suspect image, poll adb at 4 s, pull evidence.
2. **Control boot the last known-good image** (e.g.
   `snapshot59/boot-run1.img`) through the same tool: if the control produces
   the same symptoms, the difference you are chasing is in the suspect
   kernel's cmdline/DT, not the driver. (This cost one cycle — do it before
   theorising.)
3. Add `--keep-stock-console` and `--append-cmdline ' earlycon
   console=ttyMSM0,115200'` so the next failure is *visible* on the panel or
   the TTL, and drop inert tokens with `--drop-cmdline` to stay under the
   512-byte limit.
4. Confirm the capture actually ran to completion before reading it: a log
   ending at `t≈23.6 s` is the UDC-handoff cutoff above, not the end of boot.

## Environment notes

- pyserial lives in an ephemeral uv venv — currently `/tmp/diagvenv`
  (`uv venv /tmp/diagvenv && uv pip install --python /tmp/diagvenv/bin/python pyserial`).
  The system python has no `pip`, so `python3 -m pip install` fails.
- The port worktree builds with system clang 22 (`scripts/build-ack-tissot.sh`
  defaults `llvm_bin=/usr/bin`, `CC="ccache clang"`). Never run bare
  `make O=out ...` without the script's env — it re-syncs kconfig for host
  arch and destroys the merged config.

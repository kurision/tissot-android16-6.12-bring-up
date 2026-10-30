# Reference: tissot boot testing + kernel-log evidence channels

> **Current state: CURRENT-STATE.md.** The entries below are the
> running log and are ordered newest first; the top entry is not
> necessarily the current state. Build, package and flash:
> [BUILD_TISSOT.md](../BUILD_TISSOT.md).
>
> The per-run evidence this document cites (boot images, kernel directories,
> init binaries, logdumps) is no longer in the repository; it is
> git-ignored and disposable, and the boot images are in
> ../ack-kgsl-artifacts.

## GPU/CPU bring-up diagnostics disabled (2026-10-20; not boot-tested)

At the user's request, legacy GPU success-path logging and ZAP stage records
are compiled out. `RAMOOPS_PERIODIC_SNAPSHOTS=0` in `fs/pstore/ram.c` disables
the continuous console/printk-tail snapshots and their fast-cadence trigger.
Standard pstore, the explicit panic snapshot, orderly-reset capture and the
ramdisk's primary kernel-log recorder remain enabled. Without periodic writes,
old snapshot slots may still validate: do not attribute them to a new failed
boot without fresh provenance. Pre-hang periodic samples are no longer promised.

(Since 2026-10-24, with the flag off, the snapshotter also completes
`ramoops_snapshot_ready` at thread start, so the camera EEPROM/CPP
late-init gates are released immediately instead of timing out after 10 s.)

Status as of 2026-10-10 (run2/run3/run4 of snapshot60). Written after the
kgsl port's first two failed diag boots produced **zero** kernel logs while
the identical flow on the last known-good kernel worked. Every claim below
was observed this session; commands assume adb serial `2819f2320604`.

## The toolchain (all in `porting/logging/`)

| Tool | What it does | Gotchas |
|---|---|---|
| `usb-boot-capture.py` | adb reboot bootloader → `fastboot boot` (RAM only) → stream `/dev/ttyACM0` (diag gadget `18d1:9008` serial `tissot-diag`) until the EOF marker. **Only for inits built with `-DANDROID_USB_CONSOLE`**; the default build creates no gadget, so expect the device never to appear. Needs **pyserial** (`/tmp/kgsl-capture-venv` was made with `uv venv` + `uv pip install pyserial`; system python has no pip). Never poll the job; it has no host deadline. |
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

### The diag USB console is off by default (run234) — read this before trusting a capture

**The ACM console is no longer built.** `setup_gadget()`, `open_usb_sink()` and
`guard_usb_handoff()` are gated behind `-DANDROID_USB_CONSOLE`
(`scripts/initramfs-diag/init.c`), which the default build omits.

Why: the diag gadget binds the board's only UDC, so adbd can never bind and
**adb never enumerates on the port kernel**. The `sys.usb.configfs` handoff
that used to release the UDC did not fire, and because `open_sink()` only
falls back to USB when the logdump partition is unavailable — which it never
is — the gadget bought nothing and cost adb. run234 dropped it and adb came up
~35 s after boot. Re-enable with `-DANDROID_USB_CONSOLE` if a live console is
ever needed again.

**This is what produced the false "reset".** With the gadget bound, every
capture stopped at ~42-43.7 s, and for many runs that cutoff was read as the
board dying. It was not: the logdump snapshots show the port kernel running to
1695.9 s (run231), 331.4 s (run232) and 323.5 s (run233), each ending
mid-`init` with no panic, SError or reset markers. Any conclusion drawn from
where a capture stopped — the "~89 s reset", the "~47 s silence", the
PS_HOLD/cold-boot and TrustZone-watchdog theories — is void. **Read the
logdump, not the capture length, before calling anything a reset.**

Boot and collect evidence without the ACM stream:

```sh
bash porting/logging/clear-logdump.sh <run>            # while stock adb is up
adb -s 2819f2320604 reboot bootloader
fastboot -s 2819f2320604 boot boot-run<N>.img         # RAM only, nothing flashed
# poll adb; on the port kernel it comes up in ~35 s
bash porting/logging/pull-run-evidence.sh <N> boot-run<N>.img   # after a later stock boot
```

`usb-boot-capture.py` still works for images built with
`-DANDROID_USB_CONSOLE`, and `package-diag-boot.py` refuses to overwrite an
existing `--out` file — a silent no-op that looks like "the flag was ignored".
Use a fresh name.

A transient `PermissionError` on `/dev/ttyACM0` is a udev race (the node is
recreated root-owned 0600 on each re-enumeration), not a device fault. Retry
the open; don't conclude anything from the first failure.

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

## Opt-in early-reset diagnostics (2026-10-13; device verification pending)

Prepared after run82 returned to stock without an observed 6.12 ADB window.
The user builds the image; no diagnostic-image boot verdict exists yet.

### What changes

- `ramoops.reset_capture=1` enables a blocking reboot notifier before
  `device_shutdown()`. It saves caller PID/comm, reboot action/command,
  timestamp and up to 65024 bytes of kernel-log tail to logdump, then flushes.
  The 65536-byte record starts at `0x3f30000`, outside the snapshot slots,
  stall witness and QSEE trace ring. Default is off; no new Kconfig or exports.
- The notifier has its own preallocated buffer and serialization lock.
  Periodic snapshot I/O does not own that lock. Remove unregisters the
  notifier before freeing its buffer or closing the held block device.
- With capture enabled, the snapshot thread prints a heartbeat every ten
  successful samples. This is an execution/write-progress witness, not proof
  of boot completion. Emergency resets, panics and hardware watchdog resets
  do not invoke this sleeping notifier; existing snapshots remain essential.
- Build diagnostic init with `-DANDROID_CAPTURE -DANDROID_EARLY_LOGCAT`.
  The pre-policy guard appends an init-managed service to a read-only bind
  overlay of `/system/etc/init/logd.rc`, preserving its original contents.
  Android init starts a root collector in the existing shell domain on
  `post-fs-data`. It writes `/data/local/tmp/ack-early-logcat.log` with four
  rotated backups, 1024 KiB each, before host ADB is required. No policy delta.
- Logcat needs mounted `/data` and a working logd. Failures before that point
  still require kmsg/reset evidence. File output is not explicitly fsynced
  per record, so abrupt resets may lose the last records. Service startup,
  on-device overlay parsing and reset-time block I/O remain unverified.
- The notifier preserves normal reboot behavior; it never synthesizes a
  panic. Its synchronous block write/flush can still stall if storage is
  unresponsive. Use only on this diagnostic image, not as a production fix.

### Build and package on the known-good Linux build machine

Keep the run73/run81 merged configuration, toolchain environment and DTB.
Do not rerun an old fragment merge or use a host-architecture make command.
Build the kernel with the existing arm64 wrapper/environment; the only
kernel source change for this experiment is the pstore diagnostic extension.

From the kernel repository root, using the static arm64 toolchain for the
diagnostic init:

```sh
aarch64-linux-gnu-gcc -Os -static \
  -DANDROID_CAPTURE -DANDROID_EARLY_LOGCAT \
  scripts/initramfs-diag/init.c \
  -o porting/artifacts/snapshot60/init-run<N>
```

**Toolchain correction (run234).** This section previously said
`aarch64-linux-musl-gcc`, and the tracker elsewhere mentions
`zig cc -target aarch64-linux-musl`. Both are wrong for recent runs: neither
`zig` nor any musl cross-toolchain is installed on this host, and the shipped
`init-run228` is demonstrably **glibc**, not musl — its fingerprint carries
`_IO_stdin_used` and `__libc_start_main` with no musl `__init_libc`, and
`guard_usb_handoff` disassembles instruction-identical to a build made with
`aarch64-linux-gnu-gcc -Os -static`. Use the gcc command above.

Do not add `-DANDROID_USB_CONSOLE` by default; see the section above on why the
diag gadget is off. Add it only when a live console stream is specifically
needed, and expect adb to stop working.

`BOOT_DEADLINE_SECONDS` must stay zero. Keep any additional defines the
previous init used.

After building fresh `out/arch/arm64/boot/Image.gz` and the unchanged DTB:

```sh
python3 porting/logging/package-diag-boot.py \
  --base-image porting/artifacts/snapshot60/boot-run81.img \
  --kernel-dir porting/artifacts/snapshot60/kdir-run83 \
  --stage-kernel out/arch/arm64/boot \
  --init porting/artifacts/snapshot60/init-run83 \
  --keep-stock-console \
  --append-cmdline ramoops.reset_capture=1 \
  --out porting/artifacts/snapshot60/boot-run83.img \
  --report porting/artifacts/snapshot60/package-run83.json
```

Use the real run81 image as base, not a reconstructed capture. Do not pass
`--policy-append`, `--debuggable` or a firmware staging flag: the base already
contains the verified policy, props, firmware and guard inputs. The packager
asserts all other cpio records/header fields are preserved. Confirm the
staged DTB equals the run81 DTB before testing. Supply the image, package
report and init binary for offline inspection before any device boot.

`--keep-stock-console` preserves the run81 command line verbatim (including
its existing `console=ttynull`) before appending the opt-in parameter. Without
it, the packager rejects this base because console is not its final token.
The resulting header command line is 483 bytes, below the 512-byte limit.

### Collect and interpret

Archive any existing `ack-early-logcat.log*` before a repeat, then remove only
those diagnostic files after verifying the archive. Logcat appends to
existing files; without this step an old file is not fresh boot evidence.
Archive current raw/SD evidence, prove preclear, and use an unused run tag.
Keep USB and continuous host logcat capture alongside the on-device service.
Observe at least eight minutes unless the port demonstrably exits. Detect
return to stock after transport loss even if port ADB was never observed.

After returning to an authorized rooted stock ROM:

```sh
# Run in porting/artifacts/snapshot60/.
bash ../../logging/pull-run-evidence.sh 83 boot-run83.img
python3 ../../logging/decode-reset.py run83-logdump.bin
adb -s 2819f2320604 pull /data/local/tmp/ack-early-logcat.log
```

Also retrieve existing `.1` through `.4` backups into the same unused run
evidence directory, without overwriting another run's files. If ADB appears
on 6.12, capture `getprop init.svc.ack-early-logcat` and the current kernel.
A valid `ACKRESET1` record proves the orderly notifier ran and its bytes were
retrieved; successful runtime flush is reported separately in kmsg. An absent
record does not establish a hardware reset: early storage unavailability,
concurrent reset capture, failed I/O or an abrupt failure can also omit it.

## Reset logging repair after run106

Earlier pstore-blk failure descriptions above refer to the pre-repair images.
Built-in PARTLABEL opens now resolve partition metadata on each retry, but
generic block I/O still supplies no panic_write and cannot share the existing
diagnostic partition owner. Select ramoops and drop inherited pstore-blk
cmdline tokens; do not mistake a resolved label for crash durability.

Stock ramoops uses a different reservation from this port. The evidence
collector now recovers committed raw-console/dmesg slots and separate periodic
printk tails directly from logdump, with integrity/provenance metadata.
Full layout and next-image instructions are in `logging/README.md`,
"Tissot reset persistence after run106". Run107 subsequently verified the
user's rebuilt kernel/DTB: a controlled deadline reboot persisted ACKRESET1
and four CRC-valid raw/printk slots, recovered after stock returned.
See `TRACKER-kgsl-port.md`, "Run107". This is not a panic/watchdog verdict.

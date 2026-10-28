# Working rules for this repo
Don't do clean builds unless required, use the default /out dir no need to create out dir per run.

## Agent workflow

Superpowers is disabled for this project. Do not invoke the Superpowers plugin
or its separately installed skills unless the user explicitly re-enables it.

## Use the kernel development skill before any kernel edit

Read the kernel development skill (`skill://kernel-development-skills`, with
the reference that matches the task: `drivers.md`, `devicetree.md`,
`debugging.md`, `patches.md`, `review.md`, `regressions.md`, `stable.md`,
`tree.md`, `build.md`, `testing.md`, `archaeology.md`) **before** editing any
kernel source, Kconfig, binding or DTS file, and follow its verification
matrix for the change. Run its bundled scripts where they apply:
`scripts/triage_kernel_change.py`, `scripts/check_kernel_api_patterns.py`,
`scripts/index_kernel_docs.py`.

Do the same for porting work. The skill is not a formality here: the port has
already paid for a boot cycle once because a probe panic was silent at the
wrong initcall level.

## Boot testing + kernel-log evidence

`porting/REFERENCE-boot-testing.md` is the operational reference: tooling
(`usb-boot-capture.py`, `package-diag-boot.py`, `clear-logdump.sh`,
`pull-run-evidence.sh`), the snapshot layout, and the 6.12 initcall-ordering
trap that turns a driver-probe panic into a completely silent failed boot
(`pstore_blk` is level 7, driver probes are level 6). Read it before any
boot cycle; boot cycles are the scarce resource.

Before coming to conclusion do a online research for cross-verification always.

## Verify before concluding

Do not report a cause, a fix, or a regression from a single run or from
inference. Before stating anything about device behaviour:

1. **Reproduce it.** Get the failing log or the failing command in front of
   you, and name the file and line it came from.
2. **Check whether it ever worked.** `git log --all --oneline -i --grep='<symptom>'`
   and read the matching commit message in full. Commit bodies in this repo
   carry the evidence: which run, which byte count, which errno. On this port
   several subsystems were already solved and the answer was in the history
   the whole time.
3. **Research upstream.** For anything kernel-side, check current upstream
   docs and the AOSP/kernel sources for the symbol or subsystem before
   theorising. `Documentation/usb/gadget_configfs.rst`,
   `Documentation/usb/functionfs.rst`, `Documentation/admin-guide/pstore-blk.rst`,
   and the `*.rc` files under `system/core/rootdir` are the primary sources.
   A kernel bringup is not a place to guess at platform behaviour.
4. **Diff against the last known-good artifact**, not against theory. A boot
   image from a run that demonstrably worked is ground truth; a file you
   reconstructed from a capture is a hypothesis.

Say "unverified" when it is unverified. An unverified claim presented as a
conclusion costs a phone boot cycle, and the boot cycles are the scarce
resource here.

## Forward-porting drivers from 4.19 to 6.12

Read [the driver forward-porting reference](porting/REFERENCE-driver-forward-porting.md)
before adapting imported driver APIs or compatibility shims.

1. Search local history for prior fixes, then find the upstream conversion
   commits and comparable in-tree callers. Read the full conversion series
   where needed; check current upstream and AOSP sources against the target
   6.12 implementation before choosing a replacement.
2. Trace every affected caller, acquisition/release pair and error path.
   Check locking, ownership, lifetime, DMA/cache semantics and the vendor
   userspace ABI. Matching function signatures or compiling is insufficient.
3. Prioritize DMA-BUF reservation locking and cleanup, page references versus
   DMA pins, and compatibility shims that may drop donor behavior. These are
   review targets, not established causes of device failures.
4. Use existing Coccinelle rules in report mode for a focused subsystem;
   add a rule only for a repeated, understood pattern. Review false positives.
   Use Sparse when its annotations can check the suspected problem. Neither
   tool proves device behavior or replaces the evidence requirements above.
5. Record the donor behavior, upstream commit/source references, affected
   paths and focused checks. Obtain the required confirmation before edits;
   do not start a kernel build unless the user asks. Documentation examples
   do not authorize builds, source rewrites or boot cycles.

## Do not reconstruct known-good artifacts

The diag ramdisk is not reconstructible from captured evidence
(`evidence/capture/` in `tissot-port-notes`). The working
base carries a patched SELinux policy (`userdebug_plat_sepolicy.cil`,
3.3 MB) and a `force_debuggable` trigger that nothing in the tree regenerates.
Rebuilding a base from a stock `boot_a` or from a captured fstab produces an
image that packages cleanly, boots, and then stalls in userspace several
stages later than it should — which reads exactly like a kernel bug and is not.

When a base image is needed, extract a real one and change only what is
being tested. Since the 2026-10-28 split the tracked base and boot images
(`porting/artifacts/**/*.img`) are in the history of `../tissot-port-notes`,
not this repository:

```bash
# the last commit that added or changed it (the newest one may delete it)
c=$(git -C ../tissot-port-notes log --all --diff-filter=AM --format=%h -1 -- porting/artifacts/snapshot60/base-guard-debug-fw.img)
git -C ../tissot-port-notes show "$c:porting/artifacts/snapshot60/base-guard-debug-fw.img" > base.img
```

## Deleting build artifacts

Snapshot images and logdumps are gitignored (the "Diagnostic boot artifacts"
block of `.gitignore`), so `rm` on
them is unrecoverable. Before deleting anything under `porting/artifacts/`,
check whether it is the only copy of a base image. Recover a needed one from
the `tissot-port-notes` history first, or the next build has nothing to
package against.

## Repository layout

This repository is the Android Common Kernel (android16-6.12, synced to
6.12.96) plus the tissot port. GitHub: `kurision/tissot-android16-6.12-bring-up`
(remote `origin`, host alias `github-personal`), default branch `main`.

| Path | Contents |
|---|---|
| ACK tree (`arch/`, `drivers/`, `sound/`, …) | Upstream ACK, the port's changes to it, and the imported CAF 4.19 drivers |
| `porting/tools/` | `build-kernel.sh`, the full port configuration `kernel.config`, the module load order `ack-modules.load` |
| `porting/logging/` | `package-diag-boot.py`, `package-kernel-update.py` (the release ZIP) |
| `porting/anykernel/` | AnyKernel3 template for the flashable ZIP |
| `porting/policy/port-genfs-kgsl.cil` | SELinux fragment for a diagnostic ramdisk |
| `porting/*.md` | README, current state, failing components, the driver-rewrite backlog, reports, and the boot-testing and forward-porting references |
| `scripts/initramfs-diag/` | The ramdisk `/init` the release ZIP ships |
| `.github/workflows/kernel-release.yml` | Release build |
| `BUILD_TISSOT.md` | Build entry point |

Outside this repository:

- `../tissot-port-notes` (own Git history, no remote yet): findings,
  trackers, plans, tests, staged files, drafts, evidence and the tracked
  boot images, under the same `porting/...` paths they had here. A
  `porting/findings/...` reference in a document here resolves there.
- Local only, gitignored: session handoffs (`porting/HANDOFF*.md`,
  `docs/handoff-*.md`) and `porting/artifacts/`.
- `../android-6.12-port-work-slop-backup-20261028.git`: mirror of the history
  before the 2026-10-28 rewrite. Do not delete it without the user's say-so.

Do not add notes, findings, trackers, handoffs, captured logs or boot images
to this repository; they go to `tissot-port-notes` or stay local.

## History rewrite of 2026-10-28

All branches and the `v*` tags were rewritten and force-pushed once: handoff
files purged, notes and evidence split to `tissot-port-notes`, author
identities and AI trailers normalised (1413 commits became 1043). Commit
hashes quoted in older notes, reports and commit messages refer to the old
history and resolve only in the backup mirror. A clone from before the
rewrite must be re-cloned, not pulled.

Do not rewrite published history or force-push again without the user's
explicit approval for that specific operation.

Both author identities are intentional: `Anil Kumal <anil.kumal@rts.com.np>`
on this Mac and `Anil Kumal <kumalanil34@gmail.com>` on the home machine
belong to two GitHub accounts. Do not merge them.

## Coding standards

Follow the kernel's own rules (`Documentation/process/coding-style.rst`) and
the kernel development skill's `references/drivers.md`:

- Run `scripts/checkpatch.pl --strict` on every change. Code written for the
  port, and the port's edits to upstream files, stay checkpatch-clean apart
  from DT compatibles that still lack bindings.
- `sysfs_emit()` in `show()`, `dev_*()` logging and `dev_err_probe()` in
  probe, scoped cleanup or `goto` unwinding but never both in one function,
  no `BUG_ON()` for validation, no `WARN` a device or user can trigger.
- No bring-up instrumentation in kernel source: no run numbers, no
  `DEBUG runNNN` comments, no `%px`, no hard-coded physical addresses, no
  boot-only dumps. Comments say what the code does and why, not which boot
  found it. Kernel source does not cite `porting/` documents; the commit
  message carries the evidence.
- Imported trees keep their donor style until they get their own cleanup
  pass: the CAF 4.19 drivers (camera, mdss, vidc_3x, kgsl, audio, qseecom,
  diag, ion), KernelSU-Next, and the third-party BORE and ADIOS patches.
  Do not restyle them as a side effect of another change.
- Remove code nothing builds instead of carrying it. Decide reachability
  from the Makefiles and the compiler's include search order, not from
  file names.

## Commit standards

Follow `Documentation/process/submitting-patches.rst`:

- Subject `subsystem: imperative summary`, using the prefix the file's
  history uses (`git log --oneline -- <file>`): `ASoC: msm:`,
  `iommu/qcom:`, `arm64: dts: qcom: msm8953:`, `soc: qcom: subsystem-compat:`.
  `treewide:` only for a mechanical change across subsystems; `porting:`,
  `ci:` and `gitignore:` for this repository's tooling.
- Body: why the change is needed and the evidence (log line, run, file and
  line), and what was and was not verified. Write "not built" or
  "unverified" when that is the case.
- One logical change per commit; every commit builds on its own. Stage only
  the files the commit is about.
- Trailers:
  - Never add `Signed-off-by:`. It certifies the Developer's Certificate of
    Origin, so only the human author adds it (`git commit -s`).
  - Record AI or tool help as `Assisted-by: AGENT:MODEL`, for example
    `Assisted-by: Claude:claude-opus-5-5`. Never `Co-Authored-By:` or
    `Co-developed-by:` for a tool.
  - `Fixes: <12-char sha> ("subject")` when repairing an earlier commit.
  - `Reviewed-by:`, `Tested-by:`, `Acked-by:` only when that person gave
    them.

## Releases and CI

Pushing a `v*` tag, or running the workflow by hand with an existing tag,
builds the flashable ZIP on GitHub's `ubuntu-26.04` runner (LLVM 23 from
apt.llvm.org, distro pahole 1.31) and publishes a prerelease whose notes
list every commit subject since the previous release. A branch push builds
nothing. ccache and `bindgen` are cached per ref: tag builds reuse only what
a run on `main` saved, so seed the cache with a manual run from `main`. A
failed build uploads its log and config diff as a run artifact. The CI build
is the compile check for changes made on this Mac, which has no build tree.

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

The diag ramdisk is not reconstructible from `evidence/capture/`. The working
base carries a patched SELinux policy (`userdebug_plat_sepolicy.cil`,
3.3 MB) and a `force_debuggable` trigger that nothing in the tree regenerates.
Rebuilding a base from a stock `boot_a` or from a captured fstab produces an
image that packages cleanly, boots, and then stalls in userspace several
stages later than it should — which reads exactly like a kernel bug and is not.

When a base image is needed, extract a real one
(`git cat-file -p <blob> > base.img`) and change only what is being tested.

## Deleting build artifacts

Snapshot images and logdumps are gitignored (`.gitignore:189`), so `rm` on
them is unrecoverable. Before deleting anything under `porting/artifacts/`,
check whether it is the only copy of a base image. Recover a needed one from
git history first, or the next build has nothing to package against.

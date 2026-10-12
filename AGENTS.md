# Working rules for this repo

## Boot testing + kernel-log evidence

`porting/REFERENCE-boot-testing.md` is the operational reference: tooling
(`usb-boot-capture.py`, `package-diag-boot.py`, `clear-logdump.sh`,
`pull-run-evidence.sh`), the snapshot layout, and the 6.12 initcall-ordering
trap that turns a driver-probe panic into a completely silent failed boot
(`pstore_blk` is level 7, driver probes are level 6). Read it before any
boot cycle; boot cycles are the scarce resource.

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

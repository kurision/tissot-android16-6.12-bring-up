# Driver forward-porting: 4.19 → 6.12

> **Current state: CURRENT-STATE.md.** The entries below are the
> running log and are ordered newest first; the top entry is not
> necessarily the current state. Build, package and flash:
> [BUILD_TISSOT.md](../BUILD_TISSOT.md).

Research recorded 2026-10-18. This is a workflow reference, not a new device
diagnosis or authorization to edit code, build a kernel or boot the phone.

## Findings from the article

LWN's [Tools for porting drivers](https://lwn.net/Articles/739578/)
(2017-11-27) describes gcc-reduce, which filters compiler diagnostics and
collects information for porting queries, and Prequel, which searches Git
history for matching API conversion examples. Prequel searches patches;
Coccinelle matches source patterns and can propose transformations.

The useful method for this port is to find how upstream converted another
driver using the same interface, then trace the conversion through our
callers and cleanup. A conversion may span several commits. Compiler errors
locate some API changes, but a clean compile does not establish equivalent
locking, memory ownership or hardware behavior.

The original Prequel/project links failed during this research. Its present
usability and maintenance status are unverified; a failed link does not prove
abandonment. Start with Git history and the existing kernel Coccinelle rules;
defer installing Prequel or gcc-reduce until a concrete need justifies it.

## Research and review workflow

1. Start with an existing failing log/command or a specific API-contract
   question. Record the file and line. Search this repository's history for
   prior work and read matching commit bodies before proposing a change.
2. Locate the donor implementation and every affected caller. Identify the
   resource acquisition, success path, error cleanup and final release.
3. Find the upstream API conversion and comparable in-tree users. Read the
   commit body, patch and related series. Compare current upstream and AOSP
   implementations with the actual ACK 6.12 source: later APIs or vendor
   extensions may differ from this target.
4. Check semantic differences: reservation and other locks, reference counts,
   DMA mappings and cache synchronization, page pins, ownership transfer,
   failure handling, and vendor ioctl numbers/structure layouts.
5. Explain the exact files and proposed edits, their purpose and verification;
   obtain explicit confirmation under the workspace rules before editing.
6. Make the smallest complete conversion, including affected error paths.
   Use focused host checks or static analysis where useful. Build only when
   the user asks. Device claims still require the evidence process in
   [AGENTS.md](../AGENTS.md) and the operational guidance in
   [REFERENCE-boot-testing.md](REFERENCE-boot-testing.md).

Read-only search examples, run from the relevant source repository:

```sh
# Local evidence: substitute the actual symptom or symbol.
git log --all --oneline -i --grep='<symptom-or-symbol>'
git show <matching-commit>

# Source call sites and upstream conversion candidates.
rg -n '\b<symbol>\b' drivers include
git log --all --oneline -S '<symbol>' -- <subsystem-path>
git log --all --oneline -G '<changed-call-pattern>' -- <subsystem-path>
git show <conversion-commit> -- <subsystem-path>
```

`-S` finds changes in the number of occurrences; `-G` matches changed lines.
Neither establishes semantic equivalence. Search the relevant upstream/AOSP
history as well as local port history. This checkout had neither `v4.19` nor
`v6.12` tags when inspected; do not assume those revision names are available
for a range query.

## Priorities for this port

These are focused review targets, not newly established defects or causes of
the unresolved rendering/reset behavior.

| Area | What to verify |
| --- | --- |
| DMA-BUF imports in KGSL/MDSS | Reservation-lock requirements, map/unmap pairing, attachment/reference lifetime, SG validation and cleanup after partial failure. |
| User-page acquisition and release | Whether the actual use needs ordinary page references or DMA pins; matching release APIs, dirty handling and long-term restrictions where applicable. |
| Compatibility shims | Preservation of donor ownership, synchronization, return values and failure semantics; detect inert replacements or unsupported paths. |
| Vendor-facing interfaces | Existing ioctl numbers, structure layouts and behavior expected by the shipped userspace. |

An existing example is commit `5f5f8ccce` (`fix(kgsl): honor 6.12 memory
ownership contracts`). It changes KGSL DMA-BUF mapping/cleanup and SCM
source-owner conversion. Read its full message and the run210 section of
[HANDOFF.md](HANDOFF.md) for the checks and limits: device execution of the
DMA-BUF imports or secure-memory transfers was not established by that run.
Do not turn this historical example into a claim that it fixed rendering.

The DMA-BUF locking convention is documented locally under `DOC: locking
convention` in [dma-buf.c](../drivers/dma-buf/dma-buf.c). Importers must hold
the reservation lock for `dma_buf_map_attachment()` and
`dma_buf_unmap_attachment()`. The corresponding `_unlocked` wrappers acquire
the lock internally and must be called without it held. Choose according to
the caller's actual locking context; do not blindly rename functions.

For pages, follow the use cases in
[pin_user_pages.rst](../Documentation/core-api/pin_user_pages.rst). Changing
an acquisition API requires reviewing all release/error paths; a blanket
`get_user_pages()` → `pin_user_pages()` substitution is insufficient.

## Focused static checks

Coccinelle is useful for finding repeated source patterns once the relevant
contract is understood. This tree already has `scripts/coccinelle/` and
[usage documentation](../Documentation/dev-tools/coccinelle.rst). Prefer an
existing relevant rule, report mode, and one imported subsystem. A custom
rule is justified only for a repeated, understood mistake. Generic rules
are not a complete 4.19 → 6.12 migration checker; review each report against
its surrounding code.

Example for a future focused check, using a selected rule that supports
report mode and the established build environment:

```sh
make coccicheck MODE=report M=drivers/gpu/msm COCCI=<selected-rule.cocci>
```

This is a static-analysis target, not an instruction to build the kernel.
It can produce generated files or require configuration; respect workspace
edit approval and the established output-directory setup. Review proposed
transformations before any source rewrite.

Sparse complements this with checks based on pointer/address-space,
endianness and locking annotations; it cannot check every ownership rule.
See [sparse.rst](../Documentation/dev-tools/sparse.rst). Kbuild's `C=1` and
`C=2` modes integrate Sparse with compilation, so run them only as part of
a user-requested build/check with the appropriate target and toolchain.

No Coccinelle/Sparse run, kernel build or phone boot was performed for this
research. Tool availability and effectiveness on this port are unverified.

## Upstream sources

The workflow was cross-checked against these target-version references:

- [Linux 6.12 Coccinelle documentation](https://docs.kernel.org/6.12/dev-tools/coccinelle.html)
- [Linux 6.12 Sparse documentation](https://docs.kernel.org/6.12/dev-tools/sparse.html)
- [Linux 6.12 DMA-BUF documentation](https://docs.kernel.org/6.12/driver-api/dma-buf.html)
- [Linux 6.12 page-pinning documentation](https://docs.kernel.org/6.12/core-api/pin_user_pages.html)
- [Linux 6.12 driver-interface rationale](https://docs.kernel.org/6.12/process/stable-api-nonsense.html)

For a concrete conversion, additionally record the upstream/AOSP commit and
target source that establish its semantics. The general references above
are not a substitute for checking the implementation being ported.

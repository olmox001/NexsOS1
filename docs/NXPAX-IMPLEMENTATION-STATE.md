# NXPAX — implementation, evidence and acceptance state

Date: 2026-11-10. Branch: DEV-Resurrection. Status: implemented and tested in
the current worktree; **Power12 qualification OPEN; release gate BLOCKED**.

## Authority and non-regression boundaries

The source of truth is the checked-out code, current Git commit and the current
documentation: ASTRA, DIR-05/06, NEXS-NG diagnostics/testing, NX-HAL
PLAN/STATE/NXOLMO integration, and the NASA/JPL Power-of-Ten document.
Rejected historic NXPAX designs and Desktop Commander call history are not
implementation inputs. The active object-only ABI migration is authoritative;
NXPAX adds neither a syscall number nor a new object type/dispatcher, and it
does not move POSIX policy or userland services into the kernel.

## Architecture and ownership

1. **HAL:** `kernel/HAL/core/fault_protocol.c` records machine exception facts
   before delegating to the existing Core fault handler. The existing amd64 and
   aarch64 exception entry/decoding paths and HAL fault protocol remain in
   place. `kernel/HAL/core/nxpax.c` uses a static typed ring, monotonically
   assigned sequences, per-slot atomic nonblocking acquisition, and an
   explicit drop counter. Events include raw pc, address, syndrome, fault
   privilege, fault-safe CPU ID, and optional disposition data. The ring has
   `2 * NXHAL_CPU_LIMIT = 128` slots: a resource-window budget derived from
   the maximum 64 HAL CPU entries and the hardware/Core two-stage reporting
   model, **not** a guarantee that a burst cannot exceed the capacity.
   The ring allocates no memory and takes no sleeping locks in a fault.
2. **Kernel/Core:** `kernel/core/fault.c` retains user-fault termination,
   kernel-fatal reporting, panic/backtrace and screen fallback. The
   `kernel/core/nxpax.c` adapter adds disposition records without changing
   exception classification. `kernel/resources/nxpax.c` registers the
   `diagnostics/nxpax` resource through the existing `OBJ_TYPE_RESOURCE`
   path, not a dedicated syscall. Acquisition requires privileged process
   level and **exactly OS1_RIGHT_READ**; neither TRANSFER nor DUPLICATE is
   granted. Direct reads copy a typed request from userspace, enforce ABI
   version/record size/operation ID, return at most one event, account one
   attempted slot read, report cursor/newest/overrun/drops and copy the result
   back using the existing guarded user-access path. No raw addresses are
   published through a public nonprivileged handle.
3. **Userland:** `user/sys/bin/nxpax.c` is a supervised, privileged broker,
   started/restarted by `user/sbin/nxinit.c` using the existing bounded
   restart backoff (slot 10). `nxpax --service` reads the resource and serves
   the existing `OS1nx_pax` named capability port; reply rights travel
   through the existing transferred-pipe mechanism. **PC and fault address
   are cleared** before the client-visible reply; the syndrome is also
   cleared for hardware/fault events, but retained as a non-address text
   fingerprint for typed diagnostic WARNING/ERROR records. `nxpax --probe`
   traverses at most the published ring-window budget. The broker is not a
   recovery authority: if Core/HAL is no longer alive, this process cannot
   repair the kernel and does not claim to do so.

The shared ABI is mirrored exactly in `kernel/include/abi/nxpax.h` and
`system/include/contract/nxpax.h` (88-byte query). Read has stable operation
ID `OS1_NXPAX_OP_READ_RECORD`. `work_units` counts attempted ring reads and
does **not** purport to count clock cycles. Its ABI hash is tracked by the
kernel contract snapshot and identical headers are checked by the Makefile.

The HAL diagnostic callback binder was also repaired to use atomic
compare/exchange, release publication and acquire observation rather than
unsynchronized volatile state.

## Warning observability regression correction (2026-11-10)

The fatal and ordinary-log transports remain distinct. The earlier diagnostic
protocol required vlog, vfault and vpanic together; the current
implementation requires vlog at the diagnostic bind and separately requires
fault-safe vprint and vpanic at the mandatory fault-output bind. Both binds
run during Core/HAL boot. This separation MUST NOT remove diagnostic events.

Kernel printk now captures the **actual formatted** warning/error text
emitted by both Core pr_warn/pr_err and HAL nxhal_diag_warn/
nxhal_diag_error and appends an NXPAX event to the production HAL ring.
The two additive public event kinds are DIAGNOSTIC_WARNING (5) and
DIAGNOSTIC_ERROR (6). The ring retains a bounded 64-bit fingerprint of the
displayed text, the recording CPU, sequence and severity kind. The full text
continues to print on UART; it is not invented from a synthetic test string.
The privileged broker exposes the fingerprint, not arbitrary kernel text or
addresses. The 88-byte public record layout and existing op ID are unchanged.
The synchronized contract snapshot hash is updated; the two public headers
must remain byte-for-byte identical.

An NXPAX USER_TERMINATED disposition is now shown as WARN by the CLI because
the kernel has recovered by terminating the faulty process. TERMINATION_FAILED,
KERNEL_FATAL and hardware fault records keep distinct severities. An ordinary
error message is NOT automatically converted to WARN: a caller must classify
the operation's recoverability. A nonrecoverable kernel exception still panics.

Verification of this correction: NX-HAL host suite PASS, Core NXPAX
ASan/UBSan PASS, 1M- and 2M-event production-ring stress PASS (with real
contention drop accounting), and make check-kernel-contracts PASS. Fresh
interactive NXADB tests passed on AMD64 GRUB BIOS (28 kernel tests passed,
no failures) and AArch64 GRUB UEFI (29 passed, no failures); timerdiag,
heartbeat and shell interaction also passed. Real serial logs show
[NXPAX][WARN] kind=5 and newest=1 dropped=0 for AMD64, and two FDT
warnings with newest=2 dropped=0 for AArch64. Logs:
/tmp/nexs-nxpax-warning-{amd64,aarch64}-20261110.log and the matching
build/runtime/nxadb/.../serial.log files. The same source was subsequently exercised with the real /bin/crash
executable on both architectures. On AMD64 the hardware event was seq=2
kind=1 and the actual terminated process (PID 16) was seq=3 kind=2
displayed as WARN; AArch64 recorded the corresponding events at seq=3
and seq=4 (PID 16). Both probes reported dropped=0, both shells remained
interactive, and neither UART contained an unexpected KERNEL PANIC.
The crash runs passed NXADB with 28/28 and 29/29 kernel tests respectively.
Evidence: /tmp/nexs-nxpax-warning-crash-{amd64,aarch64}-20261110.log
and matching serial logs. This verifies warning and actual fault event
transport; it does not claim complete classification of silent error
return paths or complete Power12 qualification.

## Reproducible verification already performed

- `make -C kernel/HAL test-host`: PASS. This includes the real NXPAX HAL
  ring tests for empty/invalid requests, typed records, CPU IDs, overwritten
  events, wrapping and dropped-record accounting.
- `make -C kernel/HAL test-sanitize`: PASS (AddressSanitizer and UBSan),
  repeated after the nonwaiting reader ownership CAS change.
- `make test-nxpax-stress`: PASS with the real HAL ring linked under
  ASan/UBSan and pthread concurrency. Scenario A processes 1,000,000
  hardware events and concurrent reads; scenario B processes 2,000,000
  events from two writers and a reader. Successful snapshots are verified
  against independently generated PC/address/syndrome/CPU checksums.
  The logger reports genuine nonzero drops under contention (not hidden or
  reclassified as success); the test establishes integrity and accounting,
  **not** lossless forensic retention under arbitrary load.
- `make test-nxpax-core`: PASS under ASan/UBSan, linking the actual Core and
  HAL modules; separately verifies user termination and fatal disposition
  data, including the CPU ID.
- `make ARCH=amd64 kernel`, `make ARCH=aarch64 kernel` and separate user
  ELF builds for both architectures: PASS for the latest tested sources.
- Both `make ARCH=amd64 RUN_DISPLAY=none RUN_SERIAL=stdio run` and
  `make ARCH=aarch64 RUN_DISPLAY=none RUN_SERIAL=stdio run` reached an
  interactive NXShell through their respective GRUB BIOS/UEFI paths.
  QEMU was terminated intentionally by `gtimeout 45s` after observing
  NXShell; timeout exit code 124 is **not** a spontaneous guest failure.
- `make check-kernel-contracts`: PASS; matched public NXPAX headers and
  a hash-verified kernel ABI snapshot.
- Both `nxadb test -n 1 -c '/sys/bin/nxpax --probe'` guest runs **after**
  the CPU-ID/88-byte ABI expansion: PASS, NXShell interactive, history,
  timerdiag, heartbeat, 24/24 amd64 or 25/25 aarch64 in-kernel tests, zero
  unexpected faults. UART showed the NXPAX service listening and the
  client's `end newest=0 dropped=0`. Latest complete guest logs:
  `/tmp/nxpax-cas-final-amd64.log`, `/tmp/nxpax-cas-final-aarch64.log`.
  These runs also verify the final nonwaiting slot-read CAS implementation.
- Both architectures then ran the **existing** real `/bin/crash` executable
  (null-pointer page fault / lower EL data abort) on the latest ABI. Core
  logged PID 17 and successfully terminated the process; `nxpax --probe`
  read sequence 1 HAL hardware event and sequence 2 Core user-termination
  event, with `units=1` for each record, zero dropped records, and the shell
  still usable. CPU IDs were 3 for both final CAS test runs.
  Actual evidence: `/tmp/nxpax-cas-final-crash-{amd64,aarch64}.log`,
  `/tmp/nxpax-cas-final-probe-{amd64,aarch64}.log` and corresponding UART
  captures. Both guest command sequences returned success.

## Power12 audit — maintenance versus qualification

The project-wide source/AST/mutation pipeline under `tools/power12/` remains
authoritative for Power12 qualification. Development builds run
`make power12` in a **warning-only, content-digest-cached mode** so already
catalogued findings do not prevent `make run` or nxadb. Reports retain
non-zero stage statuses and `verified=false`; the mode never reports
nonconforming code as qualified. A changed audited input invalidates the cache.

`make power12-strict`, `make check-strict`, release and release-arch remain
blocking. The strict gate has open genuine findings in owned source and is
not yet a release acceptance result.

The dedicated NXPAX-specific tools `tools/power12_audit.py` and
`tools/test_power12_nxpax.py` were restored unmodified from HEAD following
the 2026-11-10 accidental deletion. `make test-nxpax-ast` runs the nine
real-C mutation controls; it is additive to the thirteen project-wide
mutation controls, not a duplicate runtime fault implementation. The old
strict NXPAX source audit intentionally reports unresolved proof obligations.

NX-CORE and NX-HAL use a separate compiler warning policy: the baseline
correctness/hardening warnings are errors by default, with
`NX_WARNINGS_AS_ERRORS=0` as an explicit opt-out. Conversion and sign
conversion warnings are present for both domains while the outstanding
narrowing sites are repaired; `NX_CONVERSION_AS_ERROR=1` is the fail-fast
conversion qualification profile. The complete Power12 strict obligation
remains open despite successful runtime boot qualification.

## Remaining acceptance requirements

- Keep both guest boot lanes and real `/bin/crash` fault tests as mandatory
  acceptance gates on every later functional ABI or exception-path change.
- Test permission denial and unsupported request/ABI size and operation ID in
  the guest, plus fault escalation/negative tests (not just successful shell
  and an empty queue). Stress-tested host concurrency is **not** identical
  to interrupt-context SMP execution with actual concurrent guest CPUs.
- Harden reply-pipe validation in the userland broker: the current port wire
  message has no explicit kernel-attested transferred-handle count. The
  existing `sys_port_send_caps` rewrites payload handles, but the ordinary
  PORT send path can carry untrusted payload bytes. The broker must not trust
  or close arbitrary client-supplied indices. Until authenticated capability
  provenance is enforced, a malicious client may induce confused-deputy
  writes/closes of another in-process handle. This is a SECURITY BLOCKER.
- Resolve event-stream gaps before claiming lossless/complete collection:
  sequence reservation is visible before record publication, and a drop can
  leave the next sequence absent. A response with status EMPTY and newer
  sequence available must not be treated as a verified end-of-stream. Provide
  an explicit in-flight/gap protocol with bounded retry and honest loss
  accounting; the current broker is not an archival event journal.
- Extend instrumentation to all required fault categories and coverage points
  after verifying ownership against actual HAL/kernel/userland sources, rather
  than introducing a second fault-dispatch layer.
- Add a complete Power12 proof/test matrix and resolve or formally approve
  individually justified divergences. The strict gate must stay red while
  obligations remain open; never weaken flags, disable checks or fabricate
  acceptance evidence.

No certification, complete Power12 conformance or final product readiness is
asserted. New files and worktree changes are not automatically committed.

# DESIGN 2026-07-18 — nxexec as the privileged execution daemon (R6)

Required by `PIANO-LIBC-ASTRA-2026-07-16` **R6**: *"nxexec demone privilegiato
+ nxauth (password root preset); studio kill-model/exec in kernel (**doc
dedicato prima del codice**)"*. Maintainer chose the daemon shape (2026-07-18)
over "shared library" and "process per command".

Directive being implemented:
> "nxexec deve essere il nostro esecutore standard e va raffinato; dovrà essere
> il punto della compatibilità posix ed è fondamentale per permettere un
> utilizzo grafico e da terminale **senza eccezioni**."

---

## 1. The problem, with evidence

There are **two divergent execution paths** today:

| | launcher / file manager | shell |
|---|---|---|
| how | spawns the **nxexec binary** | spawns the program **itself** (reuses only `nxexec.h` helpers) |
| redirection `< > >> 2>` | ✗ | ✓ |
| pipes `\|` | ✗ | ✓ |
| job control (Ctrl-Z, jobs, fg/bg) | partial (hosted terminal only) | ✓ |
| exit status propagation | ✗ | ✓ (2026-07-17w) |
| identity registration (`sys.proc.<pid>.*`) | ✓ | ✗ |

So every refinement made in one path is invisible to the other. Observed
consequence: `progname` was `/bin/lua` from the launcher but bare `lua` from
the shell, because `nxexec_spawn_search*` spawned `out_path` while leaving
`argv[0]` as the typed name. lua's test suite rebuilds every command as
`"<progname>" ...`, so the bare form forced a re-resolution — the "esecuzione
da path non diretto" the maintainer flagged. (Fixed 2026-07-18 by making
`argv[0]` the resolved path; that is a patch, not the structural fix.)

**The structural fix is one executor**, which is what this document designs.

## 2. Placement in ASTRA

- **§6.4 SRL** — "SRL services are supervised ELF processes, exposed via
  IPC/capability". nxexec is an SRL service, supervised by nxinit.
- **§6.5** — process/IPC/capability (seL4 + Mach + NT). Execution authority is
  a capability, not an ambient check.
- **§6.8** — compatibility personalities. This is where the directive "POSIX
  vive SOPRA il nostro sistema come layer di compatibilità" lands: nxexec is
  the **POSIX compatibility point** for *execution* (argv/env, fd inheritance,
  redirection, pipes, job control, exit status), while the kernel keeps only
  primitives.

## 3. BLOCKING DEPENDENCY — `OBJ_TYPE_PORT` does not exist

ASTRA §7.11 records, still open: *"`OBJ_TYPE_PORT` (IPC-as-capability) doesn't
exist"*. This is a **direct dependency** of the daemon:

- A daemon reached by **ambient pid-addressed IPC** (`SYS_SEND` to nxexec's pid)
  is authorised by "can this process send to that pid", not by holding an
  execution capability. Any process that can name the pid can ask for execution.
- The ASTRA-correct form is a **port capability**: a client holds a handle to
  nxexec's service port; possession of the handle *is* the authority, it is
  attenuable and delegable, and nxinit hands it out at spawn time.

Consequences for sequencing — this is **question Q1** below:
- **(a)** implement `OBJ_TYPE_PORT` first, then the daemon on top (ASTRA-clean,
  larger); or
- **(b)** ship the daemon on existing IPC + the current
  `process_ipc_allowed()` gate, and migrate to ports when they land (faster,
  but the authority model is temporarily weaker than ASTRA specifies).

Recommendation: **(b) with the protocol designed port-shaped** — i.e. define
the request/reply as if it travelled over a port, so the later swap is a
transport change, not a redesign.

## 4. Protocol sketch (transport-agnostic)

Request (client → nxexec): path/name, argv, cwd, spawn flags (detached),
fd redirections `{child_fd, parent_fd}` (already an ABI: `struct spawn_redir`),
requested privilege level, and whether a controlling terminal is wanted.

Reply (nxexec → client): pid, or a negative errno; plus the resolved path so the
client can report it.

Key point: **fd redirection already crosses process boundaries correctly** —
`process_redirect_child_fd()` dups the *spawner's* handle into the child. For a
daemon, the fds belong to the CLIENT, not to nxexec, so the daemon must be able
to install a handle it does not own. That needs either
`sys_cap_grant`-to-daemon (client delegates the fd first) or a
"spawn on behalf of" verb. **This is question Q2.**

## 5. What moves into nxexec

From nxshell (which keeps only line editing, builtins and job *presentation*):
path resolution + `argv[0]` normalisation; `<`/`>`/`>>`/`2>` redirection;
pipelines; the foreground watch loop (Ctrl-C/Ctrl-Z, window-stable GUI
detection); exit-status capture; identity registration.

Everything above already exists and is device-verified — this is a **move**, not
a rewrite. The risk is regression in the terminal path, so §6.

## 6. Migration (must not break the working terminal path)

1. Land the shared *policy* first: one implementation used by both binaries
   (the "library now" option), so behaviour converges and is cross-checked.
2. Introduce the daemon + protocol; route the **launcher/file-manager** path
   through it first (fewer features in flight there).
3. Route nxshell through it, keeping the in-process path behind a fallback until
   the suite (`lua all.lua`) is at least as green as before.
4. Delete the in-process path.

Gate for each step: build both arches + `make run`, and the lua suite must not
regress (it is currently the most demanding exec/redirection/pipe consumer).

## 7. Kill / ctty model interaction

`docs/PROCESS-KILL-MODEL.md` ties window ownership, ctty and subtree kill
together. Moving the spawn into a daemon **changes parentage**: children become
nxexec's, not the shell's. That silently breaks two things unless handled:

- `process_kill_allowed()` is an ancestry walk → the shell would lose kill/stop
  authority over its own jobs (it is no longer their ancestor). Job control
  would need the daemon to hand back a PROCESS capability with DESTROY rights.
- Window-aware subtree kill ("child closed with parent") would treat nxexec as
  the parent of everything.

**This is question Q3** and is exactly the "studio kill-model/exec in kernel"
R6 asked for.

## 8bis. MAINTAINER ANSWERS (2026-07-18) — binding

- **Q1 → (a) PORTS FIRST.** `OBJ_TYPE_PORT` is implemented BEFORE the daemon
  (overriding this doc's original recommendation).  Reinforced by the Q5 answer:
  *"raffina e migliora le capability ipc che sono un punto bloccante"* — IPC
  capabilities are THE blocking point, so they are the immediate work.
- **Q2 → HYBRID of both**, "optimize it for future modularization": support the
  client granting its fds (cap_grant) AND a spawn-on-behalf-of path, factored so
  the transport can be swapped/extended later without redesign.
- **Q3 → (2) LOGICAL PARENT** in the kernel, distinct from the spawning process,
  plus *"introduci process capability, studia su astra"* — so job control
  survives the daemon owning the spawn.
- **Q4 → introduce users + multi-user management NOW, but HARDCODED to a single
  user with access to `/home`** ("controlla user, dovrebbe essere già simile" —
  verify against the existing level/identity model first).  REAL multi-user
  later requires per-user mounts/partitions, via `nxauth` + `nxpopup`.
- **Q5 → nxinit, nxexec, nxntfy_srv (and others) are SYSTEM SERVICES**; refine
  their capabilities rather than leaving the flat ROOT preset (ASTRA §7.11).

Working language note: maintainer questions in Italian from here; reasoning and
all code comments stay in English.

## 9. `OBJ_TYPE_PORT` design (the now-immediate work)

Grounded in ASTRA §6.5: *"ports are first-class objects (a port is itself a
capability); sync RPC + async messaging"* and — decisive for scope — *"the
semantics ride on the existing B3 IPC layer"*: we WRAP the existing IPC, we do
not build a second transport.  The seL4 rule it repairs is *"no PID-by-number
access without a capability"*, which is exactly today's ambient pid-addressed
`SYS_SEND`.

Model (Mach mailbox semantics):
- A port is a kobject owning a message queue + a receiver wait queue — the same
  shape as the `OBJ_TYPE_PIPE` just landed (blocking read, wake on write), so
  the machinery is proven.
- **Rights ARE the port rights**: `OS1_RIGHT_WRITE` = send right,
  `OS1_RIGHT_READ` = receive right.  A service keeps the receive right and hands
  clients attenuated SEND-only handles — unforgeable, delegable via `cap_grant`.
- **Naming** (§6.6: "every node has an associated capability"): a port may be
  published under a name so clients acquire a send capability by name
  (`handle_create(OS1_NS_PORT, "nxexec", WRITE)`), rather than by knowing a pid.
  Acquisition is the ACL point; possession is thereafter the authority.

Why this unblocks everything else: the nxexec daemon is then reachable *as a
capability* rather than as a pid, which is the precondition Q1 set.

### 9.1 IMPLEMENTED (2026-07-18) — kernel + userland

Kernel (`kernel/core/object.c`, `include/abi/object.h`):
- `OBJ_TYPE_PORT` (= 7, `OBJ_TYPE_COUNT` → 8) with `struct kport`: a bounded
  message queue reusing `struct ipc_message` VERBATIM + receiver/sender wait
  queues (the same proven shape as `OBJ_TYPE_PIPE`).
- `OS1_NS_PORT` (= 5) acquisition, which is the ACL point:
  asking WITH `OS1_RIGHT_READ` publishes the port and claims ownership
  (`-EEXIST` if the name is already served — a rogue process cannot steal a
  service identity by racing it); asking WITHOUT it yields a SEND-only
  capability to an existing port (`-ENOENT` if unpublished).
- Send stamps `from` kernel-side (unforgeable) and performs NO
  `process_ipc_allowed()` check — the WRITE right IS the authority, exactly as
  the existing OBJ_TYPE_PROCESS send path documents: ambient checks gate
  ACQUISITION, not use.  Receive blocks while empty with a sender alive, and
  returns 0 once the last sender is gone so a service loop terminates.
- Lifecycle: the endpoint counter that already tracked pipe readers/writers now
  also tracks port senders/receivers at every handle edge.  Unpublishing happens
  when the LAST RECEIVER closes — under `object_lock` — because `kobj_free()`
  runs both inside and outside that lock, so unpublishing at free time would
  race (bug caught during implementation).

Userland (`include/api/os1.h`, `user/sys/lib/lib.c`) — thin personality, no new
transport: `OS1_port_create()` / `OS1_port_open()` / `OS1_port_send()` /
`OS1_port_recv()`.

**SERVICE NAMING STANDARD (maintainer, 2026-07-18): `OS1nx_<service>`.**
First constant: `OS1NX_PORT_EXEC "OS1nx_exec"` (the execution service).
Other system services (nxinit, nxntfy_srv, …) follow the same form as they are
ported onto ports.

### 9.2 VERIFIED on device (2026-07-18)

`captest` 18/18 PASS, 0 failures — the five new port tests included:
`port-create`, `port-name-unique` (a second receiver is refused, so a service
identity cannot be stolen by racing), `port-open-send` (client acquires a
SEND-only capability BY NAME, no pid), `port-roundtrip+from-unforgeable` (the
message was sent with `from = 999999` deliberately forged and the kernel
overwrote it with the real pid), `port-open-missing`.

The unforgeable-`from` result is the load-bearing one: it is the precondition for
the daemon authorising requests by sender identity.

## 10. Q3 IMPLEMENTED — logical parent (`owner_pid`) + process capability

`parent_pid` records who MECHANICALLY spawned a process; `owner_pid` records who
it BELONGS to. They diverge the moment execution moves behind a service: the
daemon is the spawner, but the job belongs to the requesting shell. Since
kill/stop/cont authority is an ancestry walk, without this the shell would
silently lose job control over its own jobs when the daemon lands.

- `struct process.owner_pid` (0 = unset → authority follows `parent_pid`).
- `process_kill_allowed()` walks `owner_pid` when set, else `parent_pid`.
- **`OBJ_CTL_SETOWNER`** — the capability verb that sets it. Authority is
  deliberately DOUBLED: `OS1_RIGHT_DESTROY` on the target AND a privileged
  caller, because setting an owner DELEGATES authority over a process. Without
  the privilege gate a process could hand its child to a third party, or
  re-home ITSELF under a victim to borrow that victim's authority chain.

Two hazards this introduced, both handled:
1. **Loop termination.** The plain parent chain is acyclic because a parent
   always has a smaller PID; `owner_pid` is service-assigned and carries no such
   guarantee, so the existing depth bound stops being "belt-and-braces" and
   becomes the actual termination guarantee. Kept and documented.
2. **Owner death.** If the owner dies, `owner_pid` points at a corpse, the walk
   dead-ends, and the job becomes killable only by a privileged process —
   wedging its pool slot exactly like the unadopted orphans `__reparent_children`
   was written to prevent. It now clears `owner_pid` on owner death, dropping
   back to the always-walkable mechanical chain.

## 11. Q2 RESOLVED + variable-size protocol (2026-07-18)

Maintainer: *"(1) ottimizza (2) non hai nessuna regola, agisci liberamente e
risolvi (3) dobbiamo poter gestire dimensioni variabili"*.

**(3) Variable sizes — the fixed record is gone.** The wire format is now a
fixed HEADER + a VARIABLE body. The header declares `body_len`, which is
validated against `EXECSVC_BODY_MAX` **before** it is used to size a read, so
the service never allocates on a client-chosen number. Body parsing is bounded
by `body_len` at every step and an unterminated string is REFUSED rather than
walked past. Body layout: `redir[]`, then `cwd\0`, then `argv[i]\0`.

**(1) Optimisation falls out of (3):** a short command now sends ~100 bytes
instead of a 1272-byte worst-case record, and `argv` no longer carries an
arbitrary ceiling purely to bound the fixed cost.

**(2) CLIENT_FD authority — decided and implemented, two tiers:**
- KERNEL: `struct spawn_redir` gained `source_pid` (0 = the spawner's own
  table). A non-zero `source_pid` is accepted **only from a PRIVILEGED
  caller** — reaching into another process's handle table is a system-service
  power, not an application one, the same rule and reasoning as
  `OBJ_CTL_SETOWNER`. Without the gate any process could siphon descriptors out
  of any other simply by naming it.
- SERVICE: nxexec further pins the source to `ipc_message.from`, the
  KERNEL-STAMPED requester, so the service cannot be talked into harvesting a
  third party's descriptors even though it is privileged enough to succeed.

Both fd mechanisms now work: GRANTED (client `cap_grant`s the handle; capability
-correct, costs a round-trip) and CLIENT_FD (kernel takes it directly; no
round-trip). The protocol chooses per-redirection, which is the "hybrid,
optimised for future modularization" the maintainer asked for.

Hazards handled while implementing:
- `struct spawn_redir` GREW, so every constructor had to initialise the new
  field; leftover garbage there would have selected the privileged
  take-from-another-process path at random. All four producer sites (nxshell ×2,
  nxexec ×2) audited and set explicitly.
- The service now closes only GRANTED refs on cleanup: CLIENT_FD refs live in
  the CLIENT's table and are not the service's to close. Closing them would
  corrupt an innocent process's fd table.
- Header placement: `execsvc.h` stays SEPARATE from `nxexec.h` (client wire
  contract vs. 526 lines of executor policy across 16 `static inline`s), but
  `nxexec.h` includes it so existing consumers get it transitively — no
  duplication, and a client still links the protocol alone (Phase 12).

Service start-up is deliberately NOT wired into nxinit yet: that belongs to
Phase 12, and putting an unverified service into the boot path would risk the
one thing that must keep working.

## 8. Open questions for the maintainer

- **Q1 — ports first?** Implement `OBJ_TYPE_PORT` before the daemon (ASTRA-
  clean), or ship on existing IPC with a port-shaped protocol and migrate?
  (Recommendation: the latter.)
- **Q2 — whose fds?** How should a client's redirection fds reach a child the
  DAEMON spawns: client grants the handles to nxexec first (`cap_grant`), or a
  new "spawn on behalf of" kernel verb?
- **Q3 — parentage & job control.** With children parented to nxexec, should the
  daemon return a PROCESS capability (WAIT+DESTROY) so the requesting shell
  keeps job control, or should the kernel learn a "logical parent" distinct from
  the spawning process?
- **Q4 — scope of nxauth v1.** `PIANO` decision 3 fixes it at *root with preset
  password, named users NOT in this block*. Confirm nxauth stays that narrow
  here, with the password panel driven by nxexec.
- **Q5 — per-service capabilities.** ASTRA §7.11 notes every `/sys/bin/*`
  service still gets the flat ROOT preset. Should nxexec become the first
  service with a refined, explicit capability set (it is the highest-authority
  one, so it is both the best and the riskiest candidate)?

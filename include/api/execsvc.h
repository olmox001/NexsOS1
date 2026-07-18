/*
 * include/api/execsvc.h
 * Execution service protocol — the contract between a client and nxexec
 * (ASTRA §6.4 SRL service, §6.5 port capability).
 *
 * This is an API header, NOT an ABI one: the protocol never crosses the syscall
 * boundary.  It is carried by userland primitives the kernel already exports
 * (a port capability + a pipe), so include/abi stays reserved for the
 * kernel<->userland contract.
 *
 * ---------------------------------------------------------------------------
 * TRANSPORT — why the request does NOT travel inside the message
 *
 * struct ipc_message carries a 64-byte payload; a realistic request (program
 * path + argv + redirections + cwd) is far larger.  Truncating argv would
 * silently corrupt commands, so the protocol uses the split ASTRA §6.5 already
 * names ("sync RPC + async messaging, OUT-OF-LINE MEMORY TRANSFER"):
 *
 *   1. the PORT message is a small RENDEZVOUS header naming the service (as a
 *      capability, never a pid) and carrying handles to the out-of-line
 *      channels;
 *   2. the request BODY travels through a PIPE the client created and delegated
 *      with OS1low_cap_grant() — arbitrary size, flow-controlled, and already
 *      device-verified;
 *   3. the REPLY returns the same way on a second pipe.
 *
 * The delegation is also what AUTHORISES the transfer: the service receives
 * exactly the two channels the client chose to hand it, and no ambient reach.
 *
 * ---------------------------------------------------------------------------
 * WIRE FORMAT — fixed header + VARIABLE body
 *
 * The body is variable-length by design.  A fixed worst-case record wasted over
 * a kilobyte on every short command AND still imposed an arbitrary ceiling on
 * argv; sizing the transfer to the request removes both problems at once.
 *
 * The header is fixed and declares `body_len`, so the receiver validates the
 * length BEFORE reading and never sizes a buffer from an unbounded client
 * number — the length is checked against EXECSVC_BODY_MAX first.  Every string
 * in the body is NUL-terminated and every parse step is bounded by body_len, so
 * a malformed or hostile body cannot walk off the end.
 *
 * Body layout (packed, in this order):
 *     struct execsvc_redir redir[nredir]
 *     cwd '\0'
 *     argv[0] '\0' argv[1] '\0' ... argv[argc-1] '\0'
 */
#ifndef _EXECSVC_H
#define _EXECSVC_H

#include <caps.h> /* struct spawn_redir, SPAWN_MAX_REDIR, SPAWN_FLAG_* */
#include <os1.h>  /* OS1NX_PORT_EXEC, struct ipc_message */

/* Protocol version: a client and a service built at different times must fail
 * LOUDLY rather than misread each other's layout. */
#define EXECSVC_VERSION 2

/* ipc_message.type values for the rendezvous header. */
#define EXECSVC_REQ_SPAWN 0x4558 /* 'EX': please execute; see field map below */

/*
 * Rendezvous header layout inside struct ipc_message (the only part that must
 * fit in 64 bytes — it carries references, not content):
 *
 *   type       = EXECSVC_REQ_SPAWN
 *   payload[0] = (int) REQUEST pipe READ end   } as the SERVICE sees them
 *   payload[1] = (int) REPLY   pipe WRITE end  }
 *   from       = stamped by the kernel; the service's only trustworthy
 *                statement of WHO is asking, and therefore the basis for
 *                authorising the request and assigning the job's logical
 *                owner (Q3).
 *
 * The client sends with OS1_port_send_caps(port, &msg, {req_r, rep_w}, 2) and
 * does NOT fill those slots itself: a handle index means something only inside
 * ONE process's table, so the kernel TRANSLATES the transferred handles into
 * the receiver's table and writes the receiver-side indices here.  Putting the
 * client's own numbers on the wire would hand the service meaningless slots —
 * which is exactly the bug this layout note exists to prevent.
 */

/*
 * How the service obtains the descriptor backing one redirection — the Q2
 * hybrid, so both mechanisms coexist without a protocol break.
 *
 *   GRANTED:   the client already delegated the handle with OS1low_cap_grant(),
 *              so `ref` indexes the SERVICE's own table.  Capability-correct:
 *              an explicit, per-handle delegation the client consented to.
 *              Costs a grant round-trip per fd.
 *
 *   CLIENT_FD: `ref` indexes the CLIENT's table and the kernel takes it
 *              directly (spawn_redir.source_pid).  No per-fd round-trip.
 *              AUTHORITY RULE: the kernel permits a non-zero source_pid only
 *              from a PRIVILEGED caller — the same rule as OBJ_CTL_SETOWNER,
 *              and for the same reason: reaching into another process's handle
 *              table is a system-service power, not an application one.  The
 *              service additionally restricts the source to the REQUESTER
 *              (ipc_message.from), so it can never be talked into harvesting a
 *              third party's descriptors.
 */
#define EXECSVC_FD_GRANTED   0
#define EXECSVC_FD_CLIENT_FD 1

struct execsvc_redir {
  int child_fd; /* slot in the CHILD (0=stdin, 1=stdout, 2=stderr, …) */
  int source;   /* EXECSVC_FD_* — how to interpret `ref`               */
  int ref;      /* handle index, in the service's or the client's table */
};

/* Hard ceiling the service enforces before reading a body.  Generous for real
 * command lines, small enough that a hostile client cannot make the service
 * commit meaningful memory. */
#define EXECSVC_BODY_MAX 4096
#define EXECSVC_ARG_MAX  32  /* argv entries (bound, not a fixed cost) */
#define EXECSVC_PATH_MAX 128

struct execsvc_spawn_hdr {
  unsigned int version;  /* EXECSVC_VERSION */
  unsigned int flags;    /* SPAWN_FLAG_* (caps.h) */
  int argc;              /* 1..EXECSVC_ARG_MAX */
  int nredir;            /* 0..SPAWN_MAX_REDIR */
  int want_ctty;         /* non-zero: child gets a controlling terminal */
  unsigned int body_len; /* bytes following this header; <= EXECSVC_BODY_MAX */
};

/* The reply is small and fixed — it carries no user-controlled vector. */
struct execsvc_spawn_rep {
  unsigned int version;
  int pid;       /* > 0 on success, else a negative errno */
  int owner_pid; /* logical owner the service assigned (Q3) */
  int reserved;
  char resolved[EXECSVC_PATH_MAX]; /* the path actually executed */
};

/*
 * execsvc_spawn - client stub: ask the execution service to run argv.
 * Returns the child pid, or < 0 if the service is unreachable — callers MUST
 * fall back to spawning directly, because the service is supervised and can be
 * briefly absent across a respawn.  Implemented in user/sys/lib/execsvc_client.c
 * (its own unit, so Phase 10a/12 can move service clients out of the libc as a
 * build change rather than another extraction).
 */
int execsvc_spawn(int argc, char *const argv[], unsigned int flags);

#endif /* _EXECSVC_H */

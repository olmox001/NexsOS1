#ifndef NXMANAGE_USER_API_H
#define NXMANAGE_USER_API_H
/*
 * NXMANAGE: userland-owned, capability-port supervisor protocol.
 * The kernel owns neither the request language nor the service catalog.
 * The kernel stamps sender identity and maintains the bounded PORT queue.
 */
#include <stdint.h>

#define NXMANAGE_PORT "OS1nx_machine_manage"
#define NXMANAGE_INIT_CONFIG "/system/boot/manager.conf"
#define NXMANAGE_SERVICES_CONFIG "/system/boot/services.conf"
#define NXMANAGE_PROFILE_CONFIG "/system/boot/profile"
#define NXMANAGE_PROTO_VERSION UINT32_C(3)
#define NXMANAGE_REQ_HELLO 0x4E584D00
#define NXMANAGE_REP_HELLO 0x4E584D03
#define NXMANAGE_REQ_REGISTER 0x4E584D01
#define NXMANAGE_REP_REGISTER 0x4E584D02
#define NXMANAGE_REQ_NAME 0x4E584D04
#define NXMANAGE_REP_NAME 0x4E584D05
#define NXMANAGE_REQ_AUTH 0x4E584D06
#define NXMANAGE_REP_AUTH 0x4E584D07
/* Bounded, unique service name; canonical registry key is
 * system.manage.service.<name>, whose value is one atomic generation record.
 * The service name travels as a separate authenticated PORT message so the
 * existing 64-byte control payload never truncates executable paths. */
#define NXMANAGE_NAME_MAX 24U
#define NXMANAGE_MODE_LOCK UINT32_C(1)
#define NXMANAGE_MODE_ONCE UINT32_C(2)
#define NXMANAGE_MODE_ASYNC_ONCE UINT32_C(3)
/* Start once and ACK spawn immediately. Never respawn or terminate on deadline. */
#define NXMANAGE_MODE_START_ONCE UINT32_C(4)

enum {
  NXMANAGE_SLOTS = 16,       /* per-boot configured service slots */
  NXMANAGE_QUEUE_BUDGET = 32,/* underlying fixed-capacity kernel PORT queue */
  NXMANAGE_POLL_MS = 50,     /* bounded userland supervisor work quantum */
  NXMANAGE_ONCE_TIMEOUT_MS = 60000, /* dependency completion maximum */
  NXMANAGE_ASYNC_TIMEOUT_MS = 1800000, /* suite execution, 30 minutes */
  NXMANAGE_ACK_STEPS = 1800, /* 90s, larger than dependency timeout */
  NXMANAGE_BASE_MS = 200,
  NXMANAGE_STABLE_MS = 10000,
  NXMANAGE_CEILING_MS = 30000
};

/* NAME requests carry one NUL-terminated canonical service name.
 * AUTH follows NAME and precedes REGISTER; it carries the service capability
 * mask selected by the protected manifest (CAP_ALL or CAP_ALL plus the
 * optional CAP_WINDOW_MANAGE). Only the authenticated init may set it.
 * The request's payload is a sequence of three terminated strings:
 * absolute executable path, blank-separated bounded arguments (0-2),
 * optional srv.* registry key. Data1=slot, data2=mode.
 * A request is authorized ONLY when the kernel-stamped sender is the
 * manager's currently tracked init child; no public handle grants authority.
 */
#endif

/* Shared userland broker/command protocol; no kernel semantic dependency. */
#ifndef NX_PAX_SERVICE_PROTOCOL_H
#define NX_PAX_SERVICE_PROTOCOL_H
#define NXPAX_PORT "OS1nx_svc_pax"
#define NXPAX_IPC_READ 0x220
#define NXPAX_CLAIM_TRIES 20U
#define NXPAX_MAX_PROBE OS1_NXPAX_MAX_WINDOW_RECORDS
#endif

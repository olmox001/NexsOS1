/*
 * Actual process capability test handshake: the parent grants an attenuated
 * PROCESS WRITE handle, then transfers its CHILD-LOCAL handle slot through
 * a kernel-stamped mailbox message. Handles are never ambient global IDs.
 */
#ifndef NEXS_CAPIPC_TEST_PROTOCOL_H
#define NEXS_CAPIPC_TEST_PROTOCOL_H
#define CAPIPC_MAGIC 0xCAFEU
#define CAPIPC_GRANT_READY 0x43495043
#define CAPIPC_WAIT_STEPS 4000U /* 4 s of bounded 1 ms polling under load */
#endif

# NX-SDK service client extensions

These files are client-side libraries for services owned and started by
NX-VIOLA. The service executable and its lifecycle remain in userland; the
stable client API and wire contract belong to NX-SDK.

- notify_client.c: OS1 notification API and error-reporting policy; resolves
  the boot-started nxntfy_srv endpoint via the registry and sends IPC.
- execsvc_client.c: client protocol for the supervised execution service.

A client returning an unavailable-service error is not evidence that the
service is absent from the system image; startup, discovery, restart and stale
endpoint behavior are covered by the service contract tests. Do not copy
server implementations into this directory.

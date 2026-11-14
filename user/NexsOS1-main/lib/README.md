# nexsos1-lib

NexsOS1-specific shared userland policy belongs here, not in NX-SDK. The SDK exposes portable contracts, shared keyboard layouts, and USERKIT protocols; this project supplies the concrete NexsOS1 look/icon/execution policy.

The runtime has no `dlopen`. This directory contains only NexsOS1-main project policy; shared keyboard layout support is provided by NX-SDK and selected at runtime through the OS registry.

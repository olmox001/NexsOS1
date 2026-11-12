# nexsos1-lib

NexsOS1-specific shared userland policy belongs here, not in NX-SDK. The SDK exposes portable contracts and USERKIT protocols; this project supplies the concrete NexsOS1 look/icon/execution policy and keyboard-layout provider.

The runtime has no `dlopen`. Global library availability is declared in `/system/boot/libraries.conf`, published by init under `system.manage.library.*`, and matching static archives are installed under `/sys/lib`.

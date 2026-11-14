# NexsOS demo1

`demo1` is a deliberately small, independent NexsOS userland project. It builds one interactive program, `demo1shell`, and selects NX-USERKIT services through its own immutable boot manifest.

It does not import source code from `NexsOS1-main`. The image builder supplies NX-USERKIT service binaries and the NX-CORE `nxmanageDD` extension as independent artifacts. The shell can inspect the manager's project identity and all authoritative service-generation records at runtime.

The current demo selects `/sbin/nxdemo1init`, replaces the standard notification backend with `nxnotify_registry`, and adds `nxexample_echo`. Interactive commands `init`, `services`, `echo-test`, and `notify-test` prove the selected init and service implementations at runtime rather than only checking image contents.

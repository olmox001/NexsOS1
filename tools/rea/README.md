# REA project wrapper

The REA source is pinned in `external/tools/rea`; the published CLI package is installed globally in the fnm-managed Node.js 24.22.1 environment.

Use the wrapper from any shell without changing the current/default Node version:

```sh
./tools/rea/run.sh --version
./tools/rea/run.sh doctor --json
./tools/rea/run.sh inspect-macho build/darwin-x86_64/tests/test-9p-frame --format json
```

To reproduce the installation on another development host:

```sh
fnm install 24.22.1
fnm exec --using=24.22.1 npm install --global rea-agents@6.8.0
```

The host is provisioned with Ghidra 12.1.4 (including its native macOS x86_64 decompiler), Hopper, Java 21, and the REA-bundled Frida Node binding. `run.sh` discovers Ghidra, Java, and Hopper without changing the user's default Node or Java selection. REA setup has been applied to the detected client registrations so their MCP server environments receive the analysis-provider paths.

Client registrations are managed through explicit setup operations; inspect ./tools/rea/run.sh setup --help before applying a configuration change.

For repeatable installation on another host, pin the same REA release and use the exact provider versions documented in `docs/primary/TOOLS_REA.md`; do not use an unpinned `npm install -g` or change the user's default Node version.

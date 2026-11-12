#!/usr/bin/env python3
"""Fail-closed verification of the actual VFS boot manifest and ELF sources.

The policy belongs to the userland manifest; the authority daemon remains
service-agnostic. This test is deliberately independent of that daemon's code.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT / 'user/sys/boot'
MAX_SLOTS = 16
MAX_PAYLOAD = 64


def fail(reason: str) -> int:
    print(f'FAIL NXMANAGE manifest: {reason}')
    return 1


def main() -> int:
    entry = (BOOT / 'entry').read_text().strip()
    manager = (BOOT / 'manager.conf').read_text().strip()
    if entry != '/krnl/extension/nxmanageDD' or manager != 'init=/sbin/nxinit':
        return fail('entry/manager bootstrap contract changed without audit')
    if not (ROOT / 'kernel/user/extension/nxmanageDD/main.c').exists():
        return fail('actual kernel-owned user extension source is missing')
    if not (ROOT / 'user/sbin/nxinit.c').exists():
        return fail('init source is missing')
    records = []
    seen = set()
    seen_names = set()
    for number, raw in enumerate((BOOT / 'services.conf').read_text().splitlines(), 1):
        if not raw or raw.startswith('#'):
            continue
        fields = raw.split('|')
        if len(fields) not in (5, 6):
            return fail(f'line {number}: expected five or six fields')
        name, mode, path, arguments, key = fields[:5]
        authority = fields[5] if len(fields) == 6 else ''
        if authority not in ('', 'window-manage'):
            return fail(f'line {number}: unknown authority')
        if authority and arguments:
            return fail(f'line {number}: argv-aware capability spawn unavailable')
        if authority and name != 'pointer':
            return fail(f'line {number}: unexpected privileged service')
        if not re.fullmatch(r'[a-z][a-z0-9_-]{0,22}', name):
            return fail(f'line {number}: invalid canonical service name')
        if name in seen_names:
            return fail(f'line {number}: duplicate service name')
        seen_names.add(name)
        if mode not in {'lock', 'once', 'start', 'debug'}:
            return fail(f'line {number}: unrecognized lifecycle mode')
        if not re.fullmatch(r'/sys/(?:bin|services)/[A-Za-z0-9_-]+', path):
            return fail(f'line {number}: noncanonical service path')
        if key and not re.fullmatch(r'srv\.[A-Za-z0-9_.-]+', key):
            return fail(f'line {number}: registry key outside allowed namespace')
        if len(arguments.split()) > 2 or len(path) + len(arguments) + len(key) + 3 > MAX_PAYLOAD:
            return fail(f'line {number}: message exceeds existing PORT payload')
        source = ROOT / ('user/sys/services' if path.startswith('/sys/services/') else 'user/sys/bin') / (Path(path).name + '.c')
        if not source.is_file():
            return fail(f'line {number}: executable has no actual compiled C source: {source}')
        identity = (path, arguments)
        if identity in seen:
            return fail(f'line {number}: duplicate executable/args registration')
        seen.add(identity)
        records.append((name, mode, path, arguments, key))
        if name == 'pointer' and authority != 'window-manage':
            return fail('pointer service must explicitly receive window-manage at boot')
    if len(records) == 0 or len(records) > MAX_SLOTS:
        return fail(f'invalid slot count {len(records)}')
    if ('shell', 'start', '/sys/services/nxttyd', '/sys/bin/nxshell', '') not in records:
        return fail('interactive shell must start once without deadline or respawn')
    if ('environment', 'once', '/sys/bin/nxenvinit', '', '') not in records:
        return fail('environment initializer must complete before dependents')
    if ('tests', 'debug', '/sys/bin/nxshell', '-c /sys/bin/nxtestrunner', '') not in records:
        return fail('headless mdebug suite is not declared as a separate one-shot')
    if ('roleprobe', 'debug', '/sys/bin/nxroleprobe', '', '') not in records:
        return fail('ROOT-capability regression program missing from mdebug')
    if records.index(('environment', 'once', '/sys/bin/nxenvinit', '', '')) > records.index(('shell', 'start', '/sys/services/nxttyd', '/sys/bin/nxshell', '')):
        return fail('environment initialization must precede the shell')
    core = (ROOT / 'kernel/main.c').read_text()
    for forbidden in ('/sbin/nxinit', 'nxmanageDD', 'nxtestrunner'):
        if forbidden in core:
            return fail(f'kernel composition root still knows userland name: {forbidden}')
    print(f'PASS: {len(records)} unique registry service names, real compiled ELF sources, lifecycle, VFS boot and queue bounds')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

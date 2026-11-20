# Threat model and required security tests

Attackers: compromised unprivileged app, compromised ROOT service, forged manager registration, reused PID/handle, malicious or stolen USB pairing, spoofed popup, substituted program, path-symlink race, off-line stolen credential file, online password guessing, service death/restart race and resource exhaustion.

Trust invariants: process principal, executable identity, service instance/generation, user/group membership and caps must be stamped by the trusted mechanism; no caller may claim its own role. Root is not MACHINE. Service manager accepts authenticated registration from PID1 over the existing port, and grants the minimum per-service operations. Each image declares an explicit capability mask through the owned spawn contract.

Credentials must not be written into registry-readable strings, source, hardcoded image manifests, UART, NXPaX logs, USB packets without transport protection or host command arguments. Require healthy entropy, audited CSPRNG, memory-hard password verifier, constant-time comparison, bounded retries and secure zeroization. Fail closed if cryptographic prerequisites are missing.

Persistent permission decision key includes principal, executable identity/digest, resource type, resource scope, operation and policy version; allow once expires after use, permanent allow is revocable. Process-modal popup must block only the requester; system popup is topmost and nonblocking. Terminal fallback accepts only server-supplied choices, with no-echo password input and deadline.

Required negative tests: wrong/absent credentials, cross-user inode access, false UID, stale token, service duplicate/spoof/restart, cap amplification, unauthorized kill, unsigned or replayed USB request, pop-up impersonation, permission revocation, symlink replacement, TTY focus races, power without authority, boot entropy failure. No secure auth service is claimed implemented by writing this plan.

# Session 3 — outstanding issues (2026-11-19)

| ID | Open issue | Acceptance criteria |
| --- | --- | --- |
| S3-01 | GNU Coreutils `nice` cannot complete: libc `execvp` is an `ENOTSUP` stub; priority APIs are not real | Userland object-based exec and scheduling semantics, real command acceptance on both architectures |
| S3-02 | GNU `nohup` cannot open `/dev/null`; `dup` and `dup2` return invented descriptor numbers | Real descriptor/device object contracts, redirection and execution tests |
| S3-03 | Terminal text pixel quality/reflow degrades at zoom 300% | Inspect framebuffer/font, resize and text wrap on both architectures |
| S3-04 | `nxadb shell` started while boot-test TTY owns focus may time out or capture suite output | Fix focus/controller handoff in `nxttyd` and test concurrent shells |
| S3-05 | Global Power12 qualification fails (3 policy/386 ID-limit/5171 AST findings) | Correct source and run strict verification without exclusions |
| S3-06 | Transitional kernel window compatibility and emergency display consumers remain | Migrate on actual code/test evidence while preserving HAL/NXPAX fault output |
| S3-07 | Earlier timerprobe sporadic failures | Soak both arches and analyze CPU timer queues without falsifying tests |
| S3-08 | Fully clean all-repo build and final distribution qualification still required | Preserve work and run clean official setup/build, real QEMU and nxadb |

Never add syscalls, invent POSIX success, suppress faults, duplicate the
compositor or move NexsOS1-specific `nxbar`/`nxui` into USERKIT.

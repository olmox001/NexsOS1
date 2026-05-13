# REFACTOR PLAN - OS1

## Micro-Fase 1: Filesystem Integrity & GPT Robustness
- **Obiettivo**: Fix corruzione Ext4 Group Descriptor e CRC32 in GPT.
- **File**: `ext4.c`, `gpt.c`.

## Micro-Fase 2: Memory Management Fixes (PMM & VMM)
- **Obiettivo**: Fix bitmap PMM e puntatori virtuali VMM.
- **File**: `pmm.c`, `vmm.c`.

## Micro-Fase 3: HAL Abstraction & Cleanup
- **Obiettivo**: Rimoziore codice arch-specific dal core.
- **File**: `vmm.c`, `pmm.c`.

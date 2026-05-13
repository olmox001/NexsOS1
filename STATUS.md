# OS1 Project Status

## Current State
- **Architecture Support**: Dual-arch (aarch64 / amd64) stable boot.
- **SMP**: Stabilized on both architectures.
- **Boot**: Hybrid ISO (MBR/GPT) stable.
- **Filesystem**: Ext4 Read-Only mounted correctly.
- **User-space**: Shell operational on both architectures.

## Bug Tracker
- [x] **ELR=0 (Instruction Abort)**: Jump to NULL in user-space transition. (Resolved)
- [ ] **Technical Debt**: Assembly/C ratio optimization.
- [ ] **HAL Unification**: Complete abstraction for all MMIO devices.

## Recent Milestones
- **2026-05-13**: AMD64 SMP stabilization and Hybrid ISO support finalized.
- **2026-05-13**: ELR=0 bug confirmed resolved by user.

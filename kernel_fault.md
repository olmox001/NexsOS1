> **RESOLVED — TIMER-UAF-01.** Root cause + certified fix:
> `docs/report/TIMER-UAF-01-CERTIFIED-FIX.md`.
> Double `list_del` of a `sleep_timer` node in the per-CPU timer wheel, caused by
> `sys_yield`'s anti-spin throttle re-`INIT_LIST_HEAD`-ing a still-linked node
> after an IPC force-wake. Fixed in `kernel/core/timer.c` (timer_add never
> double-links) + `kernel/core/syscall_dispatch.c` (yield re-arm guarded by
> `timer_pending`). Raw trace below kept as evidence.

emu-system-aarch64 -M virt -cpu cortex-a57 -m 5G -smp 4 -serial mon:stdio -display default,show-cursor=on -device virtio-gpu-device -device virtio-keyboard-device -device virtio-mouse-device -drive if=none,file=build/aarch64/disk.img,id=hd0,format=raw -device virtio-blk-device,drive=hd0 -dtb build/aarch64/virt.dtb -kernel build/aarch64/kernel.bin
[C0] [INFO] Console driver initialized
FDT: Probing... 
FDT: Successfully initialized
[C0] [INFO] Kernel: Entry x0 = 0x48000000
[C0] 
[C0] ========================================
[C0]   AArch64 HybridKernel HybridKernel MicroKernel ispired ispired v0.1.0
[C0]   Production-Ready HybridKernel MicroKernel ispired
[C0] ========================================
[C0] 
[C0] [INFO] Initializing CPU...
[C0] [INFO] CPU: Primary core 0 initialized
[C0] [INFO] CPU: Vector Table set to 0xffff000040080800
PLATFORM: arch_platform_early_init starting
[C0] [INFO] IRQ: Registered chip ARM GICv2
FDT: Probing... 
FDT: Successfully initialized
[C0] [INFO] AArch64: IDTF/FDT initialized from 0x48000000
[C0] [INFO] Initializing IRQ...
[C0] [INFO] IRQ: Registered chip ARM GICv2
[C0] [INFO] IRQ driver initialized via HAL
[C0] [INFO] GIC: 256 interrupt lines
[C0] [INFO] GIC: Distributor initialized
[C0] [INFO] Initializing timer...
[C0] [INFO] Timer: Frequency 62500000 Hz
[C0] [INFO] Timer: System tick rate 100 Hz
[C0] [INFO] Timer driver initialized via HAL
[C0] [INFO] Timer: Per-CPU virtual timer enabled (IRQ 27). Next: 0xc93026, Ctl: 0x1
[C0] [INFO] Initializing memory...
[C0] [INFO] AArch64: IDTF RAM Region [0]: 0x40000000 - 0x180000000
[C0] [INFO] PMM: Metadata initialized at 0x40a07000 (41120 KB)
[C0] [INFO] PMM: Usable RAM: 5120 MB (address span 5120 MB)
[C0] [INFO] PMM: 5120 MB usable, 5070 MB free (span 5120 MB; gaps reserved)
[C0] [INFO] PMM: DMA zone: 128 pages, Normal zone: 1297873 pages
[C0] [INFO] VMM: Initializing MMU (Phase 1: Bootstrap)...
[C0] [INFO] AArch64 VMM: Installing kernel PGD 0x4322f000 in TTBR1
[C0] [INFO] AArch64 VMM: Kernel PGD active (TTBR1).
[C0] [INFO] VMM: Bootstrap complete.
[C0] [INFO] VMM: Performing RAM-aware dynamic remapping...
[C0] [INFO] VMM: Mapping region 0x40000000 - 0x180000000
[C0] [INFO] VMM: Dynamic remapping successful. All discovered RAM is now accessible.
[C0] 
[KTEST] Starting Kernel Unit Tests (5 cases found)...
[C0] [KTEST] Running: test_vmm_protect... [C0] PASS
[C0] [KTEST] Running: test_kmalloc_growth... [C0] [INFO] kmalloc: Initialized bucket allocator. Chunk: 4 MB at 0xffff000043240000 (growable)
[C0] [INFO] kmalloc: heap grown to 8 MB (new chunk at 0xffff000043640000)
[C0] PASS
[C0] [KTEST] Running: test_math_basic... [C0] PASS
[C0] [KTEST] Running: test_string_compare... [C0] PASS
[C0] [KTEST] Running: test_string_length... [C0] PASS
[C0] [KTEST] Completed. Summary: 5 PASSED, 0 FAILED

[C0] [INFO] HAL: Initializing Bus Manager...
[C0] [INFO] HAL: Scanning Platform Bus (MMIO)...
[C0] [INFO] HAL: Registered device 'VirtIO-2' (Bus=2, ID=1af4:0002, Class=00:00:00, Base=0xa003800, IRQ=76)
[C0] [INFO] HAL: Registered device 'VirtIO-18' (Bus=2, ID=1af4:0012, Class=00:00:00, Base=0xa003a00, IRQ=77)
[C0] [INFO] HAL: Registered device 'VirtIO-18' (Bus=2, ID=1af4:0012, Class=00:00:00, Base=0xa003c00, IRQ=78)
[C0] [INFO] HAL: Registered device 'VirtIO-16' (Bus=2, ID=1af4:0010, Class=00:00:00, Base=0xa003e00, IRQ=79)
[C0] [INFO] PCI: scanning config space (ECAM/port)...
[C0] [INFO] HAL: Registered device 'PCI-1b36:0008' (Bus=1, ID=1b36:0008, Class=06:00:00, Base=0x0, IRQ=0)
[C0] [INFO] HAL: Registered device 'VirtIO-4096' (Bus=1, ID=1af4:1000, Class=02:00:00, Base=0x0, IRQ=0)
[C0] [INFO] VirtIO: Probing for block device...
[C0] [INFO] VirtIO: Found Block Device (IRQ 76)
[C0] [INFO] VirtIO: Version 1
[C0] [INFO] VirtIO: Block Device Initialized successfully
[C0] [INFO] block: active backend 'virtio-blk'
[C0] [INFO] VirtIO-GPU: Probing...
[C0] [INFO] VirtIO-GPU: Found device (IRQ 79)
[C0] [INFO] GPU: Primary device set to VirtIO-GPU
[C0] [INFO] GPU: Registered VirtIO-GPU
[C0] [INFO] VirtIO-GPU: Done.
[C0] [INFO] Graphics: Initialized via HAL (720x1280)
[C0] [INFO] Partition: Initializing...
[C0] [INFO] GPT: Valid signature found. Entries: 128 @ LBA 2
[C0] [INFO] GPT: Partition 0: Start=34, Size=196541 sectors
[C0] [INFO] GPT: Found 1 partitions
[C0] [INFO] GPT: Done.
[C0] [INFO] BufferCache: Initializing...
[C0] [INFO] Buffer: Done.
[C0] [INFO] Ext4: Mounted. Vol=, Inodes=1024, features incompat=0x42
[C0] [INFO] VFS: mounted ext4 on partition 0 as /
[C0] [INFO] VFS: Done.
[C0] [INFO] Input: Initializing input subsystem...
[C0] [INFO] VirtIO-Input: Probing devices...
[C0] [INFO] VirtIO-Input: 2 input device(s) initialized
[C0] [INFO] USB: Initializing host controllers...
[C0] [INFO] Keyboard: Initialized (Layout: it)
[C0] [INFO] Registry: Initialized with 3 default keys.
[C0] [INFO] Registry: Initialized.
[C0] [INFO] Initializing processes...
[C0] [INFO] Process: Initializing scheduler subsystem...
[C0] [INFO] Process: limit 128 (pool 128, 8 reserved for SYSTEM/ROOT, 32 children max per user process)
[C0] [INFO] Initializing scheduler...
[C0] [INFO] Scheduler: Initializing...
[C0] [INFO] Compositor: Initialized (720x1280 profile=desktop titlebar=40px corner=14px)
[C0] [INFO] Scheduler: Spawning First-Stage Init...
[C0] [INFO] Process: Creating 'init' (Prio=2)
[C0] [INFO] process_create: 'init' PID=1 slot=0 Prio=2 PageTable=0xffff000044160000
[C0] [INFO] process_create: PID 1 context allocated at 0xffff000044180cd0 (kstack=ffff000044181000)
[C0] [INFO] ELF: Mapping Segment at 0x80000000 (FileSz: 0x117f8, MemSz: 0x117f8)
[C0] [INFO] ELF: Mapping Segment at 0x800217f8 (FileSz: 0x200, MemSz: 0x188c0)
[C0] [INFO] Scheduler: Initialized PID 1 (/sys/bin/init)
[C0] [INFO] Process: Creating 'idle' (Prio=31)
[C0] [INFO] process_create: 'idle' PID=2 slot=1 Prio=31 PageTable=0xffff0000442b3000
[C0] [INFO] process_create: PID 2 context allocated at 0xffff0000442d3cd0 (kstack=ffff0000442d4000)
[C0] [INFO] VMM: PGD 0xffff0000442b3000 destroyed: freed 0 user frames, 1 table pages (free now 1293743)
[C0] [INFO] Waking secondary CPUs...
[C0] [INFO] AArch64: Starting SMP initialization for 4 potential cores
[C0] [INFO] Process: Creating 'idle' (Prio=31)
[C1] [INFO] CPU: Vector Table set to 0xffff000040080800
[C0] [INFO] process_create: 'idle' PID=3 slot=2 Prio=31 PageTable=0xffff0000442d5000
[C1] [INFO] Timer: Per-CPU virtual timer enabled (IRQ 27). Next: 0x2197fda, Ctl: 0x1
[C1] [INFO] Secondary CPU 1 online and ready
[C0] [INFO] process_create: PID 3 context allocated at 0xffff0000442f5cd0 (kstack=ffff0000442f6000)
[C0] [INFO] VMM: PGD 0xffff0000442d5000 destroyed: freed 0 user frames, 1 table pages (free now 1293710)
[C0] [INFO] AArch64: CPU 1 online
[C0] [INFO] Process: Creating 'idle' (Prio=31)
[C2] [INFO] CPU: Vector Table set to 0xffff000040080800
[C0] [INFO] process_create: 'idle' PID=4 slot=3 Prio=31 PageTable=0xffff0000442f7000
[C2] [INFO] Timer: Per-CPU virtual timer enabled (IRQ 27). Next: 0x21c4515, Ctl: 0x1
[C2] [INFO] Secondary CPU 2 online and ready
[C0] [INFO] process_create: PID 4 context allocated at 0xffff000044317cd0 (kstack=ffff000044318000)
[C0] [INFO] VMM: PGD 0xffff0000442f7000 destroyed: freed 0 user frames, 1 table pages (free now 1293677)
[C0] [INFO] AArch64: CPU 2 online
[C0] [INFO] Process: Creating 'idle' (Prio=31)
[C3] [INFO] CPU: Vector Table set to 0xffff000040080800
[C0] [INFO] process_create: 'idle' PID=5 slot=4 Prio=31 PageTable=0xffff000044319000
[C3] [INFO] Timer: Per-CPU virtual timer enabled (IRQ 27). Next: 0x21e8a61, Ctl: 0x1
[C3] [INFO] Secondary CPU 3 online and ready
[C0] [INFO] process_create: PID 5 context allocated at 0xffff000044339cd0 (kstack=ffff00004433a000)
[C0] [INFO] VMM: PGD 0xffff000044319000 destroyed: freed 0 user frames, 1 table pages (free now 1293644)
[C0] [INFO] AArch64: CPU 3 online
[C0] [INFO] Enabling interrupts...
[Init] System Initialization Starting...
[Init] Spawning Notification Server...
[C1] [INFO] Process: Creating '/sys/bin/notify_srv' (Prio=2)
[C1] [INFO] process_create: '/sys/bin/notify_srv' PID=6 slot=5 Prio=2 PageTable=0xffff00004433b000
[C1] [INFO] process_create: PID 6 context allocated at 0xffff00004435bcd0 (kstack=ffff00004435c000)
 [System Notification]ng Segment at 0x80000000 (FileSz: 0x116a0, MemSz: 0x116a0)
 [=C0] [INFO] Process: Creating '�(Window 'Shell PID 7')0x200, MemSz: 0x188c0)
=/bin/t=op' (=Prio=2)=============ts.w 'DoomGeneric OS1' (640x400) at (50,50)PID 8')
Using ./.savegame/ for savegames, id=103, my_pid=9
PID  NAME         STATE    PRIO CPU
------------------------------------
~
~
~
~
~
~
~
~
~            Kilo editor -- verison 0.0.1-os1
 [System Notification]E    PRIO CPU
 �----------------------------------
Data abort at 0xffff000040086ae0, FAR=0x0000000000000008
--- Kernel Exception Context Dump ---
Process: PID 3    READY    31   1  
SPSR_EL1: 0x00000000800003c51   2  
ELR_EL1:  0xffff000040086ae01   3  
FAR_EL1:  0x0000000000000008    0  
ESR_EL1:  0x0000000096000044    1  
EC: 0x25, ISS: 0x44LEEPING 2    3  
Frame: 0xffff0000408b40d0 (per-CPU fault stack), faulting SP: 0xffff0000442f5c30
X00: 0x0000000000000000  X01: 0xffff00004415f0d8
X02: 0x0000000000000000  X03: 0xffff0000400850a0
X04: 0xffff0000409b9860  X05: 0x0000000000000004
X06: 0x0000000000000006  X07: 0x0000000000000004
X08: 0xffff00004415f090  X09: 0x0000000000000128
X10: 0x0000000000000006  X11: 0xffff0000409b9aac
X12: 0x00000000000003c0  X13: 0x0000000000000000
X14: 0x0000000000000000  X15: 0x0000000000000000
X16: 0x0000000000000000  X17: 0x0000000000000000
X18: 0x0000000000000000  X19: 0xffff0000409baae0
X20: 0x0000000000000000  X21: 0xffff0000409bad48
X22: 0xffff0000442f5cd0  X23: 0x00000000000003c0
X24: 0x00000000ffffffff  X25: 0xffff0000409bad38
X26: 0xffff0000409ad000  X27: 0x0000000000000000
X28: 0x0000000000000000  X29: 0xffff0000442f5c30
X30: 0xffff000040086b00
SP_EL0:  0x0000000000000000
-----------------------------
Backtrace (fp chain, max 24):
  #0  ffff000040086ae0 kernel_timer_tick+0x130
  #1  ffff00004009d160 irq_handler+0x130
  #2  ffff00004008163c handle_el1_spx_irq+0xc4


*** KERNEL PANIC (fault context) ***
Unrecoverable kernel exception
Backtrace (fp chain, max 24):
  #0  ffff0000400933d0 panic+0xd0
  #1  ffff0000400933d0 panic+0xd0

System halted.
shsh@MacBook-Pro-di-shsh NexsOS1 % 
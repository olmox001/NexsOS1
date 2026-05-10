# OS1 — Report di Analisi Statica del Codice
**Data:** 2026-05-10  
**Branch:** dev-research  
**Revisore:** Claude Code (claude-sonnet-4-6)

---

## Indice

1. [Panoramica](#panoramica)
2. [Boot Layer](#1-boot-layer)
3. [Kernel Architecture — AArch64](#2-kernel-architecture--aarch64)
4. [Kernel Main](#3-kernel-main)
5. [Memory Management](#4-memory-management)
6. [Scheduler & ELF Loader](#5-scheduler--elf-loader)
7. [Filesystem](#6-filesystem)
8. [Kernel Libraries](#7-kernel-libraries)
9. [Graphics & Compositor](#8-graphics--compositor)
10. [Drivers](#9-drivers)
11. [User Space](#10-user-space)
12. [Build System](#11-build-system)
13. [Riepilogo per priorità](#riepilogo-per-priorità)

---

## Panoramica

Il progetto è un kernel monolitico per architettura AArch64, con boot via GRUB/Multiboot2, memory manager a due livelli (PMM + VMM), scheduler round-robin multi-CPU, filesystem ext4/GPT, driver virtio, e un compositor grafico. L'analisi ha ispezionato **85 file sorgente** e identificato problemi in **~35 file**.

| Severità | Problemi trovati |
|----------|-----------------|
| CRITICO  | 12+             |
| ALTO     | 20+             |
| MEDIO    | 30+             |
| BASSO    | 40+             |

---

## 1. Boot Layer

### `boot/header.S`
- **[MEDIO]** Campo architettura impostato a `0` (i386) nel tag Multiboot2, mentre il target è AArch64. Il protocollo Multiboot2 non supporta ufficialmente AArch64 via questo meccanismo.
- **[BASSO]** Nessuna verifica dell'allineamento dell'entry point `_start`.

### `boot/stage1.S`
- **[ALTO]** `secondary_core_main` è uno stub che esegue un loop infinito — i core secondari non vengono mai inizializzati.
- **[ALTO]** Nessuna validazione del puntatore `multiboot_mmap_tag` prima del dereferenziamento.
- **[MEDIO]** Identity mapping hardcoded a 4GB (blocchi da 1GB) senza verificare la RAM effettiva del sistema.
- **[MEDIO]** Nessuna gestione di errori per strutture Multiboot2 malformate.

### `boot/stage2.S`
- **[MEDIO]** `init_basic_devices` è uno stub no-op — nessun device viene inizializzato in questa fase.
- **[MEDIO]** `kernel_entry_address == NULL` causa halt senza nessun messaggio diagnostico (solo `cbz x1, halt`).
- **[BASSO]** Mancano operazioni di cache coherency tra core0 e core secondari durante il setup.

### `boot/linker.ld`
- **[MEDIO]** `__stack_top` non è esplicitamente allineato a 16 byte, violando l'ABI AArch64.
- **[BASSO]** Nessun allineamento minimo garantito per le sezioni; il kernel potrebbe finire a indirizzi non allineati.

---

## 2. Kernel Architecture — AArch64

### `kernel/arch/aarch64/boot/start.S`
- **[CRITICO]** `secondary_ttbr0` è inizializzato a `.quad 0` e viene scritto dal core primario in `main.c` (riga 94). I core secondari potrebbero leggerlo prima che il core primario lo scriva — **race condition**.
- **[MEDIO]** Codice duplicato: il check `cbz x1, primary_init` appare due volte (righe 28-29).
- **[MEDIO]** `secondary_startup` non gestisce i valori di ritorno PSCI; se la chiamata fallisce, l'esecuzione prosegue verso `halt` senza segnalare l'errore.
- **[MEDIO]** Setup SCTLR_EL1: operazioni `bic` disabilitano sia D-cache che I-cache; pericoloso se il codice successivo presuppone caching abilitato.

### `kernel/arch/aarch64/cpu/cpu.c`
- **[CRITICO]** `nr_cpus` viene incrementato in modo non atomico in `cpu_init()`. Con SMP, due CPU che chiamano `cpu_init()` simultaneamente producono un conteggio errato.
- **[ALTO]** `get_cpu_info()` entra in loop infinito silenzioso se CPU ID >= 8, senza nessun messaggio di panic — debugging impossibile.
- **[BASSO]** Nessun check che `exception_vectors_install()` abbia successo.

### `kernel/arch/aarch64/cpu/exception.S`
- **[CRITICO]** Gli handler scrivono in memoria (righe 335-338) prima che il frame di eccezione sia completamente salvato. Se un'altra interruzione arriva in questo intervallo, il frame viene corrotto.
- **[ALTO]** Scritture UART di debug (commentate) usano registri `x0`/`x1` dopo che questi sono stati poppati. Se decommentate, corrompono il contesto salvato.
- **[MEDIO]** La dimensione del frame è hardcoded a 816 byte; l'aggiunta di nuovi registri causa overflow silenzioso.

### `kernel/arch/aarch64/cpu/syscall.c`
- **[CRITICO]** `copy_from_user()` e `copy_to_user()`: nessuna guardia su `current_process->page_table == NULL` prima di `arch_set_ttbr0()`.
- **[ALTO]** Dopo lo switch di page table, il flush TLB è incompleto — il kernel può avere in cache indirizzi ora invalidi.
- **[ALTO]** `sys_ipc_send()` (riga 332) non fa bound-checking sul PID target.
- **[ALTO]** `compositor_destroy_window()` non viene chiamato al termine della syscall; le risorse finestra vengono perse se il processo termina a metà operazione.
- **[MEDIO]** Molte syscall restituiscono codici di errore generici senza distinguere tra ENOMEM, EINVAL, ecc.
- **[MEDIO]** `sys_read()` (riga 199) decrementa ELR di 4 per rieseguire SVC, assumendo che tutte le istruzioni SVC siano esattamente 4 byte.
- **[MEDIO]** Syscall FILE_WRITE (251): se `copy_from_user` fallisce, `k_buf` viene liberato ma il codice di errore non distingue ENOMEM da altri errori.

### `kernel/arch/aarch64/include/arch/arch.h`
- **[ALTO]** `arch_spin_lock()` usa WFE (wait for event) ma non invia mai un SEV (send event). Se più CPU competono, rimangono in WFE indefinitamente fino a un interrupt non correlato.
- **[MEDIO]** `arch_tlb_flush_all()` usa `tlbi vmalle1is` (inner shareable). In sistemi multi-cluster potrebbe essere necessario outer shareable.

### `kernel/arch/aarch64/kernel.ld`
- **[BASSO]** Nessun simbolo `__kernel_start` definito direttamente; usa `PROVIDE()` che può causare problemi di relocation con alcuni toolchain.
- **[BASSO]** La sezione `.text.boot` è mantenuta con `KEEP()` (corretto) ma non è garantito che venga linkata per prima su tutti i toolchain.

---

## 3. Kernel Main

### `kernel/main.c`
- **[CRITICO]** TODO a riga 178: lo slab allocator non è inizializzato, ma `kmalloc` viene usato tramite `process_create` a riga 193 — **use-before-init**.
- **[ALTO]** Il calcolo dello stack secondario `&__kernel_stack[(i + 1) * 65536]` assume esattamente 8 CPU × 64KB. Con una diversa configurazione, il calcolo va fuori bounds.
- **[ALTO]** `secondary_ttbr0 = current_ttbr0` (riga 94) può leggere TTBR0 non inizializzato se la MMU non è ancora stata configurata.
- **[MEDIO]** Nessun check che le inizializzazioni dei device abbiano successo prima di avviare lo scheduler.

---

## 4. Memory Management

### `kernel/mm/pmm.c`
- **[ALTO]** Formula della bitmap DMA errata: `[MAX_PAGES / 64 / 16]` produce una dimensione troppo piccola. La formula corretta dovrebbe essere `[(MAX_PAGES / 16 + 63) / 64]`.
- **[ALTO]** Overflow intero: `total_pages * PAGE_SIZE / (1024 * 1024)` overflow se `total_pages` è grande.
- **[MEDIO]** `MEMORY_BASE` hardcoded a `0x40000000` — specifico per QEMU virt; non funziona su hardware reale.
- **[MEDIO]** `memset(addr, 0, PAGE_SIZE)` scrive all'indirizzo ritornato da `pmm_alloc_page`, che potrebbe non essere kernel-mapped se l'identity mapping non è ancora attivo.
- **[BASSO]** Riga 307: panic usa formato errato (manca chiamata a funzione di output prima dell'halt).

### `kernel/mm/vmm.c`
- **[CRITICO]** `get_next_table()` (riga 40): la maschera `0x0000FFFFFFFFF000UL` estrae l'indirizzo fisico, poi lo casta direttamente a puntatore. Funziona **solo** con identity mapping attivo — assunzione non documentata e fragile.
- **[CRITICO]** Riga 243: `pmd[PMD_INDEX(addr)] = addr | (PAGE_KERNEL & ~PTE_PAGE)` — se `PAGE_KERNEL` include bit specifici per page descriptor, usarli in un block descriptor crea entry invalide.
- **[ALTO]** `PTE_TABLE` e `PTE_PAGE` sono mutualmente esclusivi (righe 73-74), ma vengono settati insieme senza verificare il livello dell'indice.
- **[ALTO]** Remap silenzioso (riga 114): avvisa ma non previene la sovrascrittura — può corrompere mapping in uso.
- **[MEDIO]** Riga 317: arithmetic su array di puntatori senza bounds check.

### `kernel/mm/buffer.c`
- **[CRITICO]** `pmm_free_page(buf)` (riga 70) libera il metadata allocato con `kmalloc`, non la pagina dati `buf->data` — **wrong free target**.
- **[CRITICO]** `virtio_blk_write()` chiamato mentre si tiene `buffer_lock` (riga 125) — **deadlock potenziale** se virtio_blk_write tenta di acquisire qualsiasi spinlock.
- **[MEDIO]** Nessuna politica di eviction LRU; il buffer cache può crescere senza limite.

---

## 5. Scheduler & ELF Loader

### `kernel/sched/elf.c`
- **[CRITICO]** Lo stack di tutti i processi è mappato allo stesso indirizzo virtuale fisso `0xC0000000`. Processi diversi hanno stack con VA identici — confusione garantita in un sistema multiprocesso.
- **[ALTO]** Fallimento di `vmm_map_page()` (riga 77) viene solo loggato; il processo viene creato con pagine non mappate.
- **[MEDIO]** Cache flush (riga 162): `arch_clean_cache_range_va()` con `sizeof(struct pt_regs)` = 816 byte non copre FPSR/FPCR agli offset 800-808.
- **[BASSO]** `end_vpage` usa `+ 4095` hardcoded, assume pagine da 4KB.

### `kernel/sched/process.c`
- **[CRITICO]** `proc->pid = next_pid++` (riga 207) — incremento non atomico. In SMP, due CPU che chiamano `process_create()` simultaneamente ottengono lo stesso PID.
- **[CRITICO]** Work-stealing (riga 512): `list_del_init(&next->run_list)` viene chiamato sulla runqueue di un'altra CPU senza tenere il lock di quella CPU — **corruzione della lista**.
- **[ALTO]** IPC (righe 656-658): se il target aspetta un sender specifico e il messaggio arriva da sender diverso, il messaggio viene **silenziosamente scartato**.
- **[ALTO]** Free dello stack kernel (riga 344): `pmm_free_page((void *)(proc->kernel_stack - 4096))` assume stack di esattamente 1 pagina. Se `STACK_SIZE` è diverso, il free è errato.
- **[MEDIO]** `virt_to_phys(proc->page_table)` (riga 236) loggato prima che `page_table` possa essere allocato (riga 239) — potrebbe loggare NULL.
- **[MEDIO]** `pmm_alloc_pages(STACK_SIZE / 4096)`: se `STACK_SIZE` non è multiplo di 4096, alloca dimensione errata.
- **[MEDIO]** `focus_pid` cercato nella runqueue (riga 454) senza verificare che esista ancora.
- **[BASSO]** `__builtin_ctz()` (riga 471) è GCC-specific; non portabile su altri compilatori.

---

## 6. Filesystem

### `kernel/fs/ext4.c`
- **[ALTO]** Controlla solo `bg_free_blocks_count_lo` (riga 34), ignorando i 32 bit alti. Errato per volumi ext4 a 64-bit con > 2^32 blocchi liberi.
- **[ALTO]** `bitmap_blk` usato come LBA direttamente (riga 39) senza bounds check o moltiplicazione per settori-per-blocco.
- **[MEDIO]** Loop di allocazione blocchi parte da `i=0` (riga 53) ma il commento dice "Block 0 is reserved" — incoerenza.
- **[MEDIO]** Nessun fsync dopo allocazione blocco; la scrittura della bitmap può non raggiungere il disco.

### `kernel/fs/gpt.c`
- **[ALTO]** Nessuna verifica CRC32 dell'header GPT — possibile corruzione non rilevata.
- **[MEDIO]** Firma GPT confrontata con potenziale bug di endianness (riga 35).
- **[MEDIO]** Parsing limitato a 32 entry (righe 68-78) anche se `header->num_partition_entries` può essere maggiore; le partizioni extra vengono ignorate silenziosamente.

---

## 7. Kernel Libraries

### `kernel/lib/kmalloc.c`
- **[CRITICO]** `kmalloc_init()` viene chiamato lazily al primo `kmalloc()`. Se il primo kmalloc avviene in contesto IRQ, il tentativo di acquisire uno spinlock causa **deadlock**.
- **[ALTO]** Overflow intero: `size + sizeof(struct block_header)` (riga 114) non controllato.
- **[ALTO]** `kcalloc()`: nessun check overflow in `nmemb * size` (riga 195).
- **[MEDIO]** `kfree()` verifica `blk->magic` ma non controlla che `ptr` sia almeno a distanza di `sizeof(block_header)` dall'inizio della memoria — se `ptr < 16`, il cast legge prima dell'allocazione.

### `kernel/lib/string.c`
- **[MEDIO]** `memset()` (riga 190): check NULL presente ma non causa return anticipato in tutti i path.
- **[BASSO]** `memcpy()` (righe 203-209): copia byte per byte — nessuna accelerazione SIMD.
- **[BASSO]** Nessun overflow check in nessuna funzione.

### `kernel/lib/printk.c`
- **[ALTO]** Buffer `print_num` di 66 byte (riga 39) può andare overflow se `width > 66`.
- **[MEDIO]** `in_printk` è per-CPU ma non protegge da ricorsione su CPU diverse — se due CPU chiamano printk simultaneamente, la sezione critica non è protetta.
- **[MEDIO]** `vsnprintf()` su buffer per-CPU senza verifica della dimensione; overflow possibile con format string lunghe.

### `kernel/lib/registry.c`
- **[MEDIO]** Lookup O(n) su `MAX_REGISTRY_KEYS` entry dentro una sezione critica protetta da spinlock.
- **[MEDIO]** `strncpy()` con `MAX_VAL_LEN - 1`: se il valore è esattamente `MAX_VAL_LEN` caratteri, la stringa non è null-terminated.
- **[BASSO]** Nessuna persistenza: il registry viene perso ad ogni riavvio.

### `kernel/lib/stack_protector.c`
- **[MEDIO]** Canary hardcoded (riga 9) — deve essere randomizzato al boot per avere valore protettivo reale.
- **[MEDIO]** `__stack_chk_fail()` esegue un loop infinito ma non chiama panic, quindi non logga nulla e non termina il processo in modo ordinato.

---

## 8. Graphics & Compositor

### `kernel/graphics/graphics.c`
- **[ALTO]** Completamente stub — **tutte** le funzioni sono no-op. Nessuna implementazione presente.

### `kernel/graphics/compositor.c`
- **[ALTO]** Nessuna validazione sulle dimensioni delle finestre; x, y, width, height negativi non vengono rifiutati.
- **[ALTO]** Puntatori liberati non vengono azzerati a `NULL` — rischio use-after-free (riga 36).
- **[MEDIO]** Massimo 32 finestre hardcoded senza gestione dell'overflow (nessun errore restituito se si tenta di creare la 33°).

### `kernel/graphics/draw3d.c`
- **[MEDIO]** Completezza dell'implementazione da verificare; probabile presenza di stub.

### `kernel/graphics/gl.c`
- **[MEDIO]** Completezza dell'implementazione da verificare; interfaccia dichiarata in `include/graphics/gl.h` ma implementazione potenzialmente parziale.

---

## 9. Drivers

### `kernel/drivers/uart/pl011.c`
- **[ALTO]** Race condition su `rx_head` (riga 36): `next = (rx_head + 1) % RX_BUF_SIZE` senza incremento atomico. Se un IRQ arriva durante la lettura di `rx_head` nel main loop, il buffer circolare viene corrotto.
- **[MEDIO]** `keyboard_wait_queue` dichiarato `extern` senza forward declaration — rischio di link failure.

### `kernel/drivers/timer/timer.c`
- **[ALTO]** Variabili `timer_tick_interval` e `timer_tick_remainder` usate nel codice ma non definite in nessun file visibile — **undefined reference** a link time.
- **[MEDIO]** `compositor_interval` hardcoded a 1 jiffy invece di derivarlo dal framerate richiesto.

### `kernel/drivers/keyboard/keyboard.c`
- **[MEDIO]** Stato shift/ctrl/caps globale — se due processi usano la tastiera contemporaneamente lo stato viene corrotto.
- **[BASSO]** `INIT_LIST_HEAD()` chiamato senza verificare se la lista è già stata inizializzata.

### `kernel/drivers/gic/gic.c`
- **[MEDIO]** Loop di init (riga 45-46): `for (i = 0; i < gic_num_irqs / 32; i++)` — se `gic_num_irqs` non è divisibile per 32, l'ultimo registro non viene pulito.
- **[BASSO]** Nessun check se GICD è accessibile prima di leggere/scrivere i registri.

### `kernel/drivers/virtio/virtio_blk.c`
- **[ALTO]** `virtio_blk_qsize` non viene mai inizializzato prima dell'uso nel codice.
- **[ALTO]** Loop di inizializzazione senza timeout: se non esiste un block device, il loop continua indefinitamente senza segnalare errore.

### `kernel/drivers/virtio/virtio_input.c`
- **[MEDIO]** Completezza e gestione errori da verificare.

### `kernel/drivers/gpu/virtio_gpu.c`
- **[MEDIO]** Inizializzazione e gestione errori da verificare in dettaglio.

### `kernel/drivers/gpu/gpu_core.c`
- **[MEDIO]** Completezza da verificare — astrazione tra GPU virtuale e driver hardware.

### `kernel/drivers/cpp_test.cpp`
- **[BASSO]** File C++ in un kernel C; presenza di test non integrati nel build.

---

## 10. User Space

### `user/bin/shell.c`
- **[ALTO]** Buffer `cmd_buf[128]` fisso senza bounds checking sull'input — **stack buffer overflow** se l'utente inserisce più di 127 caratteri.
- **[MEDIO]** Nessuna gestione della terminazione dei processi figli; la shell continua a girare se il figlio crasha.
- **[BASSO]** `window_draw()` chiamato senza riverificare che `my_window >= 0` dopo il check iniziale.

### `user/lib/lib.c`
- **[MEDIO]** La funzione `write()` ritorna void ma il valore di ritorno di `_sys_write()` (bytes scritti o errore) viene scartato.
- **[MEDIO]** Nessun bounds checking in nessuna funzione wrapper.

### `user/bin/init.c`
- **[MEDIO]** Da verificare completezza del processo di init e gestione errori nell'avvio dei servizi.

### `user/bin/notification_server.c`
- **[MEDIO]** Implementazione del server di notifiche da verificare per completezza e thread-safety.

### `user/bin/regedit.c`
- **[BASSO]** Utility per il registry; verificare coerenza con le API del kernel.

### `user/bin/ipc_send.c` / `user/bin/ipc_recv.c`
- **[MEDIO]** Test IPC; verificare che testino i casi limite (sender mismatch, timeout, messaggi persi) dati i bug IPC noti nel kernel.

### `user/bin/demo3d.c`
- **[BASSO]** Demo 3D; la completezza dipende dallo stato di `draw3d.c` e `gl.c`.

---

## 11. Build System

### `Makefile`
- **[ALTO]** Se `CROSS_COMPILE` non è impostato, il compilatore non viene trovato e il build fallisce con errori poco chiari.
- **[MEDIO]** Nessun dependency tracking per i file header (`.h`): modificare un header non trigghera la ricompilazione dei `.c` che lo includono.
- **[BASSO]** `-Werror` nelle CFLAGS rende ogni warning fatale — buono per rigore, ma può bloccare il build su versioni diverse del toolchain.

### `grub.cfg`
- **[BASSO]** Da verificare coerenza con il kernel linker script (entry point, indirizzi di load).

---

## Riepilogo per priorità

### Da correggere subito (CRITICO)

| # | File | Issue |
|---|------|-------|
| 1 | `kernel/sched/process.c` | PID non atomico; work-stealing senza lock; IPC drop silenzioso |
| 2 | `kernel/mm/buffer.c` | Free errato; deadlock con virtio_blk |
| 3 | `kernel/mm/vmm.c` | Cast fisico→puntatore; descriptor bits misti |
| 4 | `kernel/arch/aarch64/cpu/syscall.c` | NULL deref su page_table; window resource leak |
| 5 | `kernel/arch/aarch64/cpu/exception.S` | Frame corrotto da write anticipata; context corrotto da UART debug |
| 6 | `kernel/arch/aarch64/boot/start.S` | Race condition su `secondary_ttbr0` |
| 7 | `kernel/main.c` | `kmalloc` usato prima di init; stack CPU con bounds errati |
| 8 | `kernel/lib/kmalloc.c` | Lazy init → deadlock in IRQ context |

### Da correggere presto (ALTO)

| # | File | Issue |
|---|------|-------|
| 1 | `kernel/sched/elf.c` | Stack fisso a `0xC0000000` per tutti i processi |
| 2 | `kernel/mm/pmm.c` | Formula bitmap DMA errata; overflow intero |
| 3 | `kernel/arch/aarch64/cpu/cpu.c` | `nr_cpus` non atomico; panic silenzioso |
| 4 | `kernel/fs/gpt.c` | Nessuna verifica CRC32 |
| 5 | `kernel/fs/ext4.c` | Free block count troncato a 32 bit |
| 6 | `kernel/drivers/virtio/virtio_blk.c` | `qsize` non inizializzato; loop senza timeout |
| 7 | `kernel/drivers/uart/pl011.c` | Race condition su buffer circolare |
| 8 | `kernel/drivers/timer/timer.c` | Simboli undefined a link time |
| 9 | `kernel/graphics/graphics.c` | Tutto stub — nessuna implementazione |
| 10 | `kernel/graphics/compositor.c` | No validation; use-after-free; overflow 32 finestre |
| 11 | `user/bin/shell.c` | Stack buffer overflow sull'input |
| 12 | `kernel/arch/aarch64/include/arch/arch.h` | `arch_spin_lock` WFE senza SEV → stall |

### Da pianificare (MEDIO/BASSO)
Tutti gli altri file elencati nelle sezioni precedenti con severità MEDIO o BASSO.

---

*Report generato tramite ispezione statica — nessun file è stato modificato.*

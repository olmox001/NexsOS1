# OS1 Project Status
*Last updated: 2026-05-11*

## Branch
`dev-research`

---

## Phases completate

### Phase 1 — Scheduler & printk race fixes (committed)
- **Idle task re-enqueue guard**: `process.c` — priority check invece di pointer identity
- **Work-stealing guard**: `process.c` — idle task non viene rubato tra CPU
- **Printk spinlock race**: `printk.c` — `spin_lock_irqsave` acquisito PRIMA di settare `in_printk`
- Sistema stabile 45s+ headless senza ELR=0 panic o context corruption

### Phase 2 — Compositor 30Hz + damage rect (committed 2026-05-11)

#### 2a — 30Hz e debug print
- `kernel/drivers/timer/timer.c`: `COMPOSITOR_TARGET_FPS 30` (era 60) → fires ogni 3 tick (~33Hz effettivi)
- `kernel/arch/aarch64/cpu/cpu.c`: aggiunto `spsr=0x%lx` alla riga KERNEL-USER FAULT debug (line ~125)

#### 2b — Damage rect compositor
- `kernel/graphics/compositor.c`: rimpiazzato `memcpy(fb_va, backbuffer, 3.5MB)` con upload limitato alla bounding box sporca
- Nuove variabili globali: `damage_x1/y1/x2/y2`
- Helper `expand_damage(x, y, w, h)` — cresce solo, mai rimpicciolisce (race-safe senza lock)
- Quattro siti `compositor_dirty = 1` aggiornati:
  - `compositor_window_write` → area finestra + title bar
  - `compositor_handle_click` close → full screen (finestra distrutta)
  - `compositor_handle_click` z-order/drag → full screen (layout cambiato)
  - `compositor_update_mouse` → rettangolo cursore vecchio+nuovo (14×18px), full screen se dragging
- Flush GPU con coordinate esatte della damage rect; reset del damage dopo ogni flush
- Testato stabile 60s headless; nessun crash nuovo introdotto

---

## Bug attivo pre-esistente (NON causato da Phase 2)

### Sintomo
```
[C0] [ERROR] Instruction abort at 0x0000000000000000, FAR=0x0000000000000000
[C0] [ERROR] KERNEL-USER FAULT: EC=0x21 (0x8600000f) ELR=0x437d1cd0 PID=1
[C0] [ERROR] SPSR_EL1: 0x00000000200003c5   ← EL1h mode, tutti gli IRQ mascherati
[C0] [ERROR] ELR_EL1:  0x0000000000000000   ← jumped to NULL (o 0x437d1cd0 = init context ptr)
```

### Analisi
- EC=0x21 = Instruction Abort stesso EL → crash in kernel mode
- `schedule()` ha guard `if (next->context->elr == 0) panic(...)` MA due fast-path `return regs` la bypassano:
  - **Line ~592** (dead code): `if (prev && prev->state == PROC_RUNNING)` — entrambi i branch sopra settano già `PROC_READY`, mai RUNNING qui
  - **Line ~609** (live): `if (prev == next) { ...; return regs; }` — ritorna il frame IRQ non validato
- Se il frame IRQ ha ELR=0 o ELR=ptr-struttura-contesto, il CPU salta lì → Instruction Abort

### Fix strategy (deferred — fuori scope Phase 2)
1. Aggiungere guard ELR=0 al fast-path live (`process.c` ~line 609):
   ```c
   if (prev == next) {
       if (regs->elr == 0) panic("SCHED: BUG elr==0 on same-task return, PID %d", prev->pid);
       __sync_lock_release(&sched_lock);
       return regs;
   }
   ```
2. Verificare che `idle_task_entry != 0` in `main.c` (già confermato, è una funzione reale a ~line 142 di process.c)
3. Eseguire test 60s e confermare stabilità

---

## Fasi pendenti

| Phase | Descrizione | Stato |
|-------|-------------|-------|
| 3 | HAL split — codice arch-specific dietro interfacce C tipizzate | Non iniziato |
| 4 | VFS layer sopra ext4 | Non iniziato |
| 5 | Process manager — segnali, IPC queue drain on death, memory leak | Non iniziato |
| 6 | User-space compositor | Non iniziato |

---

## Come riprendere

```bash
cd "/Users/olmo/iCloud Drive (archivio)/Documents/Antigravity/operate System/gen1/os1test"
git log --oneline -5   # verifica commit recenti
git status             # deve essere pulito
make run               # test con make run (mai qemu direttamente)
```

### Prossima azione consigliata
Scegliere tra:
- **Fixare il bug ELR=0**: modificare `kernel/sched/process.c` ~line 609 con il guard descritto sopra, poi test 60s
- **Iniziare Phase 3 (HAL split)**: isolare il codice arch-specific AArch64 dietro interfacce C, prima di aggiungere altri driver

### Comando di test stabilità
```bash
timeout 60 make run 2>&1 | grep -E "ERROR|PANIC"
```

---

## Mappa file chiave

| File | Descrizione |
|------|-------------|
| `kernel/sched/process.c` | Scheduler, `schedule()`, `sys_ipc_recv()`, `process_wait()` |
| `kernel/drivers/timer/timer.c` | Timer ISR, `timer_handler()`, compositor interval (30Hz) |
| `kernel/arch/aarch64/cpu/exception.S` | Vector table, `irq_stub`, `vector_stub` |
| `kernel/arch/aarch64/cpu/syscall.c` | Syscall dispatch, `syscall_handler()` |
| `kernel/arch/aarch64/cpu/cpu.c` | `sync_handler()`, `get_cpu_info()` |
| `kernel/drivers/gic/gic.c` | `irq_handler()`, tabella dispatch IRQ |
| `kernel/graphics/compositor.c` | `compositor_tick()`, `compositor_render_internal()`, damage rect |
| `kernel/lib/printk.c` | `vprintk()`, `panic()` |
| `kernel/main.c` | Boot, idle task setup, wakeup CPU secondari |
| `user/bin/init.c` | Processo init, supervisor loop |
| `user/bin/shell.c` | Processo shell |

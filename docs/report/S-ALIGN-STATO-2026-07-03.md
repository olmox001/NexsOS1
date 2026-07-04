# S-ALIGN — Stato campagna di allineamento repo (2026-07-03)

> File di handoff scritto su ordine del maintainer ("scrivi tutto in un file e interrompiti") dopo
> l'interruzione per session-limit. Contiene TUTTO lo stato: architettura decisa, lavoro atterrato
> (non committato), risultati audit issue (solo in memoria di sessione, persistiti qui), e prossimi passi.
> Branch `comprehensive-review`, HEAD `6e394cf` (v0.0.4.4). NIENTE PIÙ AGENTI (ordine maintainer).

---

## 1. ARCHITETTURA DI BOOT — decisione FINALE del maintainer (autorevole)

Stratificazione ASTRA: capability (kernel) → servizi (basati su capability) → app (basate sui servizi)
→ utenti (= MASCHERE di capability, astrazione per le app). Fonti: `docs/ASTRA.md` §6.4-6.5/§7.4,
`docs/direction/DIR-04`, `docs/B3-SANDBOX-PLAN.md` §3.1/§3.4.

**TRE sistemi separati nel KERNEL + DUE sistemi separati nell'USERLAND** (system_init è SEMPRE userland):

```
KERNEL — bootloader in 3 SISTEMI SEPARATI (tutti kernel-side):
  K1: memoria/hardware
  K2: inizializzazione (subsystem)
  K3: init userland (parte SOLO quando K1+K2 confermati — barriera; oggi violata: PID1 è
      creato+enqueued in init_scheduler() PRIMA di arch_smp_init(), main.c)

USERLAND — 2 SISTEMI SEPARATI:
  U1: system_init = inizializza i SERVIZI USERLAND DI BASSO LIVELLO.
      ✔ CONFERMATO: ROOT + caps ESPLICITE via process_create_caps — MAI PLVL_MACHINE
        (oggi main.c:259 crea init a MACHINE = violazione di B3 §3.1 "MACHINE = identità macchina").
      Servizi bassi previsti: nxrespawn, nxexec, nxntfy_srv, …
  U2: nxinit = lancia le APP DI SISTEMA (nxui, nxlauncher, nxshell, …) — in seguito, a servizi su.
```

**Nuovi SERVIZI decisi dal maintainer:**
- **nxrespawn** — motore di respawn/supervisione SEPARATO dall'init. ✔ Confermato: il rate-limit
  respawn (USR-INIT-03) va qui.
- **nxexec** — gestione esecuzioni/child: standalone vs needs-shell. Bug che risolve: oggi i figli
  lanciati da nxlauncher che necessitano di una shell CREDONO che nxlauncher SIA una shell (nessuna
  differenziazione). Il meccanismo esatto è documentato in AUDIT-USERLAND (ctty_win/own-window-first).

Classificazione: servizi = ROOT (per-path preset `/sys/bin`); app = USER; FUTURA categoria dedicata
"services" = il per-service capability refinement di DIR-04-Remaining (maschera ridotta per-servizio
invece del blanket ROOT — ceiling oggi M/R/U=CAP_ALL, G=CAP_WINDOW, process.c:599). Shell ROOT per
debug. Oggi i binari `nx*` MISCHIANO servizi e app → dividere secondo ASTRA (servizi = SRL, ASTRA §6.4).
Utenti/login = fase nxperm successiva (NON ora).

## 2. LAVORO ATTERRATO NEL TREE — NON COMMITTATO (da rivedere e committare)

Gli agent (ora vietati) hanno lasciato nel working tree, PRIMA di morire per session-limit:

| File | Stato | Contenuto |
|---|---|---|
| `docs/ASTRA.md` | M (+238/-~20) | §2 violations + §7 status refresh a 2026-07-02 |
| `docs/PENDING-WORK.md` | M (+67) | refresh backlog |
| `docs/direction/DIR-01…07` (tutte e 7) | M (+9…+29 ciascuna) | sezioni "Status (2026-07-02)" appese |
| `docs/report/AUDIT-KERNEL-2026-07-02.md` | NUOVO, 453 righe, completo | mappa boot→K1/K2/K3, logica doppia/API parallele, commenti stali, DIR-06, **top-10 micro-fasi cleanup** |
| `docs/report/AUDIT-USERLAND-2026-07-02.md` | NUOVO, 379 righe, completo | classificazione servizi-vs-app, modello spawn/exec (base nxexec), leftovers, proposta divisione |

⚠ NESSUNO di questi cambi è verificato riga-per-riga dal main thread: **prima di committare va fatta
una review dei diff** (`git diff docs/`) — gli agent avevano l'ordine di citare file:line per ogni
claim, ma la verifica finale spetta al maintainer/main-thread.
⚠ **REGOLA PUSH**: push SOLO dopo test di avvio GRAFICO con interazione del maintainer su ENTRAMBE
le arch: `make run` (aarch64) e `make run ARCH=amd64`. (Modifiche solo-docs: comunque rispettare la regola.)

## 3. AUDIT ISSUE ⇄ CODICE — risultati del cluster completato (persistiti QUI, non esistono altrove)

L'agent issues è morto per session-limit ma il suo sub-audit "userland/fs/drivers" (11 issue) è
COMPLETO. Verdetti verificati sul codice a HEAD `6e394cf` (non sui commit message):

| # | Titolo | Verdetto | Evidenza chiave |
|---|---|---|---|
| 126 | ext4: creazione file | **OPEN-VALID** | ext4.c:796-814 nessun create; fs_ops senza slot .create (vfs.h:70-89); syscall_dispatch.c:266-267 commento vivo "O_CREAT → -EINVAL (#126/#127)" |
| 127 | ext4/VFS: truncate | **OPEN-VALID** | nessun syscall/hook/logica shrink; kilo.c:1380-84 lo documenta |
| 123 | USR-TTY-01 stdout routing | **PARTIAL** | modello ibrido vivo a HEAD: process.c:733-746 `ctty_win` + object.c:652-662 own-window-first→fallback ctty (33cb8cd); truncation 1023B e fd>=100 RIMOSSI; resta window-MODE deliberato + protocollo terminale completo (scrollback, raw/cooked, ECMA-48) |
| 81 | regedit blocking recv | **OPEN-VALID** | regedit.c:109 `recv(0,&msg)` bloccante, mai toccato da giugno |
| 82 | nxfont while(1) yield | **OPEN-VALID** | nxfont.c:277; kernel sys_set_font tiene raw pointer nello heap userland (UAF se esce) |
| 76 | init.cfg mai letto | **OPEN-VALID** | init.c solo commenti; spawn hardcoded (111/125/142/152); init.cfg con path stali |
| 124 | PS/2 hang senza 8042 | **FIXED** (`1a3acc4`) | ps2.c:123-133 presence-gate 0xFF prima dei write; flush bounded 16; poll_until cap 100k |
| 125 | pointer relativo/EV_ABS | **FIXED** (`41fe67c`+`eee8836`+`5a68d45`) | compositor.c:1375-88 sentinella negativa per-asse + scaling INPUT_ABS_MAX; HID tablet usb_hid.c:137-157; resync PS/2 |
| 130 | USB HID sotto UTM | **FIXED nel codice, NON confermato su UTM live** | usb_core.c:79/232 hid_report_len esatto (era 256B over-request); il maintainer stesso su GitHub chiede re-test UTM — SERVE TEST LIVE |
| 106 | doom fire button | **INCONCLUSIVO** | doomgeneric_os1.c:54-105 mapping 'k'/click→KEY_FIRE(0xa3) strutturalmente corretto; serve test di gioco live |
| 174 | nxfilem mouse click | **OPEN-VALID** | dc80001 auto-dichiarato WIP non-funzionante; events.c:146-170 wiring sembra corretto ma mai validato; serve test live |

**Cluster NON completato** (l'altro sub-audit è morto): issue lib/mm/security da code-review
(#151-#161: FDT parser, kmalloc UAF/krealloc, UTF-8 OOB, vsnprintf, backtrace overflow, entropy
fallback, atoi overflow, TOCTOU fault_depth, doppio include), arch (#164 rename OS1_, #167 FPU,
#169/#170 — validate UTM-stabili dal maintainer, candidate a chiusura), graphics (#118 metà
"finestre di processi chiusi restano", #121 GFX-DYN parziale, #128 caret, #131 perf, #133 cursor
UTM), sched (#122 quota figli, #135 timer-cap), epics (#120, #162, #163, #165, #166), #119
(notifiche — rework completo atterrato: candidate a chiusura/PARTIAL), #137/#138/#139/#140 (DIR).
→ Questi restano DA VERIFICARE (senza agent: a mano, un cluster per sessione).

**Nuove issue da APRIRE (già motivate, non ancora create):**
1. Race chiusura-figlio vs window-creation → zombie-PID + finestra orfana non chiudibile
   (docs/PROCESS-KILL-MODEL.md §5b — fix-b facile: destroy senza PID vivo).
2. Lock elementi nxui: flag finestra "locked" (no drag/resize manuale) (§5c).
3. Panic-screen rosso appare solo ~1/2 dopo stress pesante (residuo S1; possibile legame
   corruzione compositor pre-492e5ec — da ri-testare dopo il fix).
4. Per-service capability refinement (categoria "services", DIR-04 Remaining).
5. Boot separation K1/K2/K3 + split system_init/nxinit + nxrespawn/nxexec (architettura §1).

## 4. FATTO IN QUESTA SESSIONE (già committato/pushato in precedenza)

- `492e5ec` **S1b**: lock `compositor_update_mouse` (unico mutatore window-list senza lock) + clamp
  draw_w/h — il crash demo3d-drag (RIP=0xf9). Stress-validazione interattiva → maintainer.
- `cd49103` docs kill-model §5b/5c. (Prima ancora: S1 rosso `framebuffer_virt` in `badaf26`;
  notifiche complete `eb08ab1` e precedenti — vedi memoria di progetto.)

## 5. PROSSIMI PASSI (in ordine, SENZA agent)

1. **Review dei diff docs** (`git diff docs/` + i 2 report) → correzioni → commit locale delle docs.
2. **Leggere i 2 report AUDIT** (kernel: top-10 micro-fasi; userland: divisione servizi/app + modello
   spawn per nxexec) → fondere in una sequenza unica di micro-fasi S-ALIGN nel piano
   `~/.claude/plans/nexs-stabilization-usability.md`.
3. **Completare l'audit issue rimanente a mano** (cluster per cluster) + creare le 5 nuove issue +
   chiudere le FIXED (#124, #125; #130/#169/#170 dopo conferma UTM del maintainer).
4. **Eseguire le micro-fasi cleanup** (spuntando issue), build 2 arch a ogni passo.
5. **Test grafico interattivo col maintainer** (`make run`, `make run ARCH=amd64`) → push.
6. Poi implementazione S2 sulla base allineata: S2.1 barriera K3, S2.2 splash raw, S2.3 split
   system_init/nxinit + nxrespawn/nxexec (architettura §1) → S2b → S3…S8 → main-road 4.1.

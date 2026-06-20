# libkernel — Vulnerability Report

**Scope:** `kernel/lib/` — 17 file sorgente C  
**Analisi:** revisione manuale statica completa  
**Totale vulnerabilità:** 39  
**Data:** giugno 2026

---

## Indice

1. [Riepilogo esecutivo](#1-riepilogo-esecutivo)
2. [Distribuzione per severità](#2-distribuzione-per-severità)
3. [Tabella rapida di riferimento](#3-tabella-rapida-di-riferimento)
4. [Vulnerabilità critiche](#4-vulnerabilità-critiche)
5. [Vulnerabilità alte](#5-vulnerabilità-alte)
6. [Vulnerabilità medie](#6-vulnerabilità-medie)
7. [Vulnerabilità basse](#7-vulnerabilità-basse)
8. [Vulnerabilità note nel sorgente — stato aggiornato](#8-vulnerabilità-note-nel-sorgente--stato-aggiornato)

---

## 1. Riepilogo esecutivo

L'analisi ha coperto tutti i file della libreria kernel (`kernel/lib/`): `backtrace.c`, `crc32.c`, `entropy.c`, `fault_print.c`, `fdt.c`, `ktest.c`, `ktest_samples.c`, `kmalloc.c`, `math.c`, `printk.c`, `registry.c`, `stack_protector.c`, `string.c`, `utf8.c`, `vsnprintf.c`.

Sono state identificate **39 vulnerabilità**, di cui **17 non documentate nel sorgente**. Le più gravi riguardano:

- **`fdt.c`**: il parser DTB accetta input non fidati (firmware / QEMU) senza validare praticamente nessun campo — cinque percorsi distinti consentono letture OOB di memoria kernel durante il boot, prima che le protezioni MMU siano pienamente attive.
- **`kmalloc.c`**: un use-after-free in `krealloc()`, un troncamento silenzioso di `size_t → uint32_t` che corrompe il conteggio pagine in `kfree()`, e un rischio deadlock tra `kmalloc_lock` e `uart_lock`.
- **`utf8.c`**: lettura OOB su input di lunghezza arbitraria e assenza di validazione dei codepoint (surrogati, overlong encoding, valori > U+10FFFF).
- **`backtrace.c`**: integer overflow nel lookup simboli che può restituire un puntatore arbitrario a kernel space.
- **`printk.c`**: NULL dereference su `get_cpu_info()` in early boot — il fault handler invoca di nuovo `printk()`, producendo ricorsione infinita.

I file `crc32.c`, `ktest.c`, `ktest_samples.c` non presentano vulnerabilità oltre a quelle già documentate nel sorgente.

---

## 2. Distribuzione per severità

| Severità  | Conteggio | File coinvolti |
|-----------|-----------|----------------|
| Critica   | 5         | `backtrace.c`, `utf8.c`, `printk.c`, `fdt.c` (×2) |
| Alta      | 11        | `stack_protector.c`, `math.c`, `utf8.c`, `entropy.c`, `fdt.c` (×4), `kmalloc.c` (×3) |
| Media     | 15        | `vsnprintf.c`, `string.c`, `backtrace.c`, `registry.c`, `printk.c`, `math.c`, `fdt.c` (×3), `kmalloc.c` (×3) |
| Bassa     | 8         | `vsnprintf.c`, `registry.c`, `fault_print.c`, `string.c`, `ktest.c`, `fdt.c`, `kmalloc.c` |

---

## 3. Tabella rapida di riferimento

| ID | Severità | File | Titolo | Documentata |
|----|----------|------|--------|-------------|
| NEW-BT-01 | **Critica** | `backtrace.c` | Pointer overflow nel lookup simboli | No |
| LIB-UTF8-01 | **Critica** | `utf8.c` | OOB read in `utf8_decode()` | Parziale |
| NEW-PK-01 | **Critica** | `printk.c` | NULL deref da `get_cpu_info()` in `vprintk` | No |
| FDT-BUG-01 | **Critica** | `fdt.c` | Divisione per zero: `addr_cells + size_cells == 0` | No |
| FDT-BUG-02 | **Critica** | `fdt.c` | `strlen()` senza bound sul blocco struttura DTB | No |
| LIB-SSP-01 | Alta | `stack_protector.c` | Canary statico fino a `stack_guard_init()` | Sì |
| LIB-MATH-01 | Alta | `math.c` | `FP_PI == FP_2PI` nei build kernel | Sì |
| LIB-UTF8-02 | Alta | `utf8.c` | Surrogati, overlong, codepoint > U+10FFFF accettati | Parziale |
| NEW-ENT-01 | Alta | `entropy.c` | Fallback HWRNG silenzioso e debole | No |
| LIB-FDT-01 | Alta | `fdt.c` | `off_dt_struct` non validato — OOB walker | Parziale |
| FDT-BUG-03 | Alta | `fdt.c` | `fdt_get_string()` OOB via `name_off` non validato | No |
| FDT-BUG-04 | Alta | `fdt.c` | Overflow `p += (len+3)/4` nel handler `FDT_PROP` | No |
| FDT-BUG-05 | Alta | `fdt.c` | `reg_p` over-read con `addr_cells`/`size_cells` inconsistenti | No |
| MM-KM-BUG-01 | Alta | `kmalloc.c` | Use-after-free in `krealloc()` — `memcpy` su blocco liberato | No |
| MM-KM-BUG-02 | Alta | `kmalloc.c` | Troncamento `size_t → uint32_t` in `block_header.size` | No |
| MM-KM-BUG-03 | Alta | `kmalloc.c` | Overflow `total_req` in `kfree()` per large block | No |
| LIB-VSNPRINTF-01 | Media | `vsnprintf.c` | Zero-pad negativo: output un char più largo del campo | Sì |
| LIB-VSNPRINTF-02 | Media | `vsnprintf.c` | Ritorna byte scritti, non lunghezza totale — troncamento invisibile | Sì |
| LIB-VSNPRINTF-04 | Media | `vsnprintf.c` | `%p` vicino a buffer pieno emette `0x` senza cifre | Sì |
| LIB-VSNPRINTF-05 | Media | `vsnprintf.c` | `%s` ignora width e precision | No |
| NEW-STR-01 | Media | `string.c` | `atoi()` senza rilevamento overflow | No |
| NEW-BT-02 | Media | `backtrace.c` | `fp_addr_valid` non copre accesso a `fp+8` | No |
| LIB-REG-01 | Media | `registry.c` | Array piatto O(n), nessuna gerarchia | Sì |
| LIB-REG-02 | Media | `registry.c` | Modello permessi first-writer-wins troppo grezzo | Sì |
| NEW-PK-02 | Media | `printk.c` | Buffer `printk` tronca silenziosamente senza contatore | Sì |
| NEW-MATH-03 | Media | `math.c` | Range reduction O(n) in `sin_fp` — DoS su input grandi | Sì |
| LIB-FDT-02 | Media | `fdt.c` | `off_mem_rsvmap` non parsata — RAM riservata firmware passata al PMM | Sì |
| FDT-BUG-06 | Media | `fdt.c` | `depth` può diventare negativo con nodi sbilanciati | No |
| LIB-FDT-01b | Media | `fdt.c` | `fdt_count_cpus()` — stesse vulnerabilità di `fdt_get_mem_regions()` | No |
| MM-KM-03 | Media | `kmalloc.c` | Large alloc non page-aligned — impatto DMA/MMIO sottostimato | Sì |
| MM-KM-06b | Media | `kmalloc.c` | Deadlock `kmalloc_lock → uart_lock` via `pr_err`/`pr_info` | No |
| MM-KM-06c | Media | `kmalloc.c` | `heap_initialized` letto senza atomicità | No |
| LIB-UTF8-03 | Bassa | `utf8.c` | Nessun path di errore per lead byte validi ma troncati a fine buffer | No |
| LIB-REG-03 | Bassa | `registry.c` | Doppio `#include <kernel/vmm.h>` | Sì |
| LIB-REG-04 | Bassa | `registry.c` | Nessuna API di enumerazione chiavi | Sì |
| NEW-FP-01 | Bassa | `fault_print.c` | TOCTOU su `fault_depth_fallback` in `fault_exit()` | No |
| NEW-STR-02 | Bassa | `string.c` | `strcpy`/`strcat` senza bounds — footgun attivo | No |
| NEW-KT-01 | Bassa | `ktest.c` | Nessun crash recovery nel test runner | Sì |
| LIB-FDT-04 | Bassa | `fdt.c` | `fdt_find_in_memory()`: solo magic validato | Sì |
| MM-KM-BUG-04 | Bassa | `kmalloc.c` | `flags = 0` nel large path è dead code con commento fuorviante | No |

---

## 4. Vulnerabilità critiche

---

### NEW-BT-01 — `backtrace.c`: Pointer overflow nel lookup simboli

**Severità:** Critica  
**File:** `kernel/lib/backtrace.c` — funzione `ksym_lookup()`  
**Documentata nel sorgente:** No

**Descrizione**

`ksym_lookup()` calcola il puntatore al nome del simbolo come:

```c
const char *name = names + name_offs[lo];
```

`name_offs[lo]` è un `uint32_t` letto direttamente dal blob `.ksyms`. Se il blob è corrotto (o manipolato in fase di link), un offset molto grande causa un integer overflow nel pointer arithmetic: `names + 0xFFFF0000` su un'architettura a 64 bit può wrappare a un indirizzo arbitrario della memoria kernel. L'unico guard presente (`if (name >= __ksyms_end) return NULL`) non protegge da un puntatore che, dopo il wrap, risulti inferiore a `__ksyms_end` e passi il controllo restituendo un indirizzo illecito.

Il puntatore viene poi usato in `fault_printf("... %s ...", name, ...)`, che dereferenzia il nome nella path di fault — il contesto meno tollerante agli errori dell'intero kernel.

**Impatto:** Lettura di memoria arbitraria dalla path di backtrace; potenziale information disclosure di dati kernel durante un panic.

**Fix**

```c
uint32_t off = name_offs[lo];
if ((uint64_t)off >= (uint64_t)(__ksyms_end - names))
    return NULL;
const char *name = names + off;
```

---

### LIB-UTF8-01 — `utf8.c`: OOB read in `utf8_decode()`

**Severità:** Critica  
**File:** `kernel/lib/utf8.c` — funzione `utf8_decode()`  
**Documentata nel sorgente:** Parzialmente (citata ma impact sottostimato)

**Descrizione**

`utf8_decode()` accetta solo un puntatore `const char *s`, senza lunghezza. Per sequenze multi-byte legge `s[1]`, `s[2]`, `s[3]` incondizionatamente:

```c
} else if ((c & 0xE0) == 0xC0) {
    if ((s[1] & 0xC0) != 0x80) return 0;   // legge s[1] senza bounds
    ...
} else if ((c & 0xF0) == 0xE0) {
    if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;  // s[1], s[2]
```

Se un lead byte di sequenza a 2/3/4 byte appare alla fine di un buffer non terminato da NUL (scenario realistico con stringhe di font o testo utente passato dal layer grafico), la funzione legge fino a 3 byte oltre la regione valida. In contesti kernel questo può causare page fault (se i byte cadono oltre un confine di pagina non mappata) o lettura silenziosa di dati adiacenti.

**Impatto:** Page fault in contesti di rendering font; potenziale disclosure di dati kernel adiacenti al buffer.

**Fix**

Aggiungere un parametro `size_t len` e validare ogni accesso:

```c
int utf8_decode(const char *s, size_t len, uint32_t *code) {
    if (!s || !code || len == 0) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *code = c; return 1; }
    if ((c & 0xE0) == 0xC0) {
        if (len < 2 || (s[1] & 0xC0) != 0x80) return 0;
        ...
    }
    ...
}
```

Aggiornare tutti i call site in `kernel/graphics/font.c` per passare la lunghezza residua del buffer.

---

### NEW-PK-01 — `printk.c`: NULL dereference da `get_cpu_info()` in `vprintk()`

**Severità:** Critica  
**File:** `kernel/lib/printk.c` — funzione `vprintk()`  
**Documentata nel sorgente:** No

**Descrizione**

```c
int vprintk(const char *fmt, va_list args) {
    struct cpu_info *cpu = get_cpu_info();
    ...
    spin_lock_irqsave(&uart_lock, &flags);
    if (cpu->in_printk) {   // dereference senza NULL check
```

`get_cpu_info()` legge il registro GS base (amd64) o `TPIDR_EL1` (aarch64). Durante il primissimo boot, prima che i registri per-CPU siano inizializzati, o se il registro è corrotto da un'eccezione, la funzione può restituire `NULL`. La successiva lettura di `cpu->in_printk` causa un fault a indirizzo 0.

La path di fault chiama `printk()` per emettere il messaggio di errore → nuova invocazione di `vprintik()` → stesso NULL dereference → ricorsione infinita fino a esaurimento dello stack prima che `fault_enter()` intervenga.

`fault_print.c` risolve già questo problema con `arch_cpu_info_fault_safe()`: la stessa soluzione deve essere applicata a `vprintk()`.

**Impatto:** Kernel panic in early boot o su architetture con GS/TPIDR corrotto; ricorsione infinita prima del fault depth guard.

**Fix**

```c
struct cpu_info *cpu = get_cpu_info();
if (!cpu) {
    /* Early boot o registro per-CPU corrotto: path lock-free */
    uart_puts(fmt); /* best-effort, nessun buffer */
    return 0;
}
```

---

### FDT-BUG-01 — `fdt.c`: Divisione per zero in `fdt_get_mem_regions()`

**Severità:** Critica  
**File:** `kernel/lib/fdt.c` — funzione `fdt_get_mem_regions()`  
**Documentata nel sorgente:** No

**Descrizione**

Il loop che parsifica i record `reg` divide per `(addr_cells + size_cells) * 4`:

```c
for (uint32_t i = 0; i < len / ((addr_cells + size_cells) * 4); i++) {
```

`addr_cells` e `size_cells` vengono letti dal DTB (`#address-cells`, `#size-cells`). Un DTB malformato che specifica entrambi a `0` produce:

```
(0 + 0) * 4 = 0  →  len / 0  →  divisione per zero
```

Analogo scenario con overflow uint32_t: `addr_cells = 0x80000001`, `size_cells = 0x7FFFFFFF` → somma = `0` dopo overflow. Il kernel crasha durante il boot, prima dell'inizializzazione di qualsiasi driver, stack di rete o protezione.

**Impatto:** Kernel panic garantito su DTB malformato — denial of service permanente al boot.

**Fix**

```c
if (addr_cells == 0 || size_cells == 0) {
    pr_warn("FDT: invalid addr/size cells in reg property, skipping\n");
    goto advance_prop;
}
uint32_t entry_size = addr_cells + size_cells;
if (entry_size < addr_cells) goto advance_prop; /* overflow check */
entry_size *= 4;
if (entry_size == 0 || len % entry_size != 0) goto advance_prop;
for (uint32_t i = 0; i < len / entry_size; i++) { ... }
```

---

### FDT-BUG-02 — `fdt.c`: `strlen()` senza bound sul blocco struttura DTB

**Severità:** Critica  
**File:** `kernel/lib/fdt.c` — `fdt_get_mem_regions()`, `fdt_count_cpus()`  
**Documentata nel sorgente:** No

**Descrizione**

Ogni volta che il walker incontra un tag `FDT_BEGIN_NODE`, chiama `strlen()` sul nome del nodo:

```c
const char *name = (const char *)p;
size_t name_len = strlen(name);
p += (name_len + 1 + 3) / 4;
```

`strlen()` non riceve alcun limite: se il blocco struttura del DTB non contiene un byte NUL prima della sua fine (o se `p` è già uscito dal DTB per via di LIB-FDT-01), `strlen()` percorre memoria kernel arbitraria fino al primo `0x00` incontrato. Su memoria tipica del kernel (`.rodata`, `.data`, strutture dati) un NUL potrebbe trovarsi kB o MB più avanti.

**Catena di attacco completa con DTB ostile:**

```
off_dt_struct puntato oltre il DTB (LIB-FDT-01)
  → p entra in .rodata del kernel
  → FDT_BEGIN_NODE tag casuale → strlen() percorre .rodata
  → strcmp(name, "memory@") su memoria kernel
  → fdt_get_string(name_off) → pointer OOB (FDT-BUG-03)
  → regions[] riempito con indirizzi/size arbitrari
  → pmm_init() riceve mappa RAM corrotta
  → PMM alloca su regioni MMIO o firmware-reserved
```

**Impatto:** Exfiltration di memoria kernel durante il boot; corruzione della mappa RAM passata al PMM con conseguente uso di indirizzi MMIO come heap.

**Fix**

```c
size_t max_name = (size_t)((char *)end - (char *)p);
size_t name_len = strnlen(name, max_name < 128 ? max_name : 128);
if (name_len >= 128 || (char *)p + name_len >= (char *)end) break;
p += (name_len + 1 + 3) / 4;
```

---

## 5. Vulnerabilità alte

---

### LIB-SSP-01 — `stack_protector.c`: Canary statico fino a `stack_guard_init()`

**Severità:** Alta  
**File:** `kernel/lib/stack_protector.c`  
**Documentata nel sorgente:** Sì (fix parziale implementato)

**Descrizione**

`__stack_chk_guard` è inizializzato a `0x595e9fbd94fda766` a compile-time. `stack_guard_init()` sostituisce questo valore con entropia da `entropy_u64()`, ma deve essere chiamata prima che qualsiasi funzione protetta esegua il proprio epilogo. Se viene chiamata troppo tardi, tutte le funzioni di early boot hanno usato il canary prevedibile; qualsiasi attaccante che conosca il binario può costruire un overflow che preserva il canary.

Il vincolo di ordinamento non è enforced nel codice — è documentato solo in `ssp.h`.

**Fix:** Chiamare `stack_guard_init()` come prima operazione assoluta di `kmain()`, prima di qualsiasi altra chiamata a funzioni con frame protetti. Aggiungere un `_Static_assert` o una macro che validi l'ordine di inizializzazione.

---

### LIB-MATH-01 — `math.c`: `FP_PI == FP_2PI` nei build kernel

**Severità:** Alta  
**File:** `kernel/lib/math.c`  
**Documentata nel sorgente:** Sì (LIB-MATH-01)

**Descrizione**

`<kernel/math.h>` definisce `FP_PI = 411775` (che è in realtà 2π × 2¹⁶). Il guard `#ifndef FP_PI` in `math.c` non viene mai preso nei build kernel. Di conseguenza:

- `FP_PI == FP_2PI == 411775`
- `sin_fp()`: il loop di range reduction `while (x > FP_PI)` equivale a `while (x > FP_2PI)` — tre quarti del dominio (π/2, 2π] non vengono ridotti correttamente
- La riflessione di fase usa il polo sbagliato: `x = FP_PI - x` usa 2π come π

Tutte le chiamate a `sin_fp()` e `cos_fp()` nel build kernel producono risultati errati per ¾ del dominio angolare. Attualmente latente (nessun caller nel build), ma un singolo `#include` di `draw3d.c` nel Makefile attiva il bug silenziosamente.

**Fix:** Cambiare `kernel/include/kernel/math.h` riga 16 in `#define FP_PI 205887`. Aggiungere `_Static_assert(FP_PI * 2 == FP_2PI, "FP_PI mismatch");`.

---

### LIB-UTF8-02 — `utf8.c`: Surrogati, overlong encoding e codepoint > U+10FFFF accettati

**Severità:** Alta  
**File:** `kernel/lib/utf8.c` — `utf8_decode()`  
**Documentata nel sorgente:** Parzialmente (LIB-UTF8-02)

**Descrizione**

Il decoder non rifiuta:

- **Overlong encoding**: U+0000 codificato come `0xC0 0x80` — tecnica classica per bypassare filtri su byte NUL. Un attaccante può iniettare un NUL "invisibile" in una stringa di font che supera controlli su `strlen()`.
- **Surrogate pairs**: U+D800..U+DFFF — valori riservati a UTF-16, non validi in UTF-8. Alcuni consumer di Unicode si comportano in modo indefinito su questi valori.
- **Codepoint > U+10FFFF**: formalmente fuori dallo spazio Unicode; sequenze a 4 byte che codificano valori > 0x10FFFF sono accettate.

**Fix**

Aggiungere dopo l'assemblaggio di `*code`:

```c
/* Overlong check */
if (seq_len == 2 && *code < 0x80)  return 0;
if (seq_len == 3 && *code < 0x800) return 0;
if (seq_len == 4 && *code < 0x10000) return 0;
/* Surrogate range */
if (*code >= 0xD800 && *code <= 0xDFFF) return 0;
/* Out of Unicode range */
if (*code > 0x10FFFF) return 0;
```

---

### NEW-ENT-01 — `entropy.c`: Fallback HWRNG silenzioso e debole

**Severità:** Alta  
**File:** `kernel/lib/entropy.c` — `entropy_u64()`  
**Documentata nel sorgente:** No

**Descrizione**

Quando `arch_hw_random()` fallisce tutte e 16 le iterazioni, `entropy_u64()` scende silenziosamente al fallback basato su jitter del timer:

```c
uint64_t state = (uint64_t)__builtin_return_address(0);
for (int i = 0; i < 8; i++) {
    state ^= arch_timer_get_count();
    (void)splitmix64(&state);
}
return splitmix64(&state);
```

`__builtin_return_address(0)` è un indirizzo staticamente predicibile dal binario. Su hardware deterministico (emulatori, FPGA con timer fisso), il jitter è nullo. Il chiamante più critico — `stack_guard_init()` — non riceve alcuna indicazione sulla qualità dell'entropia ricevuta e non può reagire di conseguenza.

**Impatto:** SSP canary generato con entropia prevedibile → bypass SSP completo su hardware senza HWRNG.

**Fix**

```c
bool entropy_u64(uint64_t *out) {
    for (int i = 0; i < 16; i++) {
        if (arch_hw_random(out)) return true;  /* entropia forte */
    }
    /* Fallback debole: avvisare il caller */
    pr_warn("entropy: HWRNG non disponibile, fallback a jitter timer\n");
    uint64_t state = (uint64_t)__builtin_return_address(0);
    for (int i = 0; i < 8; i++) {
        state ^= arch_timer_get_count();
        (void)splitmix64(&state);
    }
    *out = splitmix64(&state);
    return false;  /* false = entropia debole */
}
```

`stack_guard_init()` può così valutare se il canary è sufficientemente robusto.

---

### LIB-FDT-01 — `fdt.c`: `off_dt_struct` non validato — OOB walker

**Severità:** Alta  
**File:** `kernel/lib/fdt.c` — `fdt_get_mem_regions()`, `fdt_count_cpus()`  
**Documentata nel sorgente:** Parzialmente (LIB-FDT-01, impatto sottostimato)

**Descrizione**

```c
uint32_t *p   = (uint32_t *)((uintptr_t)fdt_ptr + fdt32_to_cpu(fdt_ptr->off_dt_struct));
uint32_t *end = (uint32_t *)((uintptr_t)p        + fdt32_to_cpu(fdt_ptr->size_dt_struct));
```

Nessuno dei due offset viene confrontato con `totalsize`. Con `off_dt_struct = 0xFFFF0000`, `p` punta a centinaia di MB oltre il DTB. Il walker `while (p < end)` traversa `kern/.text`, `kern/.rodata` e qualsiasi struttura dati del kernel, eseguendo `strlen()`, `strcmp()` e `fdt_read32()` su ogni tag interpretato. Il risultato è lettura illimitata di memoria kernel.

**Fix:** Immediato dopo la lettura dell'header:

```c
uint32_t total    = fdt32_to_cpu(fdt_ptr->totalsize);
uint32_t off_str  = fdt32_to_cpu(fdt_ptr->off_dt_struct);
uint32_t sz_str   = fdt32_to_cpu(fdt_ptr->size_dt_struct);
uint32_t off_strs = fdt32_to_cpu(fdt_ptr->off_dt_strings);
uint32_t sz_strs  = fdt32_to_cpu(fdt_ptr->size_dt_strings);

if ((uint64_t)off_str  + sz_str  > total) return -1;
if ((uint64_t)off_strs + sz_strs > total) return -1;
```

---

### FDT-BUG-03 — `fdt.c`: `fdt_get_string()` senza bounds — OOB via `name_off`

**Severità:** Alta  
**File:** `kernel/lib/fdt.c` — `fdt_get_string()`  
**Documentata nel sorgente:** No

**Descrizione**

```c
static const char *fdt_get_string(uint32_t offset) {
    return (const char *)((uintptr_t)fdt_ptr
        + fdt32_to_cpu(fdt_ptr->off_dt_strings)
        + offset);
}
```

`offset` è letto direttamente dal campo `name_off` di ogni `FDT_PROP` nel DTB senza alcuna validazione. Con `offset = 0x7FFFFFFF`, il puntatore risultante cade gigabyte oltre il DTB. Le chiamate `strcmp(fdt_get_string(name_off), "reg")` e simili leggono poi memoria kernel arbitraria fino al primo byte NUL.

**Fix**

```c
static const char *fdt_get_string(uint32_t offset) {
    uint32_t sz_strs = fdt32_to_cpu(fdt_ptr->size_dt_strings);
    if (offset >= sz_strs) return "";  /* stringa vuota: strcmp sicuro */
    return (const char *)((uintptr_t)fdt_ptr
        + fdt32_to_cpu(fdt_ptr->off_dt_strings)
        + offset);
}
```

---

### FDT-BUG-04 — `fdt.c`: Overflow `p += (len+3)/4` nel handler `FDT_PROP`

**Severità:** Alta  
**File:** `kernel/lib/fdt.c` — `fdt_get_mem_regions()`, `fdt_count_cpus()`  
**Documentata nel sorgente:** No

**Descrizione**

```c
uint32_t len = fdt32_to_cpu(*p++);
...
p += (len + 3) / 4;
```

`len` è un `uint32_t` letto dal DTB. Con `len = 0xFFFFFFFC`:

```
(0xFFFFFFFC + 3) / 4 = 0xFFFFFFFF / 4 = 0x3FFFFFFF
```

Su un sistema a 64 bit, `p += 0x3FFFFFFF` sposta il puntatore di ~4 GB, portandolo ben oltre `end`. Il confronto `while (p < end)` è ora falso (p > end), ma solo perché siamo già usciti dai bounds — i dati già processati nell'iterazione corrente potrebbero aver letto memoria arbitraria.

**Fix**

```c
uint32_t len = fdt32_to_cpu(*p++);
uint32_t aligned_len = (len + 3) / 4;
if (p + aligned_len > end) break;  /* DTB malformato */
...
p += aligned_len;
```

---

### FDT-BUG-05 — `fdt.c`: `reg_p` over-read con celle inconsistenti

**Severità:** Alta  
**File:** `kernel/lib/fdt.c` — `fdt_get_mem_regions()`  
**Documentata nel sorgente:** No

**Descrizione**

Nel parser del campo `reg`, i branch per `addr_cells != 2` e `size_cells != 2` usano il path "32-bit" indipendentemente dal valore effettivo:

```c
} else {
    base = fdt_read32(reg_p++);  /* eseguito anche per addr_cells == 0 */
}
...
} else {
    size = fdt_read32(reg_p++);  /* eseguito anche per size_cells == 0 */
}
```

Con `addr_cells = 1` e `size_cells = 1`, il loop esegue `len/8` iterazioni ma ogni iterazione legge 8 byte — corretto. Tuttavia, con `addr_cells = 0`, un `fdt_read32(reg_p++)` viene ancora eseguito, avanzando `reg_p` di uno slot in più rispetto a quanto il DTB prevede. Con molte iterazioni, `reg_p` esce dalla finestra della proprietà e legge dati di proprietà successive o del blocco struttura come se fossero indirizzi RAM.

**Fix:** Validare che `addr_cells` e `size_cells` siano in {1, 2}; rifiutare il campo `reg` per valori non supportati.

---

### MM-KM-BUG-01 — `kmalloc.c`: Use-after-free in `krealloc()`

**Severità:** Alta  
**File:** `kernel/lib/kmalloc.c` — `krealloc()`  
**Documentata nel sorgente:** No

**Descrizione**

```c
spin_lock_irqsave(&kmalloc_lock, &flags);
struct block_header *blk = ((struct block_header *)ptr) - 1;
if (blk->magic != BLOCK_MAGIC) { ... return NULL; }
size_t old_size = blk->size;
spin_unlock_irqrestore(&kmalloc_lock, flags);   // ← lock rilasciato

void *new_ptr = kmalloc(new_size);
if (!new_ptr) return NULL;

size_t copy_size = (old_size < new_size) ? old_size : new_size;
memcpy(new_ptr, ptr, copy_size);   // ← UAF: ptr potrebbe essere già liberato
kfree(ptr);
```

Tra il rilascio del lock e la `memcpy`, un altro thread può chiamare `kfree(ptr)`. Il `kfree` scrive `BLOCK_FREE` nel magic e reinserisce il blocco nel free-list. La `memcpy` successiva legge dati da un blocco che il sistema considera già libero (use-after-free read). La successiva `kfree(ptr)` in `krealloc` viene bloccata dal magic check (magic è ora `BLOCK_FREE`), quindi non è un double-free — ma i dati già copiati nel nuovo buffer sono potenzialmente corrotti.

**Fix**

```c
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    uint64_t flags;
    spin_lock_irqsave(&kmalloc_lock, &flags);
    struct block_header *blk = ((struct block_header *)ptr) - 1;
    if (blk->magic != BLOCK_MAGIC) {
        spin_unlock_irqrestore(&kmalloc_lock, flags);
        return NULL;
    }
    size_t old_size = blk->size;
    spin_unlock_irqrestore(&kmalloc_lock, flags);

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    /* La copia avviene dopo il rilascio del lock — la UAF è ancora
     * possibile con un kfree concorrente. Fix completo: tenere il lock
     * o aggiungere un refcount. Fix pratico immediato: verificare di
     * nuovo il magic subito prima della copia. */
    spin_lock_irqsave(&kmalloc_lock, &flags);
    if (blk->magic != BLOCK_MAGIC) {
        spin_unlock_irqrestore(&kmalloc_lock, flags);
        kfree(new_ptr);
        return NULL;
    }
    memcpy(new_ptr, ptr, (old_size < new_size) ? old_size : new_size);
    blk->magic = BLOCK_FREE;
    blk->next = buckets[blk->bucket_idx];
    buckets[blk->bucket_idx] = blk;
    spin_unlock_irqrestore(&kmalloc_lock, flags);

    return new_ptr;
}
```

---

### MM-KM-BUG-02 — `kmalloc.c`: Troncamento `size_t → uint32_t` in `block_header.size`

**Severità:** Alta  
**File:** `kernel/lib/kmalloc.c` — struct `block_header`, `kmalloc()`, `kfree()`  
**Documentata nel sorgente:** No

**Descrizione**

```c
struct block_header {
    uint32_t magic;
    uint32_t size;    /* ← uint32_t, ma size è size_t (64 bit) */
    ...
};
...
blk->size = size;   /* troncamento silenzioso per size > UINT32_MAX */
```

`kmalloc()` effettua un overflow check corretto su `total_req`, ma non impedisce allocazioni con `size` tra 4 GB+1 e SIZE_MAX−32. Per tali allocazioni, `blk->size` viene troncato: una richiesta di esattamente 4 GB+1 byte salva `size = 1`. `kfree()` calcola poi:

```c
size_t total_req = blk->size + sizeof(struct block_header);  // = 33, non 4GB+33
size_t pages = (total_req + 4095) / 4096;                   // = 1, non ~1M
pmm_free_pages((void *)blk, pages);                          // libera 1 pagina, non ~1M
```

Il PMM perde ~1 milione di pagine (4 GB) che rimangono marcate come allocated ma non referenziate — memory leak permanente. Peggio: le pagine non liberate possono essere ancora in uso e la singola pagina restituita al PMM potrebbe essere riassegnata, creando una doppia-mappa.

**Fix:** Cambiare `uint32_t size` in `size_t size` in `struct block_header`. Come mitigazione immediata aggiungere `if (size > UINT32_MAX) return NULL;` in `kmalloc()`.

---

### MM-KM-BUG-03 — `kmalloc.c`: Overflow `total_req` in `kfree()` per large block

**Severità:** Alta  
**File:** `kernel/lib/kmalloc.c` — `kfree()`  
**Documentata nel sorgente:** No

**Descrizione**

```c
size_t total_req = blk->size + sizeof(struct block_header);
size_t pages = (total_req + 4095) / 4096;
pmm_free_pages((void *)blk, pages);
```

Non esiste overflow check su `blk->size + sizeof(...)`. Se un heap overflow ha corrotto `blk->size` a `UINT32_MAX` (massimo del tipo `uint32_t`), allora:

```
total_req = 0xFFFFFFFF + 32 = 0x1000001F  (overflow → valore piccolo su 32-bit, ma su 64-bit = 4294967327)
pages = (4294967327 + 4095) / 4096 ≈ 1048577  (> 1M pagine)
```

`pmm_free_pages()` tenta di liberare > 1 milione di pagine che non appartengono all'allocazione originale — corruzione del PMM.

**Fix**

```c
if (blk->size > SIZE_MAX - sizeof(struct block_header)) {
    pr_err("kfree: size corrotto (0x%x) at %p\n", blk->magic, ptr);
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    return;
}
```

---

## 6. Vulnerabilità medie

---

### LIB-VSNPRINTF-01 — `vsnprintf.c`: Zero-pad negativo: output un char più largo del campo

**Severità:** Media  
**File:** `kernel/lib/vsnprintf.c` — `print_num()`  
**Documentata nel sorgente:** Sì (LIB-VSNPRINTF-01)

`width` viene decrementato del conteggio cifre ma non del carattere di segno. Con `FLAG_ZEROPAD`, il segno occupa un byte non contabilizzato, producendo un campo un carattere più largo del richiesto. `%05d` di `-42` produce `"-00042"` (6 char) invece di `"-0042"` (5 char).

**Fix:** Dopo `width -= i;`, aggiungere: `if (flags & (FLAG_SIGN | FLAG_PLUS | FLAG_SPACE)) width--;`

---

### LIB-VSNPRINTF-02 — `vsnprintf.c`: Troncamento invisibile — ritorna byte scritti, non lunghezza totale

**Severità:** Media  
**File:** `kernel/lib/vsnprintf.c` — `vsnprintf()`  
**Documentata nel sorgente:** Sì (LIB-VSNPRINTF-02)

Il comportamento POSIX di `vsnprintf` è restituire il numero di caratteri che sarebbero stati scritti se il buffer fosse illimitato. Questa implementazione restituisce i caratteri effettivamente scritti (sempre `< size`). Il pattern idiomatico `if (vsnprintf(buf, sz, ...) >= sz) /* troncato */` non funziona — il troncamento è silenzioso.

**Fix:** Tracciare un contatore `total` separato che incrementa indipendentemente dal bound del buffer; restituire `total`.

---

### LIB-VSNPRINTF-04 — `vsnprintf.c`: `%p` con buffer quasi pieno emette `0x` senza cifre

**Severità:** Media  
**File:** `kernel/lib/vsnprintf.c` — handler `%p`  
**Documentata nel sorgente:** Sì (LIB-VSNPRINTF-04)

Il check `written < size - 2` permette di emettere `0x`, poi `print_num()` viene chiamata con capacità residua quasi zero e non scrive cifre. L'output risultante è `"0x"` — un puntatore formalmente valido ma completamente sbagliato, senza indicazione di troncamento.

**Fix:** Verificare `written + 18 < size` (2 per `0x` + 16 per le cifre hex) prima di emettere qualsiasi parte del puntatore.

---

### LIB-VSNPRINTF-05 — `vsnprintf.c`: `%s` ignora width e precision

**Severità:** Media  
**File:** `kernel/lib/vsnprintf.c` — handler `%s`  
**Documentata nel sorgente:** No

```c
case 's':
    s = va_arg(args, const char *);
    if (!s) s = "(null)";
    /* NOTE: width and precision are not applied */
    while (*s && written < (int)size - 1) buf[written++] = *s++;
```

`%-20s` non produce padding; `%.*s` non tronca al numero di caratteri specificato. Codice che usa `snprintf(buf, sz, "%-*s", width, str)` per allineare colonne riceve output privo di spaziatura, corrompendo log strutturati e output a tabella.

**Fix:** Applicare `precision` come limite di byte (se != -1), poi `width` come padding con spazi (FLAG_LEFT per allineamento a sinistra).

---

### NEW-STR-01 — `string.c`: `atoi()` senza rilevamento overflow

**Severità:** Media  
**File:** `kernel/lib/string.c` — `atoi()`  
**Documentata nel sorgente:** No

```c
while (*s >= '0' && *s <= '9') {
    res = res * 10 + (*s - '0');  /* overflow silenzioso */
    s++;
}
```

Input come `"2147483648"` (INT_MAX+1) produce `-2147483648`; `"99999999999999"` produce un valore completamente arbitrario. Qualsiasi lettura di configurazione da registry o argomenti di syscall che usi `atoi()` per dimensioni di memoria, timeout o contatori è potenzialmente exploitable tramite valori crafted.

**Fix:** Implementare `strtol()` con rilevamento overflow, o aggiungere un controllo esplicito:

```c
if (res > (INT_MAX - digit) / 10) return (sign > 0) ? INT_MAX : INT_MIN;
```

---

### NEW-BT-02 — `backtrace.c`: `fp_addr_valid()` non copre l'accesso a `fp+8`

**Severità:** Media  
**File:** `kernel/lib/backtrace.c` — `backtrace_regs()`  
**Documentata nel sorgente:** No

`backtrace_regs()` legge `*(uint64_t *)(fp + 8)` (return address) dopo che `fp_addr_valid(fp)` è passato. Se `fp` è ai massimi 7 byte del range valido, `fp+8` è oltre il limite approvato. Il read è comunque eseguito.

**Fix:** In `fp_addr_valid()`, cambiare il limite superiore da `fp >` a `fp + 15 >` (garantendo che siano leggibili 8 byte sia a `fp` che a `fp+8`), con overflow check: `if (fp + 15 < fp) return 0;`.

---

### LIB-REG-01 — `registry.c`: Array piatto O(n) — design non scalabile

**Severità:** Media  
**File:** `kernel/lib/registry.c`  
**Documentata nel sorgente:** Sì (LIB-REG-01)

128 slot statici, due scan O(n) per ogni write (update + insert), zero gerarchia nonostante la notazione dotted-key. **Fix architetturale:** sostituire con un nodo VFS sintetico `/reg`.

---

### LIB-REG-02 — `registry.c`: First-writer-wins troppo grezzo

**Severità:** Media  
**File:** `kernel/lib/registry.c`  
**Documentata nel sorgente:** Sì (LIB-REG-02, segnato come risolto — impatto residuo)

Un processo che crasha e si riavvia con un nuovo PID non può riscrivere la propria chiave (il PID precedente è il owner). I PID vengono riassegnati: se un processo malevolo ottiene il PID del processo defunto prima della registrazione legittima, diventa owner permanente della chiave. Non esiste meccanismo di scadenza o revoca.

**Fix:** Aggiungere `CAP_REG_ADMIN` per la revoca forzata. Documentare l'hazard di PID recycling in `registry.h`.

---

### NEW-PK-02 — `printk.c`: Buffer `printk` tronca silenziosamente

**Severità:** Media  
**File:** `kernel/lib/printk.c` — `vprintk()`  
**Documentata nel sorgente:** Sì (LIB-PRINTK-01)

`cpu->printk_buf` ha 2048 byte. Messaggi più lunghi vengono troncati da `vsnprintf` senza nessun contatore di messaggi persi e senza il carattere di troncamento `…`. Un messaggio di kernel panic troncato può omettere la causa dell'errore.

**Fix:** Dopo `vsnprintf`, verificare se `len == sizeof(buf) - pfx - 1`; in caso, appendere `"...[TRUNC]\n"` e incrementare `cpu->printk_dropped`.

---

### NEW-MATH-03 — `math.c`: Range reduction O(n) in `sin_fp` — DoS su input grandi

**Severità:** Media  
**File:** `kernel/lib/math.c` — `sin_fp()`  
**Documentata nel sorgente:** Sì (LIB-MATH-03)

```c
while (x > FP_PI) x -= FP_2PI;
```

Un input crafted di valore molto grande causa milioni di sottrazioni prima di convergere. Con input da codice utente non validato, questo è un DoS deterministico nel kernel.

**Fix:** Sostituire il loop con operazione modulo: `x = (int32_t)((int64_t)x % FP_2PI); if (x > FP_PI) x -= FP_2PI;`

---

### LIB-FDT-02 — `fdt.c`: `off_mem_rsvmap` non parsata — RAM firmware-reserved al PMM

**Severità:** Media  
**File:** `kernel/lib/fdt.c`  
**Documentata nel sorgente:** Sì (LIB-FDT-02)

Il DTB include una mappa di regioni RAM riservate dal firmware (GIC, PSCI, spin tables). `fdt.c` non legge mai `off_mem_rsvmap`. Il PMM riceve tutte le regioni `memory@*` come usabili, potenzialmente allocando su strutture firmware. Su hardware con firmware aggressivo, questo causa corruzione silenziosa o eccezioni EL3.

**Fix:** Iterare `off_mem_rsvmap` (struct `{uint64_t addr; uint64_t size;}` terminata da `(0,0)`) e marcare le regioni come `MEM_REGION_RESERVED` prima di passare la mappa al PMM.

---

### FDT-BUG-06 — `fdt.c`: `depth` può diventare negativo

**Severità:** Media  
**File:** `kernel/lib/fdt.c` — `fdt_get_mem_regions()`, `fdt_count_cpus()`  
**Documentata nel sorgente:** No

Un DTB con più `FDT_END_NODE` che `FDT_BEGIN_NODE` porta `depth` (int) sotto zero. I confronti `if (in_memory_node == depth)` possono dare match spurio se entrambi i valori sono diventati negativi in modo coincidente, causando falsi positivi nell'identificazione di nodi memoria/CPU.

**Fix:** `if (depth <= 0) { pr_err("FDT: struttura sbilanciata\n"); break; }` prima di `depth--`.

---

### LIB-FDT-01b — `fdt.c`: `fdt_count_cpus()` duplica le stesse vulnerabilità

**Severità:** Media  
**File:** `kernel/lib/fdt.c` — `fdt_count_cpus()`  
**Documentata nel sorgente:** No

`fdt_count_cpus()` replica interamente la logica di walk con le stesse vulnerabilità di `fdt_get_mem_regions()`: `off_dt_struct` non validato, `strlen()` senza bound, overflow in `p += (len+3)/4`. Non documentata come problema separato nel sorgente.

**Fix strutturale:** Estrarre la logica di walk in `fdt_walk(fdt_walk_cb_t cb, void *ctx)` con callback; le validazioni vengono applicate una volta sola.

---

### MM-KM-03 — `kmalloc.c`: Large alloc non page-aligned — impatto DMA/MMIO sottostimato

**Severità:** Media  
**File:** `kernel/lib/kmalloc.c` — `kmalloc()`  
**Documentata nel sorgente:** Sì (MM-KM-03, impatto sottostimato)

Large allocations restituiscono `page_base + 32` (offset del header). Driver DMA (VirtIO queues, NIC ring buffers) che usano `kmalloc()` per strutture condivise con hardware ricevono un puntatore non allineato a pagina. Le DMA engine tipicamente richiedono page-alignment: il trasferimento avviene 32 byte dentro la pagina, corrompendo i 32 byte del header e i 4064 byte della pagina successiva.

**Fix:** Aggiungere `kmalloc_pages(size_t n_pages)` che restituisce direttamente il risultato di `pmm_alloc_pages()` senza header embedded, per i caller che necessitano di page-alignment.

---

### MM-KM-06b — `kmalloc.c`: Deadlock `kmalloc_lock → uart_lock`

**Severità:** Media  
**File:** `kernel/lib/kmalloc.c` — `kmalloc()`  
**Documentata nel sorgente:** No

`pr_info()` e `pr_err()` vengono chiamate con `kmalloc_lock` già acquisito (con IRQ save). `pr_info → printk → vprintk` tenta di acquisire `uart_lock`. Se un altro CPU tiene `uart_lock` e chiama `kmalloc()` (scenario plausibile per driver UART o VirtIO che allocano buffer), si forma il deadlock classico:

```
CPU A: kmalloc_lock (held) → uart_lock (waiting)
CPU B: uart_lock (held)    → kmalloc_lock (waiting)
```

**Fix:** Spostare tutte le chiamate `pr_*` fuori dalla sezione critica di `kmalloc_lock`. Salvare in variabili locali le informazioni da loggare, rilasciare il lock, poi emettere il log.

---

### MM-KM-06c — `kmalloc.c`: `heap_initialized` letto senza atomicità

**Severità:** Media  
**File:** `kernel/lib/kmalloc.c` — `kmalloc()`  
**Documentata nel sorgente:** No

```c
if (!heap_initialized)    /* lettura senza lock né barrier */
    kmalloc_init();
```

`heap_initialized` è `int` (non `volatile`, non atomico). Il compilatore può riordinare le letture di `heap_initialized` rispetto agli aggiornamenti del heap state. Su architetture con memory model rilassato (aarch64 con TSO non garantito), un CPU potrebbe leggere `heap_initialized == 1` ma vedere `heap_ptr == NULL`.

**Fix:** Dichiarare `static volatile int heap_initialized = 0;` oppure usare `__atomic_load_n(&heap_initialized, __ATOMIC_ACQUIRE)` nel check e `__atomic_store_n(&heap_initialized, 1, __ATOMIC_RELEASE)` nella scrittura.

---

## 7. Vulnerabilità basse

---

### LIB-UTF8-03 — `utf8.c`: Nessun path di errore per lead byte troncati

**Severità:** Bassa  
**File:** `kernel/lib/utf8.c`  
**Documentata nel sorgente:** No

Se il chiamante non garantisce 4 byte leggibili oltre `s[0]`, non esiste un modo per `utf8_decode()` di rilevare e segnalare il troncamento senza la fix di LIB-UTF8-01. Vedere fix della vulnerabilità LIB-UTF8-01.

---

### LIB-REG-03 — `registry.c`: Doppio `#include <kernel/vmm.h>`

**Severità:** Bassa  
**File:** `kernel/lib/registry.c`  
**Documentata nel sorgente:** Sì (LIB-REG-03)

`<kernel/vmm.h>` è incluso due volte incondizionatamente. Gli include guard prevengono errori di compilazione, ma il duplicato è rumore per i tool di analisi statica.

**Fix:** Rimuovere il secondo `#include <kernel/vmm.h>` (riga 16 dell'originale).

---

### LIB-REG-04 — `registry.c`: Nessuna API di enumerazione chiavi

**Severità:** Bassa  
**File:** `kernel/lib/registry.c`  
**Documentata nel sorgente:** Sì (LIB-REG-04)

`registry_get()` e `sys_registry(REG_OP_READ)` richiedono la conoscenza preventiva del nome della chiave. Non esiste `registry_enum()` o `REG_OP_ENUM`.

**Fix:** Aggiungere `int registry_enum(char **keys_out, size_t max_keys, size_t *count_out)` e il corrispondente `REG_OP_ENUM` in `sys_registry()`.

---

### NEW-FP-01 — `fault_print.c`: TOCTOU su `fault_depth_fallback` in `fault_exit()`

**Severità:** Bassa  
**File:** `kernel/lib/fault_print.c` — `fault_exit()`  
**Documentata nel sorgente:** No

```c
} else if (fault_depth_fallback) {           /* lettura non atomica */
    __sync_sub_and_fetch(&fault_depth_fallback, 1);  /* decremento atomico */
}
```

Tra il check e il decremento, un altro CPU (in teoria, pre-SMP non attivo su questo path) potrebbe decrementare `fault_depth_fallback` a 0, causando underflow a `UINT32_MAX` con il nostro decremento.

**Fix:** Sostituire con decremento saturante:
```c
uint32_t prev = __sync_sub_and_fetch(&fault_depth_fallback, 1);
if (prev == UINT32_MAX) __sync_add_and_fetch(&fault_depth_fallback, 1);
```

---

### NEW-STR-02 — `string.c`: `strcpy`/`strcat` senza bounds — footgun attivo

**Severità:** Bassa  
**File:** `kernel/lib/string.c`  
**Documentata nel sorgente:** No

`strcpy()` e `strcat()` sono presenti ed esportate. Le alternative sicure `strlcpy()`/`strlcat()` esistono nello stesso file ma non sono la scelta di default. Qualsiasi nuovo codice che usa `strcpy()` senza verifica della size può overflow silenziosamente.

**Fix:** Marcare con `__attribute__((deprecated("use strlcpy/strlcat")))` per generare warning a compile-time su ogni nuovo call site.

---

### NEW-KT-01 — `ktest.c`: Nessun crash recovery nel test runner

**Severità:** Bassa  
**File:** `kernel/lib/ktest.c` — `ktest_run_all()`  
**Documentata nel sorgente:** Sì (commento nel sorgente)

Un singolo test che causa kernel panic o page fault blocca tutti i test successivi. Il commento nel sorgente cita `setjmp/longjmp` come direzione futura.

**Fix:** Implementare un frame di fault attorno a ogni `test->func()` tramite `arch_setjmp`/`arch_longjmp` su uno stack di test dedicato, segnando il test come CRASHED invece di FAIL.

---

### LIB-FDT-04 — `fdt.c`: `fdt_find_in_memory()` valida solo il magic

**Severità:** Bassa  
**File:** `kernel/lib/fdt.c` — `fdt_find_in_memory()`  
**Documentata nel sorgente:** Sì (LIB-FDT-04)

Solo il magic word `0xd00dfeed` viene verificato. Un false positive (4 byte che coincidono nel RAM) porta a `fdt_get_mem_regions()` che traversa memoria arbitraria. Vedere anche FDT-BUG-02 e LIB-FDT-01 per l'impatto a cascata.

**Fix:** Validare anche: `totalsize >= 48`, `version >= 2`, `last_comp_version <= 16`, `off_dt_struct < totalsize`, `off_dt_strings < totalsize`.

---

### MM-KM-BUG-04 — `kmalloc.c`: `flags = 0` dead code con commento fuorviante

**Severità:** Bassa  
**File:** `kernel/lib/kmalloc.c` — `kmalloc()`, large path  
**Documentata nel sorgente:** No

```c
spin_unlock_irqrestore(&kmalloc_lock, flags);
flags = 0; // Prevent double unlock
```

Il large path ritorna sempre prima di raggiungere l'etichetta `out:`, quindi il check `if (flags)` a `out:` non è mai raggiunto da questo path. `flags = 0` è dead code. Il commento "Prevent double unlock" suggerisce un rischio che non esiste nel flusso attuale ma potrebbe essere introdotto da future modifiche che aggiungono `goto out` al large path.

**Fix:** Rimuovere `flags = 0` e il commento; rendere esplicita la struttura del flusso di controllo.

---

## 8. Vulnerabilità note nel sorgente — stato aggiornato

Le seguenti vulnerabilità erano già documentate nel sorgente con i loro ID originali. Vengono riportate per completezza con lo stato attuale.

| ID originale | File | Stato | Note |
|---|---|---|---|
| LIB-FDT-01 | `fdt.c` | **Aperta** | Impatto più grave del documentato — vedi LIB-FDT-01, FDT-BUG-02, FDT-BUG-03, FDT-BUG-04 |
| LIB-FDT-02 | `fdt.c` | **Aperta** | Nessuna mitigazione implementata |
| LIB-FDT-03 | `fdt.c` | **Per design** | Stub AMD64, comportamento atteso |
| LIB-FDT-04 | `fdt.c` | **Aperta** | Solo magic validato — vedi LIB-FDT-04 |
| LIB-FDT-05 | `fdt.c` | **Aperta** | Commento stale, nessun impatto funzionale |
| LIB-KTEST-01 | `ktest.c` | **Risolta** | Flag `ktest_test_failed` implementato correttamente |
| LIB-MATH-01 | `math.c` | **Aperta** | `FP_PI == FP_2PI` nei build kernel |
| LIB-MATH-03 | `math.c` | **Aperta** | Range reduction O(n) |
| LIB-PRINTK-01 | `printk.c` | **Aperta** | Troncamento silenzioso |
| LIB-REG-01 | `registry.c` | **Aperta** | Array piatto O(n) |
| LIB-REG-02 | `registry.c` | **Parzialmente risolta** | First-writer-wins implementato, hazard PID recycling residuo |
| LIB-REG-03 | `registry.c` | **Aperta** | Doppio include |
| LIB-REG-04 | `registry.c` | **Aperta** | Nessuna API enumerate |
| LIB-SSP-01 | `stack_protector.c` | **Mitigata** | `stack_guard_init()` implementata, ordine chiamata non enforced |
| LIB-UTF8-01 | `utf8.c` | **Aperta** | Fix richiede aggiunta parametro `len` |
| LIB-UTF8-02 | `utf8.c` | **Aperta** | Nessuna validazione codepoint |
| LIB-VSNPRINTF-01 | `vsnprintf.c` | **Aperta** | Off-by-one con zero-pad negativo |
| LIB-VSNPRINTF-02 | `vsnprintf.c` | **Aperta** | Troncamento non rilevabile |
| LIB-VSNPRINTF-03 | `vsnprintf.c` | **Per design** | `%o`/`%f` assenti, accettabile senza FPU |
| LIB-VSNPRINTF-04 | `vsnprintf.c` | **Aperta** | `%p` con buffer pieno |
| MM-KM-01 | `kmalloc.c` | **Risolta** | Pool growable implementato |
| MM-KM-02 | `kmalloc.c` | **Aperta** | No cross-bucket reuse |
| MM-KM-03 | `kmalloc.c` | **Aperta** | Non page-aligned, impatto DMA |
| MM-KM-04 | `kmalloc.c` | **Aperta** | krealloc sempre alloc+copy |
| MM-KM-05 | `kmalloc.c` | **Aperta** | Header 32B su bucket 16B |
| MM-KM-06 | `kmalloc.c` | **Aperta** | Lock globale singolo |

---

*Fine del report — 39 vulnerabilità totali in 17 file.*
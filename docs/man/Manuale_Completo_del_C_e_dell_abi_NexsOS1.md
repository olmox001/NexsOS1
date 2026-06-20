# Manuale Completo del C e dell'ABI OS1/NEXS

> **Per chi è questo manuale**
> Questo testo ha **tre livelli**:
> 1. **Parte I — C per chi non ha mai programmato**. Capitoli lenti, graduali, con esempi banali. Non serve alcuna conoscenza precedente.
> 2. **Parte II — C per OS1**. Le stesse idee del C “standard”, ma viste con l’ottica di un sistema reale: memoria, file, processi. Qui si assume che tu sappia già scrivere un programma C di piccole dimensioni.
> 3. **Parte III — ABI di OS1**. Il riferimento completo: numeri di syscall, convenzioni di chiamata, strutture dati, esempi commentati di ogni famiglia di primitive. Pensato come documentazione di lavoro, non come testo da leggere in ordine.
>
> Tutto il codice è testato contro l’ABI esposta in `include/api/` del bundle. Architetture supportate: **AArch64** e **amd64** (x86-64). Il riferimento “ufficiale” è AArch64.

---

# PARTE I — C per chi parte da zero

## Capitolo 1 · Cos’è un programma, cos’è un sistema operativo

### 1.1 Un programma è una sequenza di istruzioni

Un computer è stupido. Sa fare solo pochissime cose: spostare numeri da una cella di memoria a un’altra, sommarli, confrontarli, saltare a un’altra istruzione in base al risultato di un confronto. Tutto il resto — la grafica, il suono, Internet — è una torre di istruzioni elementari impilate una sull’altra.

Un **programma** è un file di testo (il *sorgente*) che descrive questa torre. Un altro programma, il **compilatore**, lo traduce in un file di numeri (il *binario*) che la CPU è in grado di eseguire. Su OS1, il formato di quei numeri si chiama **ELF64** (Executable and Linkable Format, 64 bit).

```
sorgente.c   →  [ compilatore ]  →  programma.elf   →  [ caricato in RAM ]  →  CPU lo esegue
   (testo)         (gcc, clang)        (numeri)            (dal kernel)         (istruzioni)
```

### 1.2 Cos’è un sistema operativo

Il **kernel** è il primo programma che viene caricato all’accensione. Il suo lavoro è:
- gestire la memoria (chi può usare quali celle);
- gestire la CPU (quale programma gira adesso, per quanto tempo);
- gestire i dispositivi (tastiera, mouse, disco, scheda grafica);
- gestire la sicurezza (chi può fare cosa);
- offrire a te, programmatore, delle **chiamate di sistema** (*system call*, abbreviato *syscall*): funzioni che il kernel espone ai programmi per chiedergli di fare queste cose al posto tuo.

Tutto il resto — la shell, l’editor di testo, i videogiochi — sono **programmi userland**, cioè programmi “normali” che girano sopra il kernel e gli chiedono le cose tramite le syscall.

OS1/NEXS è un kernel **ibrido** (parte microkernel, parte monolitico) che supporta AArch64 e x86-64, SMP, VirtIO, Ext4. La cosa importante, per te che inizi, è questa: **per scrivere un programma userland su OS1 non ti serve sapere come funziona il kernel dentro**. Ti serve solo conoscere le **syscall** che il kernel espone, e il piccolo strato di libreria (`libc`) che le rende comode da chiamare.

### 1.3 La toolchain

Per trasformare un sorgente `.c` in un programma eseguibile servono:
- un **compilatore C** per la tua architettura (`aarch64-elf-gcc` oppure `x86_64-elf-gcc`);
- **QEMU** (`qemu-system-aarch64` o `qemu-system-x86_64`) per provarlo senza hardware reale;
- il **bundle OS1** (kernel, bootloader, immagine disco Ext4, script di build).

Su macOS, nello spazio di lavoro del progetto NexsOS1 (scaricabile via github):
```bash
./tools/setup-toolchain.sh
```

Su Debian/Ubuntu servono pacchetti equivalenti;
```bash
./tools/setup-toolchain-linux.sh
```
per testare una volta installata la toolchain ufficilae sarà sufficente il comando: 

(per testare il sistema in modalità aarch64):
```bash
make run
```

(per testare il sistema in modalità amd64):
```bash
make run ARCH=amd64
```
il comando ARCH="" permette di scegliere l architettura da buildare e testare, di default è aarch64

### 1.4 Cosa faremo in questa Parte I

Ti insegnerò il C procedendo per gradi: tipi di dato, variabili, funzioni, array, puntatori, memoria, strutture. Alla fine di ogni capitolo c’è un esercizio. **Non saltare gli esercizi**: la programmazione si impara scrivendo, non leggendo.

---

## Capitolo 2 · Il tuo primo programma

### 2.1 `hello.c` (possiamo modificare quello: "/user/bin/hello.c" o crearne uno nuovo con nome diverso ma richiede modifiche al makefile)

modifica il file `hello.c` con questo contenuto:

```c
#include <os1.h>

int main(void) {
    print("Ciao, OS1!\n");
    return 0;
}
```

`#include <os1.h>` dice al compilatore: “voglio usare le funzioni dichiarate nell’header `os1.h`”, che è il file che elenca tutta l’ABI di OS1 (vedi Parte III). Senza di esso il compilatore non saprebbe cosa sia `print`.

`int main(void)` è la **funzione principale**: il punto da cui il programma parte. Tutto ciò che il programma fa è scritto nel *corpo* della funzione, tra `{` e `}`.

`print("Ciao, OS1!\n");` è un’**istruzione**: chiede al sistema di scrivere una stringa sullo schermo. Il `;` finale dice “ho finito questa istruzione”.

`return 0;` conclude `main`. Per convenzione, `0` significa “tutto ok, niente errori”.

### 2.2 Compilare

Da terminale (o usando il comando "make run" che effettua la compilazione, link e avvia il test in automatico):

(salta con make run)
```bash
# AArch64
aarch64-elf-gcc -std=c99 -ffreestanding -nostdlib -I include/api \
    -o hello.elf hello.c -T user/arch/aarch64/link.ld

# x86-64
x86_64-elf-gcc -std=c99 -ffreestanding -nostdlib -I include/api \
    -o hello.elf hello.c -T user/arch/amd64/link.ld
```

`-I include/api` dice al compilatore dove trovare gli header. `-T ...link.ld` indica la *linker script*, che descrive come deve essere organizzato il file ELF in memoria.

> **Nota.** OS1 fornisce anche `lib.o` (in `user/sys/lib/`), una libreria precompilata con `libc`, `malloc`, `printf`, `font_lib`, e un piccolo `crt0` (`syscall.S`). Per un “Hello world” non ti serve: il programma sopra è *bare metal* e usa solo la syscall `write` tramite la funzione `print`. Per programmi realistici aggiungi `user/sys/lib/lib.o` e gli stub di `syscall.S` al link, vedi Parte II.

### 2.3 Caricare sull’immagine disco e avviare 

L’immagine disco di OS1 è un file Ext4. I programmi vanno messi in `/bin/` o `/sys/bin/` , il makefile se ne occupa in automatico quando eseguiamo con make run, per aggiungere altri file oltre ad hello.c vanno aggiunti i targhet, puoi cercare "hello" nella barra ricerca del tuoi ide e guardare le 2 implementazioni nel makefile che devi aggiungere per compilare in automatico i file .c che desideri:

```bash
make run ARCH=aarch64
```

Dalla shell di OS1, digiti:

```
shell:/> hello
Ciao, OS1!
shell:/>
```

### 2.4 Anatomia di un’istruzione C

```c
print("Ciao, OS1!\n");
│     │              │  │
│     │              │  └─ ; termina l’istruzione
│     │              └──── \n è il carattere “a capo” (newline)
│     └─────────────────── "Ciao, OS1!\n" è una stringa letterale
└───────────────────────── print è il nome della funzione chiamata
```

Una **stringa letterale** in C è una sequenza di caratteri racchiusa tra doppi apici. Le lettere speciali si scrivono con la barra rovesciata:
- `\n` = a capo
- `\t` = tabulazione
- `\\` = una singola barra
- `\"` = un doppio apice dentro la stringa

### 2.5 Esercizio

Modifica `hello.c` per stampare il tuo nome, su due righe diverse.

---

## Capitolo 3 · Variabili, tipi, espressioni

### 3.1 Cos’è una variabile

Una variabile è una cella di memoria con un nome. Il programma può scriverci dentro e leggere il valore attuale.

```c
int eta = 25;          // un intero
float altezza = 1.78;  // un numero con la virgola (32 bit)
double pigreco = 3.14; // un numero con la virgola (64 bit)
char iniziale = 'M';   // un singolo carattere
```

`int`, `float`, `double`, `char` sono **tipi** primitivi. Servono a dire al compilatore quanta memoria serve e come interpretarla.

### 3.2 I tipi interi su OS1

Su OS1 (sia AArch64 che x86-64) `int` è **32 bit**, `long` è **64 bit**, `long long` è **64 bit**, `short` è **16 bit**, `char` è **8 bit**. Gli header `<stdint.h>` forniscono tipi a dimensione fissa, utili quando ti serve esattamente quel numero di bit:

```c
#include <stdint.h>

uint8_t  un_byte;        // 0..255
int8_t   con_segno;      // -128..127
uint16_t due_byte;       // 0..65535
uint32_t quattro_byte;   // 0..(2^32 - 1)
int64_t  otto_byte_con_segno;
size_t   dimensione;     // grande quanto un puntatore (64 bit qui)
```

`size_t` è il tipo usato per tutte le dimensioni; è *unsigned* (sempre positivo).

### 3.3 I tipi definiti da OS1

L’header `posix_types.h` (incluso da `os1.h`) definisce:

```c
typedef int32_t pid_t;     // identificatore di processo
typedef int64_t ssize_t;   // dimensione con segno (letture/scritture)
typedef int64_t off_t;     // offset in un file
typedef int64_t time_t;    // tempo in secondi
typedef uint32_t mode_t;   // permessi file
typedef uint32_t uid_t, gid_t;
```

### 3.4 Espressioni

Una **espressione** è un calcolo che produce un valore.

```c
int a = 10;
int b = 3;
int somma = a + b;        // 13
int differenza = a - b;   // 7
int prodotto = a * b;     // 30
int quoziente = a / b;    // 3 (divisione intera!)
int resto = a % b;        // 1 (modulo)

a++;     // a diventa 11 (post-incremento)
a--;     // a torna 10
++a;     // a diventa 11 (pre-incremento)
b += 2;  // b diventa 5 (abbreviazione di: b = b + 2)
a *= 2;  // a diventa 20
```

### 3.5 Confronto e logica

```c
int x = 5;
int y = 7;

x == y    // falso (0)
x != y    // vero (1)
x < y     // vero
x >= y    // falso
!x        // vero (not logico: 0 diventa 1, qualunque non-zero diventa 0)
x && y    // vero (and logico)
x || y    // vero (or logico)
```

In C, “vero” è *qualsiasi valore non zero*, “falso” è *esattamente zero*. È una vecchia convenzione; non ti spaventare.

### 3.6 Esercizio

Scrivi un programma che calcola l’area di un rettangolo date base e altezza, e la stampa.

---

## Capitolo 4 · Controllo di flusso

### 4.1 `if`/`else`

```c
if (condizione) {
    /* fai questo se condizione è vera */
} else if (altra_condizione) {
    /* fai questo se la prima era falsa e questa è vera */
} else {
    /* fai questo se tutte le precedenti erano false */
}
```

Esempio per distinguere valori negativi da positivi:

```c
int n = 17;
if (n < 0) {
    print("negativo\n");
} else if (n == 0) {
    print("zero\n");
} else {
    print("positivo\n");
}
```


### 4.2 `while`

Ripete un blocco finché la condizione è vera:

```c
int i = 0;
while (i < 5) {
    printf("i = %d\n", i);
    i++;
}
```

### 4.3 `do/while`

Come `while`, ma il corpo è eseguito almeno una volta:

```c
int scelta;
do {
    print("1) continua  0) esci\n");
    scelta = getchar();
} while (scelta != '0');
```

### 4.4 `for`

È un `while` con contatore, in forma compatta:

```c
for (inizializzazione; condizione; passo) {
    corpo;
}
```

Esempio (stampa i numeri da 1 a 10):

```c
for (int i = 1; i <= 10; i++) {
    printf("%d\n", i);
}
```

### 4.5 `break` e `continue`

- `break` esce dal ciclo immediatamente.
- `continue` salta il resto del corpo e ricomincia dalla prossima iterazione.

### 4.6 `switch`

Utile quando confronti una variabile con molti valori:

```c
int cmd = getchar();
switch (cmd) {
case 'h':
    print("help\n");
    break;
case 'q':
    print("quit\n");
    break;
default:
    print("comando sconosciuto\n");
}
```

`break` è obbligatorio per uscire; altrimenti l’esecuzione “cade” nel caso successivo.

### 4.7 Esercizio

Scrivi un programma che stampa i primi 20 numeri della sequenza di Fibonacci, partendo da 0 e 1.

---

## Capitolo 5 · Funzioni

Una funzione è un blocco di codice con un nome. Puoi *chiamarla* ogni volta che ti serve.

```c
/* Definizione: somma due interi e restituisce il risultato. */
int somma(int a, int b) {
    return a + b;
}

/* Funzione senza valore di ritorno: stampa una riga. */
void saluta(const char *nome) {
    printf("Ciao, %s!\n", nome);
}

int main(void) {
    saluta("OS1");
    int r = somma(3, 4);
    printf("3 + 4 = %d\n", r);
    return 0;
}
```

- Il **tipo di ritorno** (`int`, `void`, …) è dichiarato prima del nome.
- I **parametri** sono elencati tra parentesi.
- `return` restituisce un valore al chiamante.
- `void` significa “niente” (né parametri né valore di ritorno).

Una funzione deve essere **dichiarata** (con un prototipo) *prima* di essere chiamata, oppure definita prima. Il modo più pulito è: prototipo in cima al file, definizione dopo `main`.

```c
int fattoriale(int n);   /* prototipo */

int main(void) {
    printf("%d\n", fattoriale(5));   /* 120 */
    return 0;
}

int fattoriale(int n) {              /* definizione */
    int r = 1;
    for (int i = 2; i <= n; i++) r *= i;
    return r;
}
```

### 5.1 Visibilità delle variabili

Le variabili dichiarate dentro una funzione sono **locali**: esistono solo finché la funzione è in esecuzione. Quelle dichiarate fuori da ogni funzione sono **globali**: esistono per tutta la vita del programma.

```c
int globale = 10;          /* visibile ovunque */

void f(void) {
    int locale = 5;         /* visibile solo qui dentro */
    globale++;              /* ok, posso modificare la globale */
    {
        int blocco = 7;     /* visibile solo in questo blocco { } */
    }
    /* blocco non esiste più qui */
}
```

### 5.2 Esercizio

Scrivi una funzione `int potenza(int base, int esp)` che calcoli l’elevamento a potenza senza usare la funzione di libreria.

---

## Capitolo 6 · Array e stringhe

### 6.1 Array

Un array è una sequenza di variabili dello stesso tipo, indicizzate da un intero.

```c
int voti[5];                  /* 5 interi non inizializzati */
int primi[5] = {2, 3, 5, 7, 11};
voti[0] = 28;                 /* il primo elemento */
voti[4] = 30;                 /* l’ultimo */
int n = voti[2];              /* legge il terzo elemento */
```

Gli indici partono da **0** e finiscono a **dimensione − 1**. Su OS1, come in C in generale, accedere fuori dai limiti è un *undefined behavior*: può succedere di tutto, dal crash silenzioso alla corruzione della memoria.

### 6.2 Stringhe

Una stringa in C è un **array di `char` terminato da un byte zero** (`'\0'`). Le stringhe letterali come `"ciao"` sono gestite dal compilatore come array costanti, ai quali viene aggiunto il terminatore.

```c
char saluto[6] = "ciao";   /* 5 caratteri + terminatore \0 */
char parola[]  = "ciao";   /* il compilatore dimensiona a 6 */

printf("%s\n", saluto);    /* stampa la stringa */
printf("%c\n", saluto[0]); /* stampa un singolo carattere: 'c' */
```

Il pacchetto `<string.h>` (incluso da `<os1.h>`) offre funzioni utili:

```c
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
void  *memset(void *s, int c, size_t n);
void  *memcpy(void *dest, const void *src, size_t n);
```

### 6.3 Stampare caratteri di escape

```c
print("prima riga\nseconda riga\n");
print("un \"apice\" dentro la stringa\n");
print("c:\\path\\to\\file\n");
```

### 6.4 Esercizio

Scrivi una funzione `int conta_vocali(const char *s)` che conta quante vocali ci sono in una stringa.

---

## Capitolo 7 · Puntatori

Questo è il capitolo più importante del C. Prenditi tempo.

### 7.1 Cos’è un puntatore

Una variabile vive in una cella di memoria. Ogni cella ha un **indirizzo** (un numero). Un **puntatore** è una variabile che contiene un indirizzo.

```c
int x = 42;
int *p = &x;       /* p contiene l’indirizzo di x */
printf("%d\n", x); /* 42 */
printf("%p\n", p); /* stampa l’indirizzo (es. 0x1000) */
printf("%d\n", *p); /* 42: *p "dereferenzia" il puntatore */
*p = 100;          /* scrive 100 nella cella puntata da p */
printf("%d\n", x); /* adesso x è 100 */
```

- `&x` = “indirizzo di x”;
- `int *p` = “puntatore a un intero”;
- `*p` = “il valore della cella a cui p punta”.

### 7.2 Puntatori e array

In C, il nome di un array **decade** in un puntatore al suo primo elemento. `a[i]` è zucchero sintattico per `*(a + i)`.

```c
int a[3] = {10, 20, 30};
int *p = a;            /* p punta ad a[0] */
printf("%d\n", p[1]);  /* 20: p[i] equivale ad a[i] */
p++;                   /* ora p punta ad a[1] */
printf("%d\n", *p);    /* 20 */
```

### 7.3 Puntatore a `void`

`void *` è un puntatore “a qualunque cosa”. Le funzioni di libreria come `memcpy` o `malloc` lo usano perché non vogliono vincolare il tipo.

```c
void *memcpy(void *dest, const void *src, size_t n);
```

### 7.4 `const`

```c
const int *p;      /* p punta a un intero che NON devi modificare */
int *const p;      /* p è un puntatore che NON puoi cambiare */
const int *const p; /* entrambe */
```

### 7.5 Puntatore a puntatore (una volta sola)

Lo userai per gli `argv` di `main`, e per il risultato di funzioni che vogliono scrivere dentro un puntatore del chiamante (es. `strtol` con `endptr`).

```c
int main(int argc, char **argv) {
    /* argv è un array di stringhe; argv[0] è il nome del programma */
    for (int i = 0; i < argc; i++) {
        print(argv[i]);
        print("\n");
    }
    return 0;
}
```

### 7.6 Esercizio

Scrivi una funzione `void scambia(int *a, int *b)` che scambia i valori di due variabili. Testala da `main`.

---

## Capitolo 8 · Strutture

Una **struttura** (`struct`) raggruppa più variabili sotto un unico nome.

```c
struct punto {
    int x;
    int y;
};

struct punto p = {10, 20};
p.x = 30;
p.y = 40;
printf("(%d, %d)\n", p.x, p.y);
```

### 8.1 `typedef`

Per evitare di scrivere `struct` ovunque, si fa:

```c
typedef struct {
    int x, y;
} Punto;

Punto origine = {0, 0};
```

### 8.2 Strutture e puntatori

Spesso userai strutture grandi, e vorrai passarle per puntatore per evitare di copiarle:

```c
typedef struct {
    char nome[32];
    int eta;
} Persona;

void stampa(const Persona *p) {
    printf("%s ha %d anni\n", p->nome, p->eta);
    /* -> è l’accesso al campo tramite puntatore */
}
```

OS1 definisce molte strutture in `posix_types.h` (es. `struct ipc_message`) e in header specifici. Imparerai a leggerle man mano.

### 8.3 Esercizio

Definisci una `struct Libro { char titolo[64]; char autore[32]; int anno; }` e scrivi una funzione che la stampa formattata.

---

## Capitolo 9 · Memoria dinamica (malloc/free)

Le variabili locali vivono nello **stack** e spariscono quando la funzione ritorna. Per ottenere memoria che vive finché vuoi tu, usa l’**heap**, tramite `malloc`.

```c
void *malloc(size_t size);          /* alloca size byte */
void *calloc(size_t n, size_t s);   /* alloca n*s byte, inizializzati a 0 */
void *realloc(void *p, size_t s);   /* ridimensiona */
void  free(void *p);                /* rilascia */
```

Esempio:

```c
int *v = malloc(10 * sizeof(int));
if (!v) {
    print("out of memory\n");
    return 1;
}

for (int i = 0; i < 10; i++) v[i] = i * i;

for (int i = 0; i < 10; i++) printf("%d ", v[i]);
print("\n");

free(v);
```

### 9.1 Regole d’oro

1. **Controlla sempre** il valore di ritorno di `malloc`: può essere `NULL`.
2. **Non leggere o scrivere** oltre la dimensione richiesta.
3. **Libera sempre** la memoria che non ti serve più.
4. **Non liberare** due volte lo stesso puntatore.
5. **Non usare** un puntatore dopo aver liberato la memoria (dangling pointer).

### 9.2 L’allocatore di OS1

L’`malloc` di OS1 (in `user/sys/lib/malloc.c`) è un first-fit con coalescing in avanti. Cresce il proprio heap con la syscall `sbrk`. **Non ha mmap**: per “allocare grosse regioni” si usa sempre `malloc` o `mmap` (che è uno shim sopra `malloc`).

### 9.3 Esercizio

Scrivi una funzione che prende un intero `n` da riga di comando, alloca un array di `n` float, lo riempie con valori a tua scelta, ne calcola la media, e libera tutto.

---

## Capitolo 10 · Header e linking

### 10.1 Header

Un header è un file `.h` con **dichiarazioni** (non definizioni):

```c
/* somma.h */
#ifndef SOMMA_H
#define SOMMA_H

int somma(int a, int b);

#endif
```

Il `#ifndef`/`#define`/`#endif` evita inclusioni multiple. Quando includi un header da più file, viene visto una volta sola.

### 10.2 Compilare più file

```bash
aarch64-elf-gcc -c -I include/api -o main.o main.c
aarch64-elf-gcc -c -I include/api -o somma.o somma.c
aarch64-elf-gcc -o programma.elf main.o somma.o
```

Il linker risolve i simboli (i nomi delle funzioni) tra i vari `.o` e produce l’ELF finale.

### 10.3 La libreria di OS1

OS1 fornisce `user/sys/lib/lib.o`: contiene `libc`, `malloc`, `printf`, `font_lib`, e un piccolo `crt0`. Per un programma realistico:

```bash
aarch64-elf-gcc -I include/api -c mio.c -o mio.o
aarch64-elf-ld -T user/arch/aarch64/link.ld \
    user/arch/aarch64/syscall.o \
    user/sys/lib/lib.o mio.o -o mio.elf
```

(Le dipendenze esatte dipendono dal Makefile del progetto; l’idea è che `syscall.o` contiene gli stub `_sys_*` e `lib.o` contiene le funzioni C che li chiamano.)

### 10.4 Esercizio

Spezza il programma dell’esercizio 9.3 in due file: `media.h`/`media.c` con la funzione `float media(float*, int)`, e `main.c` con `main`.

---

# PARTE II — C per OS1

In questa parte le nozioni del C che già conosci vengono applicate al sistema operativo reale: come si legge e scrive un file, come si lancia un processo, come si disegna una finestra, come si riceve input dalla tastiera. Tutti gli esempi sono compilabili ed eseguibili su OS1.

## Capitolo 11 · L’ecosistema OS1 in 5 minuti

### 11.1 I tre “strati” di un programma

```
+----------------------------------+
|        il tuo programma          |  ← tu scrivi qui
+----------------------------------+
|  lib.c (libc, malloc, printf…)   |  ← parte dell'OS1 bundle
+----------------------------------+
|  syscall.S (stub _sys_*)         |  ← parte dell'OS1 bundle
+----------------------------------+
|         kernel (NEXS)            |  ← non lo scrivi tu
+----------------------------------+
```

`lib.c` è una libreria C-like che fornisce `printf`, `malloc`, `fopen`, `strtol`, `strtok`, ecc. Le funzioni di `lib.c` non sono syscall: al massimo chiamano syscall tramite gli stub `_sys_*` di `syscall.S`. Per esempio, `printf` chiama `vsnprintf` (incluso da `kernel/lib/vsnprintf.c`), poi `write(1, buf, len)`, e `write` è un wrapper su `_sys_write`, che è un’istruzione `svc #0`/`syscall`.

### 11.2 I file di un programma minimo

Un programma userland minimo, completo, è:

```c
/* main.c */
#include <os1.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    print("Ciao da OS1!\n");
    return 0;
}
```

Con un Makefile:

```make
ARCH ?= aarch64
CC   := $(ARCH)-elf-gcc
LD   := $(ARCH)-elf-ld
INC  := -I include/api

CFLAGS  := -std=c99 -ffreestanding -Wall -Wextra -O2 $(INC)
LDFLAGS := -T user/arch/$(ARCH)/link.ld

LIBCRT  := user/arch/$(ARCH)/syscall.o user/sys/lib/lib.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

programma.elf: main.o
	$(LD) $(LDFLAGS) $(LIBCRT) main.o -o $@

clean:
	rm -f *.o programma.elf
```

> **Nota importante.** Nella pratica, il Makefile del progetto OS1 è più complesso (gestisce kernel, bootloader, immagine Ext4, ecc.). L’estratto sopra serve a *capire* cosa succede.

### 11.3 Cosa c’è dentro `os1.h`

`os1.h` (in `include/api/`) include a sua volta:
- `<posix_types.h>`: tipi base, codici di errore, struttura `ipc_message`.
- `<syscall_nums.h>`: i numeri delle syscall.
- `<caps.h>`: i livelli di privilegio e i bit di capability.
- `<stddef.h>`, `<stdarg.h>`, `<stdint.h>`: standard C.

Dichiara inoltre: funzioni di I/O, gestione processi, IPC, finestre, grafica, font, tempo, registry, heap, file.

---

## Capitolo 12 · I/O a basso livello: `read`, `write`, file descriptor

### 12.1 I tre descrittori standard

Un programma OS1 eredita dal kernel tre file descriptor aperti:
- `0` = **stdin**: ingresso caratteri dalla tastiera.
- `1` = **stdout**: uscita sulla propria finestra/compositor.
- `2` = **stderr**: alias di stdout (vedi Capitolo 16).

Qualsiasi altro file aperto con `open()` riceve un descrittore ≥ 3.

### 12.2 `read`/`write`

```c
long read(int fd, char *buf, unsigned long count);
long write(int fd, const char *buf, size_t count);
```

`write` restituisce il numero di byte scritti (o negativo su errore). `read` restituisce il numero di byte letti, `0` su EOF, negativo su errore.

Esempio:

```c
char buf[64];
long n = read(0, buf, sizeof(buf));
if (n > 0) {
    write(1, buf, n);
}
```

### 12.3 Modello di errore: `-errno`

Tutte le syscall di OS1 seguono la convenzione Linux: successo = `>= 0`, fallimento = negativo il valore di un codice di errore POSIX definito in `posix_types.h` (`-EINVAL`, `-ENOMEM`, `-EFAULT`, `-EBADF`, …).

```c
long r = write(1, "ciao", 4);
if (r < 0) {
    /* è capitato un errore; -r è uno dei codici in posix_types.h */
    printf("errore %ld\n", r);
}
```

### 12.4 Aprire, leggere, scrivere, chiudere un file

```c
#include <os1.h>
#include <fcntl.h>

int main(void) {
    int fd = open("/config.txt", O_RDONLY);
    if (fd < 0) {
        print("file non trovato\n");
        return 1;
    }

    char buf[256];
    long n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        print("errore di lettura\n");
        close(fd);
        return 1;
    }
    buf[n] = '\0';
    print(buf);

    close(fd);
    return 0;
}
```

`open` richiede il *path* assoluto (es. `/config.txt`); la directory corrente è tracciata per il processo ma il path di `open` vuole lo `/` iniziale.

### 12.5 Le funzioni ad alto livello

OS1 include una piccola `stdio` emulata (in `lib.c`): `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `fgets`, `fputs`, `fprintf`. Internamente, `fopen` chiama `file_read(path, NULL, 0, 0)` per fare il probe della dimensione.

```c
FILE *f = fopen("/config.txt", "r");
if (!f) { print("errore\n"); return 1; }
char buf[128];
fgets(buf, sizeof(buf), f);
fclose(f);
```

I `FILE*` per stdin/stdout/stderr sono i valori `(FILE*)0`, `(FILE*)1`, `(FILE*)2` (vedi `stdio.h`); `fread`/`fwrite` li instradano direttamente a `read`/`write` sullo fd.

### 12.6 Esercizio

Scrivi un programma che apre un file di testo, conta righe, parole, caratteri (come `wc`), e stampa i tre numeri.

---

## Capitolo 13 · Processi: spawn, exit, wait, segnali

### 13.1 Creare un processo

```c
int spawn(const char *path);
int spawn_args(const char *path, int argc, char *const argv[]);
long spawn_caps(const char *path, int level, unsigned long caps);
long spawn_level(const char *path, int level);
```

`spawn` lancia un ELF dal filesystem e restituisce il **PID** del nuovo processo (>0) o un errore (≤0). Il figlio riceve argc/argv come da ABI C:

```c
char *argv[] = {"kilo", "notes.txt", NULL};
int pid = spawn_args("/bin/kilo", 2, argv);
```

### 13.2 Capability e livelli

I processi in OS1 hanno:
- un **livello di privilegio** (PLVL_MACHINE=0, PLVL_ROOT=1, PLVL_USER=2, PLVL_GUEST=3);
- una **maschera di capability** (bit CAP_SPAWN, CAP_FS_WRITE, CAP_IPC_ANY, CAP_WINDOW, CAP_REG_WRITE).

Allo spawn, il kernel **clampa** sia il livello (il figlio non può essere più privilegiato del padre) sia la capability (intersezione). Questo è il modo “ufficiale” di lanciare processi con meno potere:

```c
#include <caps.h>

/* figlio con solo CAP_WINDOW */
long pid = spawn_caps("/bin/editor", PLVL_USER, CAP_WINDOW);
```

### 13.3 `exit` e `wait`

```c
void exit(int status);          /* termina il processo corrente */
int  kill_process(int pid);     /* chiede al kernel di terminarne un altro */
int  wait(int pid);             /* non-bloccante */
```

`wait` è *non-bloccante*:
- ritorna `pid` se il figlio è appena morto ed è ancora “corpo”;
- ritorna `-1` se è ancora vivo;
- ritorna `-2` se non esiste (mai esistito o già ripulito dal kernel).

Questo è il pattern canonico di `init`:

```c
while (1) {
    int r = wait(pid_shell);
    if (r == pid_shell || r == -2) {
        /* è morto, riavvialo */
        pid_shell = spawn("/bin/shell");
    }
    OS1_sleep(50);  /* non busy-waitare */
}
```

### 13.4 PID, cwd, argomenti

```c
int  get_pid(void);                /* PID del processo corrente */
int  chdir(const char *path);
int  getcwd(char *buf, size_t size);
```

`main` riceve `argc`/`argv` come in POSIX, con la convenzione che `argv[argc]` non è necessariamente `NULL` (il kernel tronca a `argc`).

### 13.5 Esercizio

Scrivi un programma `monitor` che fa `spawn("/bin/shell")` in un ciclo, aspettando con `wait` e rilanciando se muore. Stampa su stdout ogni riavvio. Confronta con `init.c` nel bundle.

---

## Capitolo 14 · IPC: send, recv, try_recv

### 14.1 Il modello

L’IPC di OS1 è **a messaggi**, bloccante o non-bloccante. Il messaggio è una struttura fissa:

```c
struct ipc_message {
    int from;             /* PID del mittente, riempito dal kernel in ricezione */
    int type;             /* IPC_TYPE_* */
    uint64_t data1;       /* payload numerico #1 */
    uint64_t data2;       /* payload numerico #2 */
    char payload[64];     /* payload binario/UTF-8 */
};
```

Tipi:
- `IPC_TYPE_RAW = 0`
- `IPC_TYPE_INPUT = 1` (tastiera)
- `IPC_TYPE_MOUSE = 4` (mouse)
- `IPC_TYPE_NOTIFY = 0x100` (popup di notifica)
- `IPC_TYPE_RESIZE = 0x200` (cambio dimensione finestra/desktop)

### 14.2 Le primitive

```c
int send(int pid, struct ipc_message *msg);
int recv(int pid, struct ipc_message *msg);
int try_recv(int pid, struct ipc_message *msg);   /* non-bloccante */
```

- `send` blocca finché il destinatario non ha ricevuto.
- `recv` blocca finché un mittente (specifico, o `-1` per *qualsiasi*) non ha inviato.
- `try_recv` ritorna subito: `< 0` se non c’era niente, `0` se ha ricevuto.

### 14.3 Esempio: server di notifiche minimale

```c
#include <os1.h>

int main(void) {
    struct ipc_message msg;
    while (1) {
        if (recv(-1, &msg) < 0) continue;   /* blocca finché non arriva qualcosa */
        if (msg.type == IPC_TYPE_NOTIFY) {
            /* stampa il payload sulla finestra o sulla console */
            print("[notify] ");
            print(msg.payload);
            print("\n");
        }
    }
    return 0;
}
```

### 14.4 Notifiche di sistema

La funzione `notify(title, msg)` è un helper che compone `title: msg` nel payload e lo spedisce al PID letto dalla chiave di registry `srv.notify_pid`. Vedi Capitolo 18.

### 14.5 Esercizio

Scrivi due programmi: `sender` che legge una riga da stdin e la manda al PID passato come argomento; `receiver` che resta in ascolto su `recv(-1, ...)` e stampa ogni messaggio. Avviali in due finestre della shell.

---

## Capitolo 15 · Finestre e grafica

### 15.1 Creare una finestra

```c
int create_window(int x, int y, int w, int h, const char *title);
```

Restituisce un *window id* (>0) o negativo su errore. Le coordinate sono in pixel, origine in alto a sinistra.

```c
int win = create_window(100, 100, 640, 480, "La mia finestra");
if (win < 0) {
    print("errore creando finestra\n");
    return 1;
}
```

### 15.2 Disegnare primitive

```c
void window_draw(int win_id, int x, int y, int w, int h, unsigned int color);
void window_blit(int win_id, int x, int y, int w, int h, const unsigned int *buf);
void compositor_render(void);   /* pubblica il disegno sullo schermo */
```

Il `color` è **ARGB 32 bit**: `0xAARRGGBB`. Una piccola tavolozza è definita in `graphics.h`:

```c
COLOR_WHITE 0xFFFFFFFF
COLOR_BLACK 0xFF000000
COLOR_RED   0xFFFF0000
COLOR_GREEN 0xFF00FF00
COLOR_BLUE  0xFF0000FF
```

Esempio: una finestra rossa con un rettangolo blu al centro.

```c
int win = create_window(50, 50, 400, 300, "demo");
window_draw(win, 0, 0, 400, 300, COLOR_RED);
window_draw(win, 150, 100, 100, 100, COLOR_BLUE);
compositor_render();
```

### 15.3 Testo

```c
int graphics_draw_text(int win_id, int x, int y, const char *text, uint32_t color);
int graphics_text_width(const char *text);
```

Il rendering di testo usa il font bitmap OS1 (`/fonts/Rewir-Light.off`) caricato automaticamente. Se il font non è disponibile, fa fallback sulla pipeline terminale.

```c
graphics_draw_text(win, 10, 10, "Ciao!", COLOR_WHITE);
```

### 15.4 Immagini

```c
uint32_t *graphics_load_image(const char *path, int *w, int *h);
```

Carica un PNG, JPG, BMP, GIF (via `stb_image`, incluso in `lib.c`). Restituisce un buffer ARGB da liberare con `free`.

```c
int w, h;
uint32_t *px = graphics_load_image("/wallpaper.png", &w, &h);
if (px) {
    window_blit(win, 0, 0, w, h, px);
    compositor_render();
    free(px);
}
```

### 15.5 Flag di finestra e focus

```c
void set_window_flags(int win_id, int flags);
void set_focus(int pid);
```

Bit dei flag:
- `1` = top-most (sempre sopra le altre finestre)
- `2` = visible
- `4` = hidden
- `8` = passive (click-through, non prende il focus)

### 15.6 Esercizio

Scrivi un programma che apre una finestra 200×200, la riempie di un colore a tua scelta, ci scrive sopra il tuo nome centrato, e resta in attesa di un input da tastiera per chiudersi.

---

## Capitolo 16 · Input: tastiera, mouse, resize

### 16.1 L’evento

```c
typedef struct {
    int type;
    union {
        struct {
            unsigned char key;       /* ASCII (0 per i tasti speciali) */
            int state;                /* 0=rilasciato, 1=premuto, 2=ripetuto */
            uint16_t scancode;        /* HID scancode */
            char utf8[8];             /* UTF-8 (se applicabile) */
        } keyboard;
        struct {
            int button;
            int state;
            int x, y;
        } mouse;
        struct {
            int w, h;                  /* nuova dimensione */
        } resize;
    };
} input_event_t;
```

### 16.2 `input_poll_event`

```c
int input_poll_event(input_event_t *event);
```

Ritorna `1` se un evento era pronto, `0` altrimenti. **Non blocca**: l’input arriva come IPC dal driver e devi chiedere periodicamente.

```c
input_event_t ev;
while (running) {
    if (input_poll_event(&ev)) {
        if (ev.type == INPUT_TYPE_KEYBOARD && ev.keyboard.state == KEY_PRESSED) {
            char c = ev.keyboard.key;
            if (c == 'q') running = 0;
        } else if (ev.type == INPUT_TYPE_MOUSE) {
            printf("mouse @ %d,%d button=%d state=%d\n",
                   ev.mouse.x, ev.mouse.y, ev.mouse.button, ev.mouse.state);
        } else if (ev.type == INPUT_TYPE_RESIZE) {
            printf("window resized to %dx%d\n", ev.resize.w, ev.resize.h);
        }
    }
    /* non busy-waitare: usa un sleep corto */
    OS1_sleep(10);
}
```

### 16.3 Scancode dei tasti speciali

Quando `key` è zero (perché il tasto non è ASCII), guarda `scancode`:

```c
INPUT_KEY_ESC        1
INPUT_KEY_BACKSPACE  14
INPUT_KEY_TAB        15
INPUT_KEY_ENTER      28
INPUT_KEY_UP         103
INPUT_KEY_LEFT       105
INPUT_KEY_RIGHT      106
INPUT_KEY_DOWN       108
```

### 16.4 Esercizio

Crea un programma “paint” minimale: finestra 400×400 bianca, cliccando col mouse disegni un cerchietto nero di 4 pixel di raggio nel punto del click. Esci premendo `q`.

---

## Capitolo 17 · Tempo: OS1_sleep, nanosleep, clock_gettime

### 17.1 `OS1_sleep` (base API proprietaria)

```c
void OS1_sleep(int ms);
```

Mette il processo in stato *sleeping* per `ms` millisecondi. Non è interrompibile: il processo si risveglia dopo il tempo richiesto. L’unità è il **millisecondo**, distinta da quella POSIX.

```c
print("a");
OS1_sleep(500);   /* mezzo secondo di attesa vera, no busy-wait */
print("b\n");
```

### 17.2 `nanosleep` (POSIX-like)

```c
#include <time.h>
int nanosleep(const struct timespec *req, struct timespec *rem);
```

Granularità al nanosecondo. Su OS1 la precisione effettiva è il **tick del kernel** (~10 ms a HZ=100); arrotondato per eccesso.

```c
struct timespec ts = { 0, 50 * 1000 * 1000 };   /* 50 ms */
nanosleep(&ts, NULL);
```

### 17.3 `clock_gettime`

```c
int clock_gettime(clockid_t clk, struct timespec *ts);
```

- `CLOCK_MONOTONIC` (alias `CLOCK_REALTIME`): nanosecondi monotonicamente crescenti dal boot.
- `CLOCK_PROCESS_CPUTIME_ID`: nanosecondi di CPU consumati da *questo* processo.

```c
struct timespec t0, t1;
clock_gettime(CLOCK_MONOTONIC, &t0);
/* … fai qualcosa … */
clock_gettime(CLOCK_MONOTONIC, &t1);
long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000
                + (t1.tv_nsec - t0.tv_nsec) / 1000000;
printf("elapsed: %ld ms\n", elapsed_ms);
```

### 17.4 `os1_mono_ns` / `os1_cpu_ns`

Dirette, senza passare per la struttura `timespec`. Spesso utili nei giochi o nei loop di animazione:

```c
unsigned long long start = os1_mono_ns();
/* lavoro */
unsigned long long dur = os1_mono_ns() - start;
printf("%llu ns\n", dur);
```

### 17.5 Esercizio

Scrivi un cronometro: stampa il tempo trascorso in millisecondi, aggiornato ogni 100 ms, fino a quando premi `q`.

---

## Capitolo 18 · Registry e configurazione

Il **registry** è una mappa chiave→valore globale, persistente (fino al prossimo reboot). I processi possono leggere e scrivere.

```c
int registry_read(const char *key, char *buf, size_t size);
int registry_write(const char *key, const char *value);
```

- `read` copia il valore in `buf` (al massimo `size-1` byte + `'\0'`); ritorna 0 se la chiave esiste, negativo se non esiste.
- `write` setta il valore. OS1 non ha autenticazione su `registry_write` (è un noto security TODO): chiunque può sovrascrivere una chiave.

```c
char buf[64];
if (registry_read("os.theme", buf, sizeof(buf)) == 0) {
    print("tema attuale: ");
    print(buf);
    print("\n");
} else {
    registry_write("os.theme", "dark");
}
```

### 18.1 Convenzioni

Alcune chiavi sono usate dal sistema:
- `srv.notify_pid`: il PID del notification server, su cui `notify()` risolve la destinazione.
- `os.*`: chiavi convenzionali di configurazione OS1.
- Chiavi con prefisso `app.<appname>.*`: specifiche di una singola applicazione.

### 18.2 Esercizio

Scrivi un programma `setlang` che prende una lingua da riga di comando e la scrive in `user.lang`. Scrivi anche un programma `getlang` che la legge e la stampa.

---

## Capitolo 19 · Memoria: sbrk, malloc, mmap

### 19.1 `sbrk`

La syscall più “a basso livello” per la memoria è `sbrk`, che estende (o contrae) il *program break*, cioè il confine dell’heap del processo.

```c
void *sbrk(intptr_t increment);
```

- `increment > 0`: aggiunge memoria all’heap; ritorna il *vecchio* break.
- `increment == 0`: ritorna il break attuale.
- `increment < 0`: contrae l’heap.
- Ritorna `(void *)-1` su errore (es. out of memory).

### 19.2 `malloc`/`free`/`realloc`/`calloc`

Lo strato sopra `sbrk`. Vedi Capitolo 9 della Parte I. Ricorda: su OS1, `malloc` non libera mai memoria al sistema; l’heap cresce monotonicamente per la vita del processo.

### 19.3 `mmap`/`munmap`

`mmap` con `MAP_ANONYMOUS` è supportato **come shim sopra `malloc`**: serve a portare programmi POSIX. Per scrivere codice nativo OS1, preferisci `malloc`.

```c
#include <sys/mman.h>
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
               MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
if (p != MAP_FAILED) {
    /* … usa p come un buffer di 4 KB … */
    munmap(p, 4096);
}
```

### 19.4 Stack e variabili locali

Lo stack è allocato dal kernel al primo caricamento del processo (128 KB di default; vedi `STACK_SIZE` in `os1.h`). Cresce automaticamente verso il basso; **overflow dello stack** = crash senza messaggio. Per un programma con buffer grossi, preferisci sempre `malloc`.

### 19.5 Esercizio

Implementa una piccola arena di memoria: `arena_create(size)`, `arena_alloc(arena, n)`, `arena_reset(arena)`. Deve solo tenere un puntatore all’ultimo offset e crescere in grossi blocchi preallocati. Usala in un programma che fa molte piccole allocazioni e poi le libera tutte in un colpo.

---

## Capitolo 20 · Fonti e tipografia

OS1 usa un formato bitmap proprietario, documentato in `font.h` e `font_lib.h`.

```c
struct font_glyph_info {
    int16_t x0, y0;        /* offset bitmap */
    uint8_t width, height; /* dimensioni glyph */
    int16_t advance;       /* avanzamento orizzontale */
    uint32_t data_offset;  /* offset nel bitmap */
};
```

L’header `font.h` documenta l’header on-disk:

```c
struct font_header {
    uint32_t magic;       /* FONT_MAGIC = 0x31534F */
    uint16_t size;
    uint16_t first_char;
    uint16_t num_chars;
    uint16_t ascent;
    uint16_t descent;
    uint32_t bitmap_size;
};
```

### 20.1 Caricare e disegnare un font

```c
#include <font_lib.h>

struct font_ctx *f = font_load("/fonts/Rewir-Light.off");
if (f) {
    font_draw_string(win_id, f, 10, 10, "Ciao!", 0xFFFFFFFF);
    int w = font_string_width(f, "Ciao!");
    print("width: "); print_hex(w); print("\n");
    font_free(f);
}
```

### 20.2 `set_font`

`int set_font(void *data, size_t size)` imposta un nuovo font di sistema. Il puntatore `data` resta di proprietà del chiamante.

### 20.3 Esercizio

Scrivi un programma che carica il font, lo usa per scrivere una frase lunga in una finestra, e mostra la larghezza totale in pixel.

---

## Capitolo 21 · Esempi “quasi veri”

### 21.1 Cronometro grafico

```c
/* cronometro.c */
#include <os1.h>
#include <time.h>

int main(void) {
    int win = create_window(80, 80, 240, 80, "Cronometro");
    if (win < 0) return 1;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int running = 1;
    while (running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000
                + (now.tv_nsec - t0.tv_nsec) / 1000000;

        window_draw(win, 0, 0, 240, 80, COLOR_BLACK);
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld.%03ld s", ms / 1000, ms % 1000);
        graphics_draw_text(win, 10, 30, buf, COLOR_WHITE);
        compositor_render();

        input_event_t ev;
        if (input_poll_event(&ev) &&
            ev.type == INPUT_TYPE_KEYBOARD &&
            ev.keyboard.state == KEY_PRESSED &&
            ev.keyboard.key == 'q') {
            running = 0;
        }
        OS1_sleep(33);
    }
    return 0;
}
```

### 21.2 Lettore di un file di log

```c
/* tail.c */
#include <os1.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        print("uso: tail <file>\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        printf("non posso aprire %s\n", argv[1]);
        return 1;
    }
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        print(buf);
    }
    fclose(f);
    return 0;
}
```

### 21.3 IPC chat

```c
/* chat_send.c */
#include <os1.h>

int main(int argc, char **argv) {
    if (argc < 2) { print("uso: chat_send <pid>\n"); return 1; }
    int pid = atoi(argv[1]);
    char buf[64];
    while (fgets(buf, sizeof(buf), stdin)) {
        struct ipc_message m = {0};
        m.type = IPC_TYPE_RAW;
        memcpy(m.payload, buf, 64);
        send(pid, &m);
    }
    return 0;
}
```

```c
/* chat_recv.c */
#include <os1.h>

int main(void) {
    int my_pid = get_pid();
    printf("chat_recv pid=%d\n", my_pid);
    struct ipc_message m;
    while (1) {
        if (recv(-1, &m) == 0) {
            print("ricevuto: ");
            print(m.payload);
            print("\n");
        }
    }
}
```

---

# PARTE III — ABI di OS1

> **Questa parte è un riferimento.** Ogni syscall è elencata con: numero, firma, argomenti, valore di ritorno, errori, esempio d’uso. Tutti i numeri provengono da `include/api/syscall_nums.h` e sono la **single source of truth** condivisa fra kernel, dispatcher e stub utente (ABI-01/ABI-SYS-01).
>
> Convenzione di ritorno: successo = `>= 0`, fallimento = `-errno` (dove `errno` è uno dei valori in `posix_types.h`).
>
> **Architetture**: AArch64 (`x0..x5` argomenti, `x8` numero, `svc #0`); amd64 (`rdi,rsi,rdx,r10,r8,r9`, `rax`, `syscall`).

## Capitolo 22 · Convenzioni generali

### 22.1 Error model

Tutte le syscall restituiscono:
- `>= 0` su successo (spesso un conteggio, un PID, un fd);
- valore negativo (es. `-EINVAL`, `-ENOMEM`) su fallimento.

`errno.h` esiste, ma attualmente `errno` è un semplice `int` placeholder; i wrapper in `lib.c` non lo settano. Scrivi le tue funzioni facendo `if (r < 0) return r;`.

### 22.2 Tabella rapida dei codici di errore

| Codice | Nome | Significato tipico |
|---|---|---|
| 1   | `EPERM`  | permesso negato |
| 2   | `ENOENT` | file/dir inesistente |
| 3   | `ESRCH`  | processo inesistente |
| 5   | `EIO`    | errore I/O |
| 9   | `EBADF`  | file descriptor invalido |
| 11  | `EAGAIN` | riprova (risorsa non pronta) |
| 12  | `ENOMEM` | out of memory |
| 13  | `EACCES` | permesso negato |
| 14  | `EFAULT` | indirizzo non valido |
| 16  | `EBUSY`  | risorsa occupata |
| 17  | `EEXIST` | esiste già |
| 20  | `ENOTDIR`| non è una directory |
| 22  | `EINVAL` | argomento invalido |
| 38  | `ENOSYS` | syscall non implementata |
| 39  | `ENOTEMPTY` | directory non vuota |

### 22.3 La tabella dei file descriptor

- `0` (stdin): input dalla tastiera / IPC.
- `1` (stdout): finestra di output del processo (se ne ha una) o UART.
- `2` (stderr): come `1` su OS1.
- `>= 3`: file aperti con `open`.

I programmi **windowed** scrivono sulla propria finestra; quelli **windowless** (es. strumenti CLI lanciati dalla shell) scrivono sul TTY della shell. `window_of_pid(pid)` ritorna l’id della finestra associata, 0 se non ce l’ha.

### 22.4 Capability: cosa può fare un processo

| Bit | Nome | Cosa sblocca |
|---|---|---|
| `1 << 0` | `CAP_SPAWN`     | `spawn`, `spawn_args`, `spawn_caps` |
| `1 << 1` | `CAP_FS_WRITE`  | scrittura su file, `O_CREAT` |
| `1 << 2` | `CAP_IPC_ANY`   | `send` a PID non imparentati |
| `1 << 3` | `CAP_WINDOW`    | `create_window`, `set_focus` su sé stessi |
| `1 << 4` | `CAP_REG_WRITE` | `registry_write` |

I livelli (PLVL_MACHINE=0 … PLVL_GUEST=3) sono il *root* del modello: PLVL_MACHINE bypassa ogni capability check (è il kernel/init/servizi); gli altri sono soggetti alla maschera.

La maschera `CAP_*` è l’autorità **ambientale** che gate l’*acquisizione* delle capability; sopra di essa, dal 2026-06-20, c’è il **layer a oggetti**: handle non falsificabili a oggetti del kernel, con diritti separabili e attenuabili (Capitolo 23-bis). Inoltre la **posizione di un binario nel VFS** ne fissa il preset di privilegio (`/sys/bin` = ROOT, `/bin` = USER), sempre soggetta al creator-clamp monotono (vedi 23-bis.6).

---

## Capitolo 23 · Syscall reference (numerica)

> I numeri sono quelli di `syscall_nums.h`. “Firma” è la dichiarazione C del wrapper. “Rit” indica il valore di ritorno. Per ogni syscall è indicato anche il file utente dove il wrapper è definito (in `user/sys/lib/lib.c`).

### 23.1 POSIX-shaped

| # | Nome | Firma | Rit | Note |
|---|---|---|---|---|
| 56 | `SYS_OPEN`        | `int open(const char *path, int flags)`         | fd ≥ 3 o errore | O_CREAT/O_TRUNC rifiutati con `-EINVAL` |
| 57 | `SYS_CLOSE`       | `int close(int fd)`                              | 0 / errore     | |
| 62 | `SYS_LSEEK`       | `long lseek(int fd, long off, int whence)`       | nuova pos / errore | `SEEK_SET/CUR/END` in `posix_types.h` |
| 63 | `SYS_READ`        | `long read(int fd, char *buf, unsigned long n)`  | byte letti ≥ 0 | `0` su EOF |
| 64 | `SYS_WRITE`       | `long write(int fd, const char *buf, size_t n)` | byte scritti ≥ 0 | è **lungo**, non void |
| 93 | `SYS_EXIT`        | `void exit(int status)`                          | non ritorna   | l’unica syscall che non ritorna |
| 169| `SYS_GET_TIME`    | `long get_time(void)`                            | ms dal boot   | vedi Capitolo 17 |
| 172| `SYS_GETPID`      | `int get_pid(void)`                              | PID           | |

**Esempio end-to-end: leggere un file a blocchi.**

```c
int fd = open("/config.txt", O_RDONLY);
if (fd < 0) return 1;
char buf[512];
long n;
while ((n = read(fd, buf, sizeof(buf))) > 0) {
    write(1, buf, n);
}
close(fd);
```

### 23.2 Grafica e compositor

| # | Nome | Firma | Note |
|---|---|---|---|
| 200 | `SYS_DRAW`              | `void draw(int x, int y, int w, int h, int color)` | disegno diretto sul framebuffer |
| 201 | `SYS_FLUSH`             | `void flush(void)`                                | forza il push del disegno |
| 210 | `SYS_CREATE_WINDOW`     | `int create_window(int x,int y,int w,int h,const char *title)` | id finestra |
| 211 | `SYS_WINDOW_DRAW`       | `void window_draw(int w, int x, int y, int ww, int h, uint32_t c)` | disegna un rettangolo nella finestra |
| 212 | `SYS_COMPOSITOR_RENDER` | `void compositor_render(void)`                   | pubblica il frame |
| 213 | `SYS_WINDOW_BLIT`       | `void window_blit(int w, int x, int y, int ww, int h, const uint32_t *buf)` | upload di un pixel buffer ARGB |
| 214 | `SYS_WINDOW_SET_FLAGS`  | `void set_window_flags(int w, int flags)`         | vedi 15.5 |
| 215 | `SYS_DESTROY_WINDOW`    | `void destroy_window(int w)`                     | |
| 217 | `SYS_WINDOW_WRITE`      | `long window_write(int w, const char *b, unsigned long n)` | scrive testo su una finestra per id |
| 218 | `SYS_WINDOW_OF_PID`     | `int window_of_pid(int pid)`                     | 0 se non ha finestra |
| 219 | `SYS_WINDOW_GRID`       | `int window_grid(int w, int *cols, int *rows)`  | dimensione griglia TTY della finestra |
| 270 | `SYS_DISPLAY_INFO`      | `long _sys_display_info(void)`                   | ritorna `(w<<16) | h` |
| 271 | `SYS_SET_DISPLAY_MODE`  | `int _sys_set_display_mode(int w, int h)`       | CAP_MACHINE |
| 272 | `SYS_WINDOW_RESIZE`     | `int _sys_window_resize(int w, int ww, int h)`  | ridimensiona la finestra |
| 273 | `SYS_DISPLAY_POLL`      | `int _sys_display_poll(void)`                    | 1 se c’è stato un resize |
| 274 | `SYS_SET_STYLE`         | `int _sys_set_style(int style, int theme)`       | -1 = lascia invariato |
| 275 | `SYS_SET_ZOOM`          | `int _sys_set_zoom(int percent)`                 | 25..400 |

### 23.3 Memoria

| # | Nome | Firma | Note |
|---|---|---|---|
| 216 | `SYS_SBRK`              | `void *sbrk(intptr_t inc)` | gestisce l’heap del processo |

**Esempio:**

```c
void *old = sbrk(0);              /* program break attuale */
void *p   = sbrk(4096);           /* alloca 4 KB */
if (p == (void *)-1) { /* errore */ }
```

### 23.4 Processi

| # | Nome | Firma | Note |
|---|---|---|---|
| 220 | `SYS_SPAWN`        | `int spawn(const char *path)` | |
| 221 | `SYS_KILL`         | `int kill_process(int pid)` | |
| 222 | `SYS_GETPROCS`     | `long _sys_get_procs(void *buf, size_t max)` | riempie `struct ps_info[]` |
| 223 | `SYS_YIELD`        | `void yield(void)` | rilascia la CPU |
| 234 | `SYS_SPAWN_CAPS`   | `long spawn_caps(const char *path, int level, unsigned long caps)` | kernel clampa livello e caps |
| 247 | `SYS_WAIT`         | `int wait(int pid)` | non-bloccante, vedi 13.3 |

### 23.5 IPC

| # | Nome | Firma | Note |
|---|---|---|---|
| 230 | `SYS_SEND`         | `int send(int pid, struct ipc_message *m)` | |
| 231 | `SYS_RECV`         | `int recv(int pid, struct ipc_message *m)` | `pid=-1` = qualsiasi |
| 232 | `SYS_SET_FOCUS`    | `void set_focus(int pid)` | |
| 233 | `SYS_TRY_RECV`     | `int try_recv(int pid, struct ipc_message *m)` | non-bloccante |

### 23.6 Registry, file, font, directory, tempo

| # | Nome | Firma | Note |
|---|---|---|---|
| 250 | `SYS_REGISTRY`         | `long _sys_registry(int op, const char *k, char *v, size_t n)` | op 0=read, 1=write |
| 251 | `SYS_FILE_WRITE`       | `int file_write(const char *p, const void *b, int n, int off)` | |
| 252 | `SYS_FILE_READ`        | `int file_read(const char *p, void *b, int n, int off)` | se `b==NULL` e `n==0` ritorna la dimensione del file |
| 253 | `SYS_SET_FONT`         | `int set_font(void *data, size_t size)` | il buffer deve restare allocato |
| 254 | `SYS_LIST_DIR`         | `int list_dir(const char *p, char *b, size_t n)` | nomi separati da spazio |
| 255 | `SYS_CHDIR`            | `int chdir(const char *p)` | |
| 256 | `SYS_GETCWD`           | `int getcwd(char *b, size_t n)` | |
| 257 | `SYS_NANOSLEEP`        | `void _sys_nanosleep(unsigned long long ns)` | precisione ~tick |
| 258 | `SYS_CLOCK_GETTIME`    | `long _sys_clock_gettime(int clk)` | 0=monotonic, 1=cpu |

### 23.7 Oggetti / handle / capability (235..243 + 202)

Il **layer a capability reale** (ASTRA §6.1/6.2/6.5, stabilizzato 2026-06-20). Le firme e le costanti vengono da `include/api/object.h`; il modello è descritto nel Capitolo 23-bis.

| # | Nome | Firma | Rit | Note |
|---|---|---|---|---|
| 202 | `SYS_WINDOW_ENUM`  | `long OS1_window_enum(struct window_info *buf, unsigned long max)` | conteggio finestre / `-errno` | snapshot di tutte le finestre |
| 235 | `SYS_HANDLE_CREATE`| `long OS1low_handle_create(int ns, const char *path, unsigned int rights, int type)` | handle ≥ 0 / `-errno` | `ns` = `OS1_NS_*` |
| 236 | `SYS_HANDLE_DUP`   | `long OS1low_handle_duplicate(int handle, unsigned int new_rights)` | nuovo handle / `-errno` | `new_rights ⊆` diritti correnti |
| 237 | `SYS_HANDLE_CLOSE` | `long OS1low_handle_close(int handle)` | 0 / `-errno` | libera lo slot |
| 238 | `SYS_CAP_QUERY`    | `long OS1low_cap_query(int handle)` | `(type<<24)|rights` / `-errno` | usa le macro `OS1_CAPQ_*` |
| 239 | `SYS_CAP_GRANT`    | `long OS1low_cap_grant(int target_pid, int handle, unsigned int rights)` | 0 / `-errno` | richiede `OS1_RIGHT_TRANSFER` |
| 240 | `SYS_OBJECT_READ`  | `long OS1_object_read(int handle, void *buf, unsigned long n)` | byte / `-errno` | richiede `OS1_RIGHT_READ` |
| 241 | `SYS_OBJECT_WRITE` | `long OS1_object_write(int handle, const void *buf, unsigned long n)` | byte / `-errno` | richiede `OS1_RIGHT_WRITE` |
| 242 | `SYS_OBJECT_WAIT`  | `long OS1_object_wait(int handle, long arg)` | object-specific / `-errno` | richiede `OS1_RIGHT_WAIT` |
| 243 | `SYS_OBJECT_CTL`   | `long OS1_object_ctl(int handle, int cmd, long arg)` | object-specific / `-errno` | verbi `OBJ_CTL_*` |

---

## Capitolo 23-bis · Oggetti, handle e capability (modello a oggetti)

> **Stato (2026-06-20)**: layer implementato e stabile (ASTRA §7.1). È la generalizzazione del "seme" B3 (la fd-table per-processo) in un vero modello a capability stile seL4/Mach: l’autorità non è più un’identità ambientale (PID + maschera di livello), ma un **handle non falsificabile** verso un **oggetto** del kernel. Header: `include/api/object.h` (condiviso kernel↔userland). Test di regressione: `/bin/{captest,capipc,capreg,capkill}`.

### 23-bis.1 Il modello in tre frasi

1. Una risorsa del kernel (file, processo, chiave di registry, finestra) è un **oggetto** refcontato. Un processo lo nomina solo tramite un **handle**: un piccolo intero, indice in una tabella **privata** del processo. Lo stesso numero in un altro processo non significa nulla, e un valore senza slot installato è `-EBADF` — non si può "indovinare" autorità.
2. Ogni handle porta un sottoinsieme di **diritti** (`OS1_RIGHT_*`). I diritti sono **separabili** (un handle può avere READ senza WRITE) e **attenuabili**: `duplicate`/`grant` possono solo *togliere* diritti, mai aggiungerne. L’escalation è impossibile per costruzione.
3. **Acquisire** una capability è gated dall’autorità ambientale (es. `CAP_FS_WRITE` + l’ACL `/sys,/bin` per aprire un file in scrittura; autorità di window-manager per controllare la finestra di un altro processo); **usarla** dipende solo dai diritti dell’handle. Un handle *concesso* delega quell’autorità (grant Mach/seL4).

Relazione con `<caps.h>` (Capitolo 22.4): la maschera `CAP_*` è l’autorità **ambientale** che gate l’*acquisizione* di una capability; `<object.h>` è la capability **per-oggetto** che poi possiedi e deleghi.

### 23-bis.2 Tipi di oggetto e namespace

`OS1low_handle_create(ns, path, rights, type)` crea un handle; `ns` dice come interpretare `path`:

| `type` (`OBJ_TYPE_*`) | `ns` (`OS1_NS_*`) | `path` | Oggetto |
|---|---|---|---|
| `OBJ_TYPE_FILE` (1)    | `OS1_NS_FS` (1)   | path filesystem        | file VFS (read/write/seek) |
| `OBJ_TYPE_PROCESS` (2) | `OS1_NS_PROC` (2) | PID in decimale (stringa) | processo (wait, IPC a capability, kill via `OBJ_CTL_KILL`) |
| `OBJ_TYPE_REGKEY` (3)  | `OS1_NS_REG` (3)  | chiave dotted          | chiave di registry (get/set, §6.6) |
| `OBJ_TYPE_WINDOW` (4)  | `OS1_NS_WIN` (4)  | id finestra in decimale | finestra del compositor (vedi 23-bis.5) |

### 23-bis.3 Diritti (`OS1_RIGHT_*`)

| Bit | Nome | Cosa abilita |
|---|---|---|
| `1 << 0` | `OS1_RIGHT_READ`      | `OS1_object_read` |
| `1 << 1` | `OS1_RIGHT_WRITE`     | `OS1_object_write` |
| `1 << 2` | `OS1_RIGHT_WAIT`      | `OS1_object_wait` |
| `1 << 3` | `OS1_RIGHT_DUPLICATE` | `OS1low_handle_duplicate` |
| `1 << 4` | `OS1_RIGHT_TRANSFER`  | `OS1low_cap_grant` verso un altro processo |
| `1 << 5` | `OS1_RIGHT_DESTROY`   | distruggere l’oggetto sottostante |

`OS1_RIGHT_ALL` è l’OR di tutti. `OS1low_cap_query(handle)` ritorna `(type<<24)|rights`; estrai i campi con `OS1_CAPQ_TYPE(v)` / `OS1_CAPQ_RIGHTS(v)` (un ritorno negativo è un `-errno`).

### 23-bis.4 Verbi di controllo (`OBJ_CTL_*`)

`OS1_object_ctl(handle, cmd, arg)` esegue un’azione type-specific sull’oggetto (il diritto **è** l’autorità):

| `cmd` | Valore | Oggetto | Effetto | Diritto richiesto |
|---|---|---|---|---|
| `OBJ_CTL_KILL`     | 1 | PROCESS | termina il target           | `OS1_RIGHT_DESTROY` |
| `OBJ_CTL_MINIMIZE` | 2 | WINDOW  | manda in background (dock-restorabile) | `OS1_RIGHT_WRITE` |
| `OBJ_CTL_RESTORE`  | 3 | WINDOW  | mostra + alza + focus       | `OS1_RIGHT_WRITE` |
| `OBJ_CTL_FOCUS`    | 4 | WINDOW  | dà il focus tastiera + alza | `OS1_RIGHT_READ` (il focus è non privilegiato, come il click-to-focus del compositor) |
| `OBJ_CTL_CLOSE`    | 5 | WINDOW  | distrugge solo questa finestra | `OS1_RIGHT_DESTROY` |

**Esempio — terminare un processo via capability (non via PID ambientale):**

```c
char pidstr[12];
snprintf(pidstr, sizeof(pidstr), "%d", target_pid);
long h = OS1low_handle_create(OS1_NS_PROC, pidstr, OS1_RIGHT_DESTROY, OBJ_TYPE_PROCESS);
if (h < 0) return h;                 /* -EPERM se non hai autorità di kill */
long r = OS1_object_ctl((int)h, OBJ_CTL_KILL, 0);
OS1low_handle_close((int)h);
```

**Esempio — delegare un file in sola lettura a un altro processo:**

```c
long h = OS1low_handle_create(OS1_NS_FS, "/etc/motd",
                              OS1_RIGHT_READ | OS1_RIGHT_TRANSFER, OBJ_TYPE_FILE);
/* il destinatario riceverà SOLO READ: i diritti possono solo restringersi */
OS1low_cap_grant(other_pid, (int)h, OS1_RIGHT_READ);
OS1low_handle_close((int)h);
```

### 23-bis.5 Le finestre come oggetti — Window Manager API

Una finestra è una capability di prima classe (`OBJ_TYPE_WINDOW` + `OS1_NS_WIN`, ASTRA §6.7/§7.3). Il compositor resta puro *meccanismo*; la *politica* di gestione finestre vive in un servizio userland (il dock `/sys/bin/nxui`).

`OS1_object_read` su un handle finestra riempie una `struct window_info`:

```c
struct window_info {
  int id;             /* id finestra del compositor          */
  int pid;            /* processo proprietario               */
  int x, y;           /* posizione a schermo                 */
  int w, h;           /* dimensione di disegno a schermo     */
  unsigned int flags; /* maschera WININFO_*                  */
  char title[64];     /* titolo della finestra               */
};
```

Bit di stato (`flags`):

| Bit | Nome | Significato |
|---|---|---|
| `1 << 0` | `WININFO_VISIBLE`   | attualmente composita (mostrata) |
| `1 << 1` | `WININFO_MINIMIZED` | mandata in background, restorabile dal dock |
| `1 << 2` | `WININFO_TOPMOST`   | sempre sopra, senza decorazioni |
| `1 << 3` | `WININFO_FOCUSED`   | possiede il focus tastiera |
| `1 << 4` | `WININFO_PASSIVE`   | click-through (popup di sistema) |

Per enumerare e controllare le finestre la libreria offre dei wrapper di comodità sopra il modello a capability (`handle_create → object_ctl → handle_close`):

```c
long OS1_window_enum(struct window_info *buf, unsigned long max); /* SYS_WINDOW_ENUM (202) */
int  OS1_window_minimize(int win_id);   /* OBJ_CTL_MINIMIZE */
int  OS1_window_restore(int win_id);    /* OBJ_CTL_RESTORE  */
int  OS1_window_focus(int win_id);      /* OBJ_CTL_FOCUS    */
int  OS1_window_close(int win_id);      /* OBJ_CTL_CLOSE    */
```

`OS1_window_enum` ritorna il numero di finestre scritte in `buf` (o un `-errno`). I wrapper di controllo ritornano `0` o un `-errno` (`-EPERM` senza autorità, `-ESRCH` per una finestra inesistente). **Regola di autorità**: un’app può sempre pilotare la **propria** finestra; pilotare quella di un altro processo richiede autorità di window-manager (machine/root) — è ciò che usa il dock. Il `FOCUS` è l’eccezione: richiede solo READ, coerente col click-to-focus aperto del compositor.

**Esempio — un mini dock che elenca e ripristina le finestre:**

```c
struct window_info win[32];
long n = OS1_window_enum(win, 32);
for (long i = 0; i < n; i++) {
    if (win[i].flags & WININFO_MINIMIZED)
        OS1_window_restore(win[i].id);   /* la riporta in primo piano */
}
```

### 23-bis.6 Preset di capability per percorso (VFS)

La **posizione di un binario nel VFS ne fissa il preset di privilegio** (ASTRA §7.2):

- `/sys/bin/*` → spawnato a **ROOT** (autorità di sistema; la rifinitura per-servizio è un follow-up);
- tutto il resto, in particolare `/bin/*` → **USER**.

È un *tetto + default*, **non** un’escalation: il **creator-clamp monotono** (`process_create_caps`) vieta comunque a un figlio di essere più privilegiato del suo creatore. Quindi una shell USER che lancia un binario di `/sys/bin` **non** diventa root. Inoltre `/sys/bin` è write-protected (il kernel nega le scritture non-machine sotto `/sys` e `/bin`): i binari che reggono il preset sono **immutabili**.

### 23-bis.7 Il pattern dei servizi stratificati (SRL)

Ogni CLI/controllo di sistema è costruito come **layer helper riutilizzabile + frontend sottile**, usabile sia da app utente che da app di sistema (ASTRA §7.4). L’helper **non aggiunge controlli ambientali**: si limita a wrappare syscall che il kernel **già** gate per chiamante, quindi il servizio è **sicuro-per-chiamante** automaticamente (un’app USER e un servizio ROOT ottengono esattamente i propri diritti).

- Esempio header riusabile: `user/sys/bin/nxproc.h` (gestione processi). Il CLI `nxproc` e i comandi `ps`/`top` della shell consumano lo **stesso** helper.
- Altro esempio: `nxres` (risoluzione/stile/tema del desktop).
- Pianificati, stesso pattern: `nxinfo` (info di sistema) e `nxperms` (autorizzazioni a livello utente).

Se scrivi un tuo strumento di sistema, segui lo stesso schema: metti la logica in un `.h`/`.c` helper che chiama solo syscall gated, e tieni il `main()` del CLI come una buccia che fa il parsing degli argomenti. La sicurezza la fa il kernel sul chiamante, non l’helper.

---

## Capitolo 24 · Header di OS1 in dettaglio

### 24.1 `<os1.h>`

L’header di primo livello. Includilo in ogni sorgente userland. Include a sua volta:
- `<posix_types.h>`: tipi base.
- `<syscall_nums.h>`: i numeri.
- `<caps.h>`: PLVL/CAP.
- `<stddef.h>`, `<stdarg.h>`, `<stdint.h>`: standard.

Dichiara inoltre:
- wrapper ad alto livello (`print`, `printf`, `printf_win`, `create_window`, `spawn`, …);
- wrapper sottili sugli stub `_sys_*`;
- costanti del sistema (`STACK_SIZE`, `MAX_PROCESSES`, `PROCESS_NAME_MAX`).

### 24.2 `<posix_types.h>`

Contiene:
- typedef di base (`pid_t`, `ssize_t`, `off_t`, `time_t`, `mode_t`, …);
- `PAGE_SIZE`, `PAGE_SHIFT`, macro di allineamento;
- `MIN/MAX/CLAMP`;
- codici di errore `EPERM`…`ENOTEMPTY`;
- `O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_APPEND`, `SEEK_SET/CUR/END`;
- la struttura `ipc_message`;
- i tipi di IPC `IPC_TYPE_RAW/INPUT/MOUSE/NOTIFY/RESIZE`;
- attributi del compilatore (`__packed`, `__aligned`, `__unused`, …);
- barriere di memoria (`mb/rmb/wmb/isb`) — non servono nel programmatore userland tipico, ma sono lì se ti servono.

### 24.3 `<caps.h>`

```c
#define PLVL_MACHINE 0
#define PLVL_ROOT    1
#define PLVL_USER    2
#define PLVL_GUEST   3
#define PLVL_COUNT   4

#define CAP_SPAWN     (1u << 0)
#define CAP_FS_WRITE  (1u << 1)
#define CAP_IPC_ANY   (1u << 2)
#define CAP_WINDOW    (1u << 3)
#define CAP_REG_WRITE (1u << 4)
#define CAP_ALL (CAP_SPAWN|CAP_FS_WRITE|CAP_IPC_ANY|CAP_WINDOW|CAP_REG_WRITE)
```

### 24.3-bis `<object.h>`

L’ABI a oggetti/handle/capability (incluso da `<os1.h>`). Definisce i tipi di oggetto `OBJ_TYPE_*`, i diritti `OS1_RIGHT_*`, i namespace `OS1_NS_*`, i verbi `OBJ_CTL_*`, i flag `WININFO_*`, `struct window_info` e le macro `OS1_CAPQ_*`. Le funzioni `OS1low_handle_*` / `OS1low_cap_*` / `OS1_object_*` e `OS1_window_*` operano su queste costanti. Modello e firme: Capitolo 23-bis.

### 24.4 `<input.h>`

Definisce `input_event_t` e i tipi/scancode dei tasti. Vedi Capitolo 16.

### 24.5 `<graphics.h>`

Disegno di rettangoli, blit, testo, caricamento immagini, tavolozza standard. Vedi Capitolo 15.

### 24.6 `<font.h>` / `<font_lib.h>`

Il formato bitmap OS1 e le funzioni di disegno misurazione. Vedi Capitolo 20.

### 24.7 `<dirent.h>`, `<sys/stat.h>`, `<sys/mman.h>`, `<sys/ioctl.h>`, `<sys/wait.h>`, `<sys/types.h>`

Shim POSIX-like per portare programmi da Linux/BSD. Comportamenti specifici documentati nei commenti in cima a ciascun header. Esempi:
- `<sys/mman.h>`: `mmap(MAP_ANONYMOUS, …)` → `malloc`.
- `<dirent.h>`: `opendir` snapshotta `list_dir` e lo tokenizza.
- `<sys/ioctl.h>`: solo `TIOCGWINSZ`, letto da `window_grid`.
- `<sys/wait.h>`: `WIFEXITED` sempre vero, `WEXITSTATUS` = `status & 0xff` (lo stato d’uscita non è ancora propagato dal kernel).

### 24.8 `<signal.h>`

No-op: `signal()` ritorna sempre `SIG_DFL`. Su OS1 non c’è un modello di segnali.

### 24.9 `<unistd.h>`

- `isatty(fd)`: vero per 0/1/2.
- `getpid()`: alias di `get_pid()`.
- `pipe()`: ritorna errore (OS1 non ha pipe anonime).
- `unlink()`: no-op (VFS non ha cancellazione).
- `usleep(usec)`: blocking, risolto da `nanosleep`.

### 24.10 `<time.h>`

- `clock_gettime(CLOCK_MONOTONIC | CLOCK_PROCESS_CPUTIME_ID, ts)`.
- `nanosleep(req, rem)` con `rem` azzerato se non nullo.

### 24.11 `<termios.h>`, `<poll.h>`

No-op: `tcgetattr`/`tcsetattr` azzerano la struttura e ritornano 0; `poll` ritorna 0 (nessun fd pronto). Servono solo a far compilare programmi POSIX-style.

---

## Capitolo 25 · Convenzioni ABI di basso livello

### 25.1 AArch64

```
x0  = arg 0 / ritorno
x1  = arg 1
x2  = arg 2
x3  = arg 3
x4  = arg 4
x5  = arg 5
x8  = numero di syscall
svc #0
```

Gli **stub** in `user/arch/aarch64/syscall.S` non sono che:

```asm
.global _sys_write
_sys_write:
    mov x8, #SYS_WRITE
    svc #0
    ret
```

Lo stub riceve gli argomenti già nei registri dal C compiler. Il kernel restituisce in `x0`; lo stub lo lascia dov’è e ritorna con `ret`.

`_start` è la prima funzione eseguita: chiama `main`, poi passa il valore di ritorno a `_sys_exit`. Non c’è un vero `crt0` C: è un paio di istruzioni in assembly.

### 25.2 amd64

```
rax = numero di syscall
rdi = arg 0
rsi = arg 1
rdx = arg 2
r10 = arg 3 (NON rcx!)
r8  = arg 4
r9  = arg 5
syscall
```

Differenza importante rispetto al System V ABI: in `syscall` l’arg 3 passa in `r10`, non in `rcx`. Per questo gli stub hanno:

```asm
_sys_create_window:
    movq $SYS_CREATE_WINDOW, %rax
    movq %rcx, %r10   ; rcx → r10 per l'arg 3
    syscall
    ret
```

### 25.3 Chiamare una syscall a mano (raro)

Se per qualche ragione non vuoi usare i wrapper (debug, micro-ottimizzazione), puoi:

```c
register long x8 __asm__("x8") = SYS_WRITE;  // AArch64
register long x0 __asm__("x0") = 1;          // fd
register long x1 __asm__("x1") = (long)"hi"; // buf
register long x2 __asm__("x2") = 2;          // count
__asm__ volatile ("svc #0"
                  : "+r"(x0)
                  : "r"(x1), "r"(x2), "r"(x8)
                  : "memory");
```

Non farlo a meno che tu non sappia esattamente perché: perdi portabilità, leggibilità, e i controlli di capability.

---

## Capitolo 26 · Diagnostica e debugging

### 26.1 Crash e segnali

OS1 non ha segnali, ma il kernel isola i fault utente: un processo che crasha **muore, ma non porta giù il kernel né gli altri processi**. La shell sopravvive, puoi rilanciare.

Per testare:

```c
int main(void) {
    *(volatile int *)0 = 0;   /* page fault garantito */
    return 0;
}
```

Vedrai un messaggio del kernel (sulla console UART) con un backtrace simbolico, e il processo termina. La shell è ancora lì.

### 26.2 Stack canary

`__stack_chk_guard` è un valore fisso in `lib.c`. Abilitando `-fstack-protector` il compilatore mette un canary prima dei buffer locali. Se sfori, viene chiamato `__stack_chk_fail` e il processo muore con “Stack smashing detected!”.

### 26.3 `errno` (per ora inerte)

`errno` esiste come variabile globale, ma i wrapper non lo settano. Quando vedi un valore negativo da una syscall, gestiscilo localmente. In futuro i wrapper setteranno `errno` come side effect.

### 26.4 `print_hex` e dump manuale

```c
print("addr=");
print_hex((unsigned long)p);
print("\n");
```

`print_hex` scrive esattamente 18 byte (`0x` + 16 cifre esadecimali maiuscole) indipendentemente dalla dimensione.

### 26.5 `ps` e `/sys/bin/proce.c`

Il comando `ps` nella shell è implementato in `user/sys/bin/proce.c`. Usa `SYS_GETPROCS` per ottenere un array di `struct ps_info`:

```c
struct ps_info {
    int pid;
    char name[PROCESS_NAME_MAX];
    int state;       /* 1=CREATED 2=RUNNING 3=SLEEPING 4=ZOMBIE 5=DEAD 6=READY */
    int priority;
    uint64_t cpu_time;
    int on_cpu;
};
```

### 26.6 Esercizio

Scrivi un debugger “povero”: attach al PID dato, manda SIGKILL via `kill_process` quando l’utente preme una certa combinazione di tasti.

---

## Capitolo 27 · Pattern e ricette

### 27.1 Loop di eventi non-bloccante

Il pattern universale per un programma con UI:

```c
int running = 1;
while (running) {
    input_event_t ev;
    while (input_poll_event(&ev)) {
        /* gestisci tutti gli eventi in coda */
    }
    /* logica di gioco / animazione */
    /* disegna */
    compositor_render();
    OS1_sleep(16);  /* ~60 fps */
}
```

### 27.2 Worker IPC con `try_recv`

```c
while (1) {
    struct ipc_message m;
    if (try_recv(-1, &m) == 0) {
        /* gestisci un messaggio */
    }
    /* fai altro lavoro */
    OS1_sleep(50);
}
```

### 27.3 “Daemon” con `recv` bloccante

```c
while (1) {
    struct ipc_message m;
    recv(-1, &m);     /* si blocca finché qualcuno non manda */
    /* processa m */
}
```

### 27.4 Foreground shell job

```c
static void run_foreground(int pid) {
    if (pid <= 0) return;
    while (1) {
        if (window_of_pid(pid) > 0) break;        /* detached */
        if (wait(pid) != -1) break;                /* terminato */
        struct ipc_message m;
        if (try_recv(-1, &m) == 0 &&
            m.type == IPC_TYPE_INPUT && m.data2 != 0 &&
            m.payload[0] == 0x03) {                /* Ctrl+C */
            kill_process(pid);
            break;
        }
        yield();
    }
}
```

### 27.5 Editor di testo minimale

Vedi `kilo` nel bundle: lettura/scrittura file, gestione di un buffer, scrolling, input raw. È un programma da ~1000 righe ed è il modo migliore per vedere tutte le API insieme in un caso reale.

### 27.6 Game loop con timing

```c
unsigned long long last = os1_mono_ns();
while (running) {
    unsigned long long now = os1_mono_ns();
    float dt = (now - last) / 1e9f;
    last = now;

    update(dt);
    render();

    /* mantieni 60 fps */
    unsigned long long target = last + 16 * 1000000ULL;
    while (os1_mono_ns() < target) yield();
}
```

---

## Capitolo 28 · Porting da POSIX: che cosa funziona, che cosa no

OS1 ha una piccola libc POSIX-like. La regola pratica:

| Funzionalità | Stato | Note |
|---|---|---|
| `printf`/`snprintf`/`sscanf` | ✅ | sottoinsieme ragionevole |
| `fopen`/`fread`/`fwrite`/`fclose` | ✅ | emulato sopra `file_*` |
| `open`/`close`/`lseek`/`read`/`write` | ✅ | |
| `malloc`/`realloc`/`calloc`/`free` | ✅ | first-fit, heap monolitico |
| `gettimeofday`/`clock_gettime` | ✅ | monotonic = boot, REALTIME = boot |
| `nanosleep`/`usleep` | ✅ | precisione tick |
| `fork`/`exec` | ❌ | usa `spawn`/`spawn_args` |
| `pipe` | ❌ | IPC diretto |
| `mmap` (file-backed) | ❌ | solo `MAP_ANONYMOUS` (→ malloc) |
| `signal` | ❌ | no-op |
| `select`/`poll` | ❌ | `poll` ritorna 0 |
| Thread (pthread) | ❌ | un solo thread per processo |
| `termios` raw mode | ✅ | no-op (è il default) |
| `stat` | parziale | solo `st_size` e `st_mode` |
| `mkdir`/`unlink`/`rename` | no-op | |

### 28.1 Strategia di porting

1. Includi `<os1.h>` al posto di `<unistd.h>`/`<stdio.h>`/`<stdlib.h>`.
2. Sostituisci `fork`+`exec` con `spawn`/`spawn_args`.
3. Sostituisci `pipe` con IPC diretto.
4. Sostituisci `SIGCHLD` polling con `wait()` non-bloccante.
5. Compila con `-std=c99 -ffreestanding -Wall -Wextra`.
6. Linka `lib.o` e `syscall.o`.

---

## Capitolo 29 · Sicurezza: cosa c’è, cosa manca

### 29.1 Capability già implementate

- `kill_process` verifica che il chiamante possa terminare il target (controllo di capability).
- `registry_write` è per-cap (proprietà della chiave: first-writer-wins).
- `open` con `O_CREAT` richiede `CAP_FS_WRITE`.
- `spawn` richiede `CAP_SPAWN` per il path di destinazione.
- Le syscall window-richiedono `CAP_WINDOW`.
- **Layer a oggetti reale** (ASTRA §7.1): handle non falsificabili a oggetti del kernel, con diritti separabili e attenuabili; l’escalation è impossibile per costruzione (`dup`/`grant` possono solo restringere). Vedi Capitolo 23-bis. Regressioni: `/bin/{captest,capipc,capreg,capkill}`.
- **Preset di privilegio per percorso** (ASTRA §7.2): `/sys/bin` spawna a ROOT, `/bin` a USER, sempre soggetti al creator-clamp monotono; `/sys/bin` è write-protected (binari immutabili).

### 29.2 Cose *non* ancora sicure (TODO noti nel codice)

- La rifinitura **per-servizio** delle capability è un follow-up: oggi i binari di `/sys/bin` partono tutti al preset ROOT, senza ancora una maschera ridotta per singolo servizio.
- L’enforcement VFS read-only (oltre al gate sulle scritture) è ancora da completare.
- Manca ancora il **refactor della call-surface** (DIR-01): non tutte le syscall/verbi legacy sono già unificati sul modello `OS1_`/`OS1low_` + capability (vedi 23-bis e DIR-01).
- Un modello di permessi a livello utente multi-utente (futuro `nxperms`) non esiste ancora.

### 29.3 Buone pratiche utente

- Scrivi i tuoi servizi con il livello di privilegio **minimo necessario**: se un tool non deve scrivere sulla registry, spawnalo con `PLVL_USER` e senza `CAP_REG_WRITE`.
- Non fidarti dei path assoluti dagli argomenti: controlla che siano sotto una directory attesa prima di operare.
- Non chiamare `kill_process` su PID letti dall’esterno senza un controllo di capability. Quando vuoi delegare/limitare l’autorità, preferisci il modello a oggetti: crea un handle `OBJ_TYPE_PROCESS` con solo i diritti necessari e agisci via `OS1_object_ctl(..., OBJ_CTL_KILL, ...)` (Capitolo 23-bis).

---

## Capitolo 30 · Glossario e riferimenti rapidi

### 30.1 Tutti i file descriptor che vedrai

| fd | Nome convenzionale | Significato |
|---|---|---|
| 0 | stdin  | input dal driver tastiera (un byte alla volta) |
| 1 | stdout | output sulla finestra del processo / UART |
| 2 | stderr | come stdout |
| ≥ 3 |       | file aperti con `open` |

### 30.2 Tutti i tipi di evento IPC

| Tipo | Costante | Quando arriva |
|---|---|---|
| 0    | `IPC_TYPE_RAW`    | messaggio applicativo |
| 1    | `IPC_TYPE_INPUT`  | evento tastiera |
| 4    | `IPC_TYPE_MOUSE`  | evento mouse |
| 0x100 | `IPC_TYPE_NOTIFY` | popup di notifica |
| 0x200 | `IPC_TYPE_RESIZE` | cambio dimensione |

### 30.3 Tutti i flag di finestra

| Bit | Significato |
|---|---|
| 1 | top-most |
| 2 | visible |
| 4 | hidden |
| 8 | passive (click-through) |

### 30.4 Funzioni di libreria più usate

| Funzione | Header | Note |
|---|---|---|
| `print` | os1.h | scrive una stringa su stdout |
| `printf` | stdio.h | formattazione, scrivi su stdout |
| `printf_win` | os1.h | scrive formattato su una finestra |
| `sprintf`/`snprintf` | stdio.h | formattazione su buffer |
| `malloc`/`free`/`realloc`/`calloc` | stdlib.h | heap |
| `read`/`write` | unistd.h | fd basso livello |
| `fopen`/`fread`/`fwrite`/`fclose` | stdio.h | fd alto livello |
| `spawn`/`spawn_args`/`spawn_caps` | os1.h | processi |
| `kill_process`/`wait`/`exit` | os1.h | processi |
| `send`/`recv`/`try_recv` | os1.h | IPC |
| `create_window`/`destroy_window` | os1.h | finestre |
| `window_draw`/`window_blit` | os1.h | disegno |
| `compositor_render` | os1.h | pubblica |
| `input_poll_event` | input.h | tastiera/mouse/resize |
| `OS1low_handle_create`/`_duplicate`/`_close` | os1.h (object.h) | handle a oggetti del kernel |
| `OS1low_cap_query`/`_grant` | os1.h (object.h) | interroga/concede i diritti di un handle |
| `OS1_object_read`/`_write`/`_wait`/`_ctl` | os1.h (object.h) | I/O e controllo uniforme di un oggetto |
| `OS1_window_enum`/`_minimize`/`_restore`/`_focus`/`_close` | os1.h (object.h) | window manager (dock) |
| `registry_read`/`registry_write` | os1.h | configurazione |
| `file_read`/`file_write`/`list_dir` | os1.h | file system |
| `OS1_sleep` | os1.h | sleep in ms |
| `nanosleep`/`usleep` | time.h, unistd.h | POSIX-like |
| `clock_gettime`/`os1_mono_ns`/`os1_cpu_ns` | time.h, os1.h | tempo |
| `font_load`/`font_draw_string` | font_lib.h | tipografia |
| `graphics_load_image` | graphics.h | immagini |

### 30.5 Comandi utili della shell OS1

```
help, clear, time, demo, demo3d, shell, ps, ls, cd, pwd,
cat, kill, exec, about, exit
```

`nxres` è un tool per cambiare risoluzione/stile del desktop (vedi `user/sys/bin/nxres.c`).

### 30.6 Errori comuni

1. **Dimenticare il `;** alla fine di un’istruzione. Errore del compilatore.
2. **Confondere `=` con `==**`: il primo assegna, il secondo confronta.
3. **Array fuori dai limiti**: silent corruption. Abilita `-fsanitize=address` se disponibile.
4. **Non liberare la memoria**: leak. OS1 non restituisce pagine al kernel; in un processo lungo, esaurisci l’heap.
5. **Chiamare `write(fd, ...)` su un fd inesistente**: ritorna `-EBADF`. Controlla sempre il valore di ritorno.
6. **Confondere `printf` con `printf_win`**: il primo scrive su stdout, il secondo vuole un id di finestra.
7. **Blocking read da fd sbagliato**: `read(99, ...)` blocca per sempre. Usa `try_recv` per polling.

---

## Capitolo 31 · Conclusione

Hai ora tutte le basi per scrivere programmi userland su OS1/NEXS:

- Sai cos’è un programma, un processo, un file descriptor, una syscall.
- Conosci le convenzioni di chiamata su AArch64 e amd64.
- Conosci le tre famiglie di API: base proprietaria (`os1.h`), POSIX-like (`<stdio.h>`, `<unistd.h>`, `<time.h>`, …), e di basso livello (`_sys_*`).
- Sai come funzionano processi, IPC, finestre, input, file system, registry, memoria, tempo.
- Sai come strutturare un programma (loop di eventi, worker IPC, foreground job, game loop).
- Conosci i limiti del porting da POSIX.

Da qui in poi è pratica. Buoni tre punti di partenza:

1. **Scrivi un editor di testo minimale** sul modello di `kilo` (vedi bundle).
2. **Scrivi un piccolo gioco 2D** (vedi `demo3d` per il pattern).
3. **Porta un programma POSIX semplice** (un `ls` con `-l` per esempio).

Tutto il codice che vedi in `user/sys/bin/` e `user/bin/` è leggibile, commentato, e dimostra pattern reali. Leggerlo è il modo migliore per imparare il resto.

Buon coding. 🛠️
# nxec — Userland Image, VFS e namespace

**ID:** SPEC-VFS-USERLAND-001  
**Versione:** 0.1.0-DESIGN  
**Stato:** DESIGN / FORMAT COMPARISON REQUIRED

## 1. Separazione kernel/userland

La VFS completa non appartiene al Core MACHINE. Il kernel mantiene mount registry/authority, namespace, capability, page/buffer objects e syscall/REG endpoint minimi. Il VFS service userland implementa path lookup, filesystem drivers, metadata, cache, journaling e compatibility. Non può emettere capability che il kernel non abbia concesso.

NXUI, nome provvisorio della nxec Userland Image, è separata dall'immagine kernel. Il kernel la verifica e la monta come root image read-only; overlay scrivibili sono volumi distinti.

## 2. Formati a confronto

| Formato | Punti forti | Limiti |
|---|---|---|
| cpio newc | semplice, sequenziale, adatto initramfs | indici e metadata limitati |
| tar/ustar/pax | noto, tool diffusi | parser/semantica ampia, seek non ideale |
| SquashFS | read-only, compressione, metadata/index | parser più complesso e licenza da valutare |
| NXFS custom | manifest/capability native | formato nuovo, rischio bug/manutenzione |

Il riferimento a root image temporaneo tipo seL4/initramfs è un modello di boot, non autorizza copiare codice o assumere equivalenza. Confrontare footprint parser, random access, compressione, integrità, tooling e licenze prima di scegliere.

## 3. NXUI

Contiene manifest/schema, filesystem base read-only, indice file/dir, ELF executable e shared libraries, moduli userland, symlink/metadata limitati, policy owner/mode, digest e firma. Non contiene dati personali scrivibili. User data e log persistenti sono volumi separati.

Il kernel riceve NXUI come immagine distinta, la verifica prima del mount e usa REG tra mount authority e VFS service. Il Core non delega al VFS il diritto di montare risorse arbitrarie.

## 4. Namespace

Namespace iniziali:
- /machine: non visibile alla userland ordinaria; diagnostica tramite capability ristrette.
- /system: servizi e runtime SYSTEM.
- /root: sandbox amministrativa.
- /users/<id>: home identità.
- /shared/<name>: oggetti condivisi con grant.
- /dev: proxy REG dei device, non MMIO diretto.
- /proc e /sys: viste sintetiche capability-scoped.
- /tmp: volatile e quota-bounded.

Il namespace è vista costruita da mount capability. Path assoluto non è globale.

## 5. Mount dati utente

Ogni utente riceve mount persistente dedicato data/sdN. sdN è ID volume/mount. Registry associa identity, mount ID e generation. Il processo scrive solo con capability del mount e subtree concesso. Symlink, hardlink, bind mount e rename non attraversano namespace senza policy. Unmount blocca nuovi accessi e gestisce handle attivi secondo lifetime.

Il mount fisico può essere esposto come /users/<id>; il path fisico non viene reso autorità.

## 6. Symlink e linking

VFS risolve symlink nel namespace del processo con hop limit, cycle detection, capability check a ogni crossing e regole esplicite per absolute/relative link. Shared libraries sono risolte dal dynamic linker nel namespace del processo. Path non è autorità: loader verifica file capability, hash, ABI, firma e symbol version.

## 7. Compatibility layers

Layer selezionabili per processo/namespace: ABI personality/syscall translation, ELF loader/dynamic linker namespace, libc/runtime, driver compatibility (libriver), file/path semantics e thread/process model. Attivazione richiede ABI, dipendenze, rights e risorse compatibili. Nessun layer modifica capability policy o eredita privilegi dell'host.

## 8. Recovery e test

Error classes: path, namespace, capability, metadata, media, journal, service e kernel. Ogni classe ha codice stabile, retry budget, escalation e audit.

Test: symlink/mount escape, hardlink/rename cross-tenant, revoke durante I/O, unmount con handle attivi, signature/hash failure, metadata corrotti, sdN isolation, shared grants, VFS crash/restart, journal replay, symbol collision, compatibility switching e overlay quota-bounded.

## 9. Gate

Non congelare NXUI/NXFS prima del confronto cpio newc, tar/pax e SquashFS. Verificare fonti primarie sul modello initial root image seL4 prima di dichiarare equivalenza. Il kernel non incorpora filesystem parser complesso prima della decisione.


## 8. Riferimento seL4: root task e BootInfo, non VFS nel kernel

Il riferimento seL4 va interpretato con precisione: al boot il kernel passa il controllo a un root/initial task e fornisce BootInfo, capability e descrittori di memoria untyped. Il root task gestisce l'allocazione di oggetti kernel tramite capability untyped e avvia i servizi userland. Questo è il modello da studiare per il bootstrap e la delega esplicita delle risorse.

Non si assume che seL4 fornisca un filesystem root temporaneo nel kernel. Il filesystem/VFS e la root image sono responsabilità dei componenti userland o di un root task/initial service. nxec mantiene quindi kernel image e userland/root image separate; il kernel concede le capability iniziali e il servizio SYSTEM monta la seconda immagine.

BootInfo non è un'autorità implicita: ogni range untyped, device untyped, IRQ e capability deve essere convertito in oggetti/diritti espliciti nel nostro registry. Il modello di seL4 è un riferimento architetturale, non un formato immagine da copiare.

Riferimenti primari: seL4 BootInfo and initial task: https://docs.sel4.systems/Tutorials/libraries-1.html; capability and untyped memory: https://docs.sel4.systems/Tutorials/capabilities.html; untyped memory allocation: https://docs.sel4.systems/projects/rust/tutorial/root-task/kernel-objects.html

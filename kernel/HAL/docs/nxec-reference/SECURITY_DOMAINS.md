# nxec — Security Domains, Principals e Capability

**ID:** SPEC-SEC-DOMAIN-001  
**Versione:** 0.1.0-DESIGN  
**Stato:** DESIGN; richiede threat model e test formali.

## 1. Principio

Ogni entità hardware/software è oggetto tipizzato nel registry e protetto da capability. Il modello uniforme non appiattisce i livelli di fiducia: tipo, principal, dominio, label, diritti, generation, lifetime e policy restano distinti. Un nome di dominio, UID, firma valida o registry entry non concede accesso.

## 2. Domini

**MACHINE:** codice privilegiato e controllo hardware: HAL, hypervisor control, MMU, interrupt, timer, CPU state, DMA/IOMMU e primitive sicurezza. Non accessibile direttamente da userland.

**SYSTEM:** servizi di sistema userland privilegiati. Invoca syscall/endpoint solo con capability. Driver userland possono ricevere capability device e API SYSTEM limitate. SYSTEM non è ring zero.

**ROOT:** sandbox userland amministrativa. Può ottenere capability amministrative specifiche, revocabili e auditate; nessun accesso implicito a MACHINE.

**USER:** sandbox per identità utente, sessione e namespace. Scrittura limitata ai mount concessi.

**SHARED:** sandbox/namespace per oggetti condivisi con policy esplicita di read/write, ownership, lifetime e revoca. Non significa accesso universale.

**DRIVER:** classe moduli userland con capability device, range MMIO, interrupt, DMA e operazioni specifiche. Non ottiene MACHINE perché è driver.

## 3. Mapping ISA

AMD64: CPL0 kernel, CPL3 userland; VMX root/non-root è stato ulteriore e non coincide con CPL. Ring 1/2 non sono sandbox ordinarie.

ARM64: EL3 firmware/secure monitor se presente; EL2 hypervisor se disponibile; EL1 kernel; EL0 userland. Secure/Non-secure è asse separato. Non chiamare letteralmente ARM64 ring 0. La mappatura concreta è per backend.

## 4. Capability

Capability contiene object ID, generation, rights mask, scope, owner/delegate chain, policy version, revocation epoch, quota/deadline e audit provenance. L'handle può esporre index, generation e rights hint; i diritti effettivi sono kernel-authoritative. Rights nell'handle non sono autorità autonoma.

## 5. Risorse

CPU/thread/scheduling budget; frame fisici, mapping e page tables; buffer/shared memory; MMIO, port I/O, interrupt, timer; DMA/IOMMU; device/driver; process/module/ELF/library; REG endpoint/queue; mount/file/directory/namespace; user identity/session; clock/entropy/debug/audit.

API comuni: lifecycle, query, delegate, revoke, map/unmap e accounting. Operazioni specifiche restano per tipo.

## 6. Delega

Delega non amplia diritti. La child capability è sottoinsieme di rights, scope, risorse, quota e durata del parent. Revoca diretta o parent lineage invalida i discendenti con semantica deterministica.

## 7. Separazione

MACHINE non accessibile da userland; SYSTEM espone syscall e servizi userland con capability; ROOT amministra solo capability concesse; USER accede a risorse personali; SHARED richiede grant; DRIVER ha device capability e IPC SYSTEM.

Syscall non è bypass: ogni syscall esegue capability check. Nessuna syscall accetta puntatore arbitrario in address space altrui.

## 8. Sandbox

Ogni sandbox possiede address space/page-table root, namespace REG/VFS, capability table, quota CPU/memoria/buffer/queue, policy device/network, audit stream e lifecycle/recovery. Shared memory è oggetto esplicito con mapping per principal e diritti per pagina/range. Revoke blocca nuovi accessi prima della reclamation.

## 9. Multiutente

Ogni identità ha namespace privato e mount dati dedicato, ad esempio data/sdN. sdN è ID volume, non indice derivato dal nome. Registry associa user identity, mount ID e generation. Path non è autorità. Symlink, rename, bind mount e hardlink non possono attraversare namespace senza policy.

Root/System/User/Shared sono namespace separati. Servizi SYSTEM non scrivono home utente senza capability temporanea esplicita.

## 10. Revoca

Dal linearization point non si autorizzano nuove operazioni. Operazioni in corso rivalidano ai boundary di accesso e commit. Cleanup usa capability distinta. Propagazione cross-core tramite epoch/fence; snapshot stale non autorizza. CPU offline/non responsive richiede containment.

## 11. Threat model

Attori: processo compromesso, driver malevolo, modulo ELF non firmato, utente locale, servizio SYSTEM compromesso, DMA ostile, firmware difettoso, HCD/ELF/REG/VFS ostile, CPU eterogenea, hypervisor esterno, memoria corrotta e replay/race cross-core.

Asset: trust keys, kernel, page tables, capability tables, dati utenti, MMIO, DMA, interrupt, mount e audit. Obiettivi: confidenzialità, integrità, isolamento, availability quota-bounded, revocation correctness e provenance.

## 12. Gate

Formalizzare representation, derivation, delegation, revocation linearization, epoch, DMA threat model, identity lifecycle, root/system authority e audit. Test property-based/model checking: forgery, ABA, revocation race, confused deputy, privilege escalation e cross-tenant access.

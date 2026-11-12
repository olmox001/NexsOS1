# nxec — REG: Resource and Event Protocol

**ID:** SPEC-REG-001  
**Versione:** 0.1.0-DESIGN  
**Stato:** DESIGN / FORMALIZATION REQUIRED

## 1. Principio

REG è protocollo comune per nominare risorse, richiedere operazioni, pubblicare eventi, trasferire capability e condividere buffer. È logicamente comune ma fisicamente distribuito: registry canonico, shard per dominio/CPU e snapshot locali versionati.

REG non interpreta codice. Un frame descrive un'operazione autorizzata; payload contiene dati tipizzati o riferimenti a buffer. Codice nativo è caricato solo dal Module Loader dopo verifica ELF, manifest, firma/policy e capability.

## 2. Modello uniforme

Ogni oggetto ha object_type, schema_version, object_id, generation, owner principal, lifecycle state, compatibility class K0-K6, security label, policy, capability reference, parent IDs e accounting/budget.

Oggetti: frame fisico, mapping virtuale, buffer, thread, CPU, interrupt, device, endpoint, modulo, processo, mount, file, transazione e operazione. L'header comune non cancella semantica specifica: ogni tipo ha schema e operazioni ammesse distinti.

Handle opachi index+generation+rights hint o capability token. Nessun client dereferenzia handle come puntatore. Il Capability Engine valida l'autorità.

## 3. Livelli

1. Wire: framing, version, message type, lengths, sequence, error code.
2. Session: negoziazione versione, principal, endpoint, quota.
3. Capability: rights, generation, scope, revocation epoch.
4. Operation: semantica tipizzata.
5. Transport: shared-memory rings, doorbell/event, local fast path.
6. Registry: canonical index, shard, snapshot e audit.

Cambiare transport non cambia autorizzazione o semantica.

## 4. Request envelope

Campi da fissare nel layout wire: protocol major/minor; header/total length; message kind/opcode; flags reserved-zero; session/endpoint ID; request sequence/correlation ID; object capability; expected generation; required rights; payload descriptor inline bounded o shared-buffer capability+offset+length; deadline class; idempotency key per side effects; trace/audit context.

Tutti i campi hanno larghezza fissa e endianness definita. Offset e length sono verificati contro la dimensione del buffer capability. Nessun cast arbitrario su input non allineato.

## 5. Stati e completion

Stati terminali: COMPLETED, FAILED, CANCELLED, EXPIRED, REVOKED e PARTIAL solo se definito dall'operation schema.

Distinguere ACCEPTED, STARTED, COMMITTED, COMPLETED. Timeout non equivale a cancellation. La risposta riporta request sequence, operation ID, object generation, stato, errore stabile, quantità completata e result references.

Le operazioni transazionali definiscono commit point, idempotency e rollback. Non promettere exactly-once universale attraverso crash; definire deduplication bounded e semantica retry.

## 6. Push, eventi e CANCEL

Il client invia request; il server pubblica completion/eventi su endpoint registrati. Subscription e notifiche sono capability-scoped e bounded.

CANCEL è ispirato alla semantica Tflush di Plan 9 9P: cancellare una risposta pendente non annulla retroattivamente un effetto commesso. Il client attende ack; se completion precede l'ack, accetta il completamento. Esporre TOO_LATE/ALREADY_COMMITTED.

Cancellazione cooperativa fino al safe point definito. Operazioni hardware non interrompibili hanno stop/containment contract e limite dichiarato.

## 7. Shared memory e zero-copy

Trasporto primario: ring buffer preallocati e shared memory con mapping capability-controlled. I descrittori contengono offset/length, mai puntatori virtuali di altro address space.

Ogni buffer dichiara owner/borrower, generation, rights, lease state, range, cache coherency, DMA/IOMMU policy e reclamation condition. Zero-copy ammesso solo con alignment, coherency, lifetime, ownership e rights dimostrati. Altrimenti copia bounded da pool autorizzato o rifiuto/backpressure. DMA richiede IOMMU o bounce buffer controllato.

## 8. Backpressure e parallelismo

Code bounded con crediti e contatori overflow. Esauriti i crediti: WOULD_BLOCK o evento credit availability; mai coda infinita.

Code per CPU/endpoint riducono contesa globale. Registry canonico è autorità, non mutex globale hot path. Operazioni locali usano snapshot immutabili e epoch; capability rivalidata al punto di accesso e commit.

Formalizzare priorità, deadline, quota principal, fairness, starvation bound, retry budget, timeout, CPU offline e overflow. Un loop async non è automaticamente bounded.

## 9. Coerenza e revoca

Tecniche da confrontare per tipo: RCU, epoch reclamation, hazard pointers, refcount, MVCC, immutable snapshot e seqlock. Nessuna è universale. Seqlock non protegge lifetime; RCU/epoch possono trattenere memoria; refcount può creare contesa; MVCC non sostituisce revoca.

La revoca ha linearization point autorevole. Snapshot locale obsoleta non può autorizzare: verificare epoch/generation al punto protetto. CPU offline/non responsive richiede containment.

## 10. Sicurezza

Capability unforgeable; session binding; replay protection; bounds-first parsing; no arbitrary pointer chasing; rate limit/quota/audit; tenant isolation; no executable code in ordinary payload; cancellation distinta da revocation; errori senza leak di indirizzi kernel o dati altrui.

## 11. Operation classes

RESOURCE_QUERY, CREATE/DESTROY, MAP/UNMAP, BUFFER_LEASE/RETURN, THREAD_CONTROL, DEVICE_SUBMIT/COMPLETE, MODULE_LOAD/UNLOAD, MOUNT/UNMOUNT, SUBSCRIBE/UNSUBSCRIBE, CANCEL e REVOKE. Ogni operation schema definisce side effects, commit point, idempotency e failure modes.

## 12. Metriche e fonti

Misurare enqueue/dequeue cycles, doorbell cost, request→accept/start/commit/completion, percentili, MAX_OBSERVED, queue depth, starvation, cache miss, context switch, zero-copy bytes, fallback copies, revocation propagation e memory retained. Nessun MAX_OBSERVED è WCET.

Confronti: Plan 9 9P2000, L4 IPC/capability, shared-memory rings, virtio queues e async models. 9P ispira framing/cancellation ma non capability authority né memory model.

Fonti: https://9fans.github.io/plan9port/man/man9/ ; https://docs.oasis-open.org/virtio/virtio/

**Gate:** wire layout, atomic memory model, ring algorithm, cancellation proof, token representation e reclamation policy OPEN.


## 8. Semantica cancellation derivata da 9P Tflush

Plan 9 9P2000 Tflush è un riferimento utile per l'annullamento di richieste asincrone: il client invia un tag nuovo che identifica il tag precedente; il server non deve emettere la risposta originale se non l'ha già emessa e risponde Rflush; il client attende Rflush prima di riutilizzare il tag. REG adotta il principio di drenaggio/acknowledgement, non copia il namespace 9P né la sua authority.

REG CANCEL contiene request_id + generation, non un semplice tag riutilizzabile. La risposta è CANCELLED, TOO_LATE, ALREADY_TERMINAL o DENIED. CANCELLED significa che l'endpoint garantisce che non avverranno ulteriori effetti esterni della richiesta; se la richiesta ha già superato il commit point, il server risponde TOO_LATE e restituisce/consente di recuperare la completion originale. L'ID non viene riutilizzato finché non è risolta la terminal state. La cancellazione non promette rollback di operazioni irreversibili.

Se completion e cancellation corrono in parallelo, entrambe sono correlate allo stesso generation. Il client non deve liberare il buffer condiviso o riutilizzare request_id finché riceve la terminal resolution. Le capability vengono rivalidate prima di commit e accessi a risorse protette; cancellation non sostituisce revocation.

Riferimento primario: 9P2000 protocol, Tflush/Rflush: https://9p.io/sys/man/5/0intro

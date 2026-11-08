# nxec — Boot Bundle e Kernel Image Specification

**ID:** SPEC-BOOT-IMAGE-001  
**Versione:** 0.1.0-DESIGN  
**Stato:** DESIGN / COMPARISON REQUIRED

## 1. Scopo

Definire un artefatto kernel precompilato che il preboot verifichi, decomprima, carichi e consegni al suo entry ELF. Contenitore, ELF, firma, compressione, indirizzi fisici e virtuali sono livelli distinti.

## 2. Alternative

| Livello | Opzione | Vantaggio | Costo/rischio |
|---|---|---|---|
| Contenitore | ELF diretto | standard | non descrive bundle, firma e componenti multipli |
| Contenitore | cpio/tar | tool diffusi | manifest, firma e indici da aggiungere |
| Contenitore | NXB versionato | policy e componenti espliciti | formato nuovo da mantenere |
| ELF | ET_EXEC | loader semplice | virtual base fissa |
| ELF | ET_DYN static PIE | base rilocabile | relocation whitelist complessa |
| Compressione | none | decoder nullo | immagine più grande |
| Compressione | LZ4 frame | velocità e blocchi indipendenti | rapporto inferiore a codec più lenti |
| Compressione | Zstandard | rapporto elevato | decoder/workspace più complessi |

La decisione richiede benchmark AMD64/ARM64, footprint decoder, workspace massimo, latenza e throughput.

## 3. NXB — modello proposto

Nome provvisorio: NXB1, nxec Boot Bundle. Non è un formato congelato. Contiene header/version, manifest canonico, component table, payload kernel ELF, eventuali moduli trusted, riferimenti a userland image separata e blocco firma.

Kernel e userland sono artefatti distinti. Il bootloader può consegnare entrambi, ma il kernel mantiene l'autorità di verificare e montare userland.

Header richiesto: magic, major/minor, header_size, total_size, target ISA/ABI, flags, manifest offset/size, component table offset/count/entry size, signature offset/size, build ID, monotonic version e ID algoritmo digest/firma. Campi little-endian a larghezza fissa, reserved zero, bounds overflow-safe.

Ogni component record dichiara ID, kind, ISA/ABI, K-class, offset/dimensione compressa, dimensione decompressed massima, algoritmo/parametri, digest compresso e decompresso, alignment, policy caricamento, dipendenze e trust flags.

## 4. Catena di fiducia

Il preboot deve essere autenticato dal percorso Secure Boot o policy equivalente. La firma copre manifest canonico e digest di ogni componente. CRC e checksum rilevano corruzione, non autenticità.

Trust store: key ID, algoritmo, uso, periodo, stato revoca, versione minima, chiavi recovery separate, rotazione e revoca. SHA-256 ed Ed25519 sono candidati, non decisioni congelate: verificare standard, implementazione constant-time, key lifecycle, Secure Boot chain e licenze.

Anti-rollback distingue TPM NV, UEFI authenticated variables, contatore hardware e contatore software. Se manca una radice monotona affidabile, non dichiarare anti-rollback hardware.

## 5. Compressione

Decoder su blocchi con dimensione massima dichiarata e validata prima dell'uso. Workspace preallocato; output size non può superare limite componente o riserva. LZ4 Frame con block independence è candidato per workspace bounded e parallelizzazione futura; xxHash/checksum LZ4 non sostituisce digest crittografico del contenuto decompresso.

Confrontare blocchi 64 KiB, 256 KiB e 1 MiB, LZ4/Zstandard/none, footprint, memoria massima, throughput, latenza e cicli/byte. Nessuna dimensione è congelata senza benchmark.

## 6. ELF kernel loader

Il loader usa program headers, non sezioni di debug. Verifica ELF class, endianness, machine, type, ABI, header size, numero PHDR, PT_LOAD, p_filesz<=p_memsz, range file/memoria, alignment e congruenza p_offset/p_vaddr, permessi W^X, entry dentro segmento executable, assenza PT_INTERP, PT_DYNAMIC e relocation whitelist. Nessun symbol lookup non autorizzato e nessuna relocation su pagine read-only dopo sealing.

### 6.1 Relocation profiles

KFIXED: ET_EXEC a virtual base definita, caricamento fisico indipendente e mapping al virtual address previsto.

KPIE: ET_DYN static PIE con relocation relative limitate e base casuale da entropy verificata. Richiede specifica ISA per R_X86_64_RELATIVE e R_AARCH64_RELATIVE, addend, overflow, RELA/REL, conteggi, symbol table e W^X.

Entrambi restano in valutazione fino a prototipo e confronto sicurezza/prestazioni. Non accettare relocation arbitrarie.

## 7. Mapping e ownership

Il preboot calcola output size, riserva frame e mapping. La decompressione avviene in regione temporanea NX; digest, ELF validation e relocation precedono i permessi finali. Una pagina non deve essere writable ed executable contemporaneamente.

Non sovrascrivere preboot, HCD, firmware runtime, MMIO o buffer in uso. Il BHD dichiara owner/lifetime di ogni regione. Il kernel non presume che il preboot abbia già liberato memoria.

## 8. Boot Handoff Descriptor

Entry ABI definita, non salto al primo byte. BHD indica: CPU state, stack, page tables, MMU state, cache/coherency, interrupt state, HCD, memory map, kernel base/segment map, bootloader provenance, firmware state, entropy provenance, console/debug endpoint e moduli.

Il kernel rivalida BHD, HCD e segmenti. Il BHD non è capability e non autorizza dereferenziazione di indirizzi fisici.

## 9. Preboot reclaim/resident

Default: reclaim codice preboot dopo verifica handoff, ricostruzione mapping e conferma ownership. Conservare audit immutabile. Modalità diagnostica futura può mantenere pagine read-only, non eseguibili da userland e senza capability hardware implicite. Il demone error-prevention è SYSTEM userland; un monitor privilegiato è componente kernel separato.

## 10. Test

Bundle malformed, offset overflow, directory overlap, component unknown required, digest/signature invalidi, key revoked/rotation/rollback, decompression bomb, truncation, ELF PHDR/PT_LOAD invalidi, relocation non whitelist, entry non executable, W^X, memoria insufficiente, CPU heterogeneity, handoff incompleto, firmware invalidi, cancellation e QEMU AMD64 BIOS/UEFI + ARM64 UEFI.

## 11. Fonti

ELF gABI: https://refspecs.linuxfoundation.org/elf/elf.pdf  
ELF Program Header: https://refspecs.linuxfoundation.org/elf/gabi4%2B/ch5.pheader.html  
UEFI 2.11: https://uefi.org/specs/UEFI/2.11/  
LZ4 Frame: https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md  
GRUB 2.14: https://www.gnu.org/software/grub/manual/grub/grub.html

**Gate:** NXB layout, manifest canonicalization, trust store, compression profile, relocation whitelist e BHD ABI restano OPEN.

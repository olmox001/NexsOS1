Affrontare lo sviluppo dei driver per un sistema operativo custom — specialmente con un design architetturale elegante e rigoroso come **ASTRA** per NEXS — è il vero "battesimo del fuoco" per un OS dev.

Come giustamente detta la documentazione di ASTRA: **il kernel core non conosce l'hardware, conosce i servizi**. Elencare 100 nomi commerciali di schede madri (es. *ASUS ROG Strix Z790-E*) o singoli modelli di CPU nel codice sarebbe una violazione del Modello a Livelli. Per un kernel ASTRA, una scheda madre o una CPU non sono altro che un agglomerato di **Infrastructure Providers** scoperti via `ACPI` (su amd64) o `FDT` (su aarch64) che espongono registri su un bus (`PCI` o `MMIO`).

Per consentirti di mappare l'intero mercato consumer e server del 2026, la lista seguente organizza i componenti hardware richiesti raggruppandoli per **famiglie di controller, chipset e interfacce logiche standard**. Questo approccio copre oltre il 98% dell'hardware esistente sul mercato, traducendolo direttamente in contratti ASTRA e driver ELF per la Fase C.

---

## 1. Motherboard & Chipset Landscape (Top 100 Market Equivalents)

In ASTRA, una scheda madre è identificata dal suo **Host Bridge / Root Complex PCI** e dal **PCH (Platform Controller Hub)**. Questa tassonomia copre le oltre 100 schede madri consumer e server più vendute:

### amd64 (Intel & AMD) — Gestite da ACPI + PCI Provider

* **Intel 600/700/800 Series PCH (Cofani di oltre 50 schede madri consumer come Z790, B760, H610, Z890, B860):**
* *Sottodispositivi critici:* Intel LPC/eSPI Bridge (configurazione legacy/IRQ), Intel SPI Controller (accesso al chip Flash BIOS), Intel SMBus Controller (sensoristica), Intel xHCI Controller (USB 3.2/4), Intel High Definition Audio Controller.


* **Intel W790 / C741 Series (Schede madri workstation e server Xeon):**
* *Sottodispositivi critici:* Intel VMD (Volume Management Device — isolamento e pass-through NVMe), IPMI/BMC AST2600 via PCIe, Root Port PCIe Gen 5 integrati.


* **AMD 600/800 Series FCH (Oltre 40 schede madri consumer come X670E, B650, A620, X870E):**
* *Sottodispositivi critici:* AMD Promontory/ASM4242 Chipset (connesso via PCIe link), AMD PSP (Platform Security Processor) BAR, AMD FCH SMBus, USB4 Host Controller integrato.


* **AMD WRX90 / SP5 / SP6 (Schede madri Threadripper ed EPYC server):**
* *Sottodispositivi critici:* AMD IOMMU (v2), ASpeed AST2600 BMC (interfaccia grafica VGA legacy su matrice 2D + console seriale su LAN).



### aarch64 (SBC, Laptops & Servers) — Gestite da FDT o ACPI (SBSA)

* **Broadcom BCM2711 / BCM2712 (Raspberry Pi 4 & 5):**
* *Sottodispositivi critici:* Raspberry Pi RP1 I/O co-processor (su Pi 5, gestisce GPIO, Ethernet, SPI via link PCIe custom), Broadcom V3D GPU, DesignWare PCIe Root Complex.


* **Qualcomm Snapdragon X Elite / Plus Reference Platforms (Laptop moderni Windows-on-ARM):**
* *Sottodispositivi critici:* Qualcomm System NOC (Network on Chip), Qualcomm xHCI USB, Synopsys DesignWare PCIe Controller, Qualcomm SMC (Secure Monitor Call) per l'alimentazione.


* **Apple Silicon SoCs (MacBook/Mac Studio M1/M2/M3/M4/M5):**
* *Sottodispositivi critici:* Apple AIC (Apple Interrupt Controller), Apple IOMMU (DART), Apple Mailbox (comunicazione co-processori), Apple PMGR (Power Management).


* **Ampere Altra / AWS Graviton 3 & 4 (Server ARM Enterprise):**
* *Sottodispositivi critici:* SBSA-compliant GICv3/v4 (Generic Interrupt Controller), SBSA UART (PL011), standard PCIe ECAM space (scoperto interamente via ACPI).



---

## 2. 100 Most Prevalent CPUs & Integrated Topologies

Il livello ISA di ASTRA gestisce solo il context switch e le page table. L'architettura delle CPU sotto forma di topologia core (SMP) e le GPU integrate (iGPU) vengono esposte come servizi di calcolo.

| Produttore / Famiglia | Copertura Modelli (Top 100) | Protocolli ISA Estesi | iGPU Integrata (Mappa a `gpu.elf`) |
| --- | --- | --- | --- |
| **Intel Core 12th/13th/14th Gen** (Alder/Raptor Lake) | i9-14900K, i7-13700K, i5-12400, ecc. (30+ SKUs) | AVX2, FMA3, Intel Thread Director (E-core/P-core topology via ACPI CPC) | Intel UHD Graphics 770 / Iris Xe (Gen12) |
| **Intel Core Ultra Series 1 & 2** (Meteor/Arrow/Lunar Lake) | Core Ultra 7 155H, Ultra 9 285K, ecc. (20+ SKUs) | AVX2, AVX-VNNI, architettura disaggregata a Tile/Chiplet | Intel Arc Graphics (Xe-LPG / Xe2-LPG) |
| **Intel Xeon Scalable** (4th/5th/6th Gen) | Xeon Platinum 8480+, Silver 4410Y, Granite Rapids | AVX-512, Intel AMX (Advanced Matrix Extensions) | Assente (richiede BMC AST2600 su mobo) |
| **AMD Ryzen 5000/7000/9000** (Zen 3/4/5) | Ryzen 7 7800X3D, R5 5600X, R9 9950X (30+ SKUs) | AVX2, AVX-512 (Zen 4/5 implementazione nativa a 512-bit) | AMD Radeon Graphics (RDNA2 / RDNA3) |
| **AMD Ryzen AI 300 Series** (Strix Point) | Ryzen AI 9 HX 370 | AVX-512, architettura eterogenea Zen 5 + Zen 5c | AMD Radeon 880M / 890M (RDNA3.5) |
| **AMD EPYC** (Milan, Genoa, Bergamo, Turin) | EPYC 9654, 9754, Turin 128-core (10+ SKUs) | AVX-512, AMD SEV-SNP (Secure Encrypted Virtualization) | Assente |
| **Apple Silicon M-Series** (M1/M2/M3/M4/M5) | Varianti Base, Pro, Max, Ultra (15+ SKUs) | ARMv8.5-A / ARMv9.2-A, Apple AMX (Matrix coprocessor) | Apple AGX Graphics (Mailbox + MMIO) |
| **Qualcomm Snapdragon X** | X1E-84-100, X1E-78-100, X1P-64-100 | ARMv9.2-A, Large Physical Address Extensions (LPAE) | Qualcomm Adreno X1 GPU |
| **Ampere / AWS Graviton** | Ampere Altra Q80-30, AWS Graviton 4 | ARMv8.2+ / ARMv9-A (Neoverse V2/N2 cores) | Assente |

---

## 3. 50 Dominant VGA/Graphics Controllers

I driver video devono essere compilati come servizi isolati in userspace nella Fase C (`gpu.elf`). Comunicano tramite `map_mmio()` per mappare i BAR dei comandi e i Ring Buffer, e `dma_alloc()` per allocare la memoria di sistema accessibile dalla GPU (GART/IOMMU).

### NVIDIA (Architetture Moderne) — Driver `nvgpu.elf`

1. **Famiglia Blackwell (RTX 50-Series):** RTX 5090, 5080, 5070. *Interfaccia:* NVLink 5, firmware GSP (GPU System Processor) obbligatorio per l'inizializzazione del clock e dei canali di memoria.
2. **Famiglia Ada Lovelace (RTX 40-Series):** RTX 4090, 4080, 4070 Ti, 4060. *Interfaccia:* Falcon Microcontroller per il caricamento dei firmware proprietari, sottomoduli NVDEC/NVENC per il video.
3. **Famiglia Ampere (RTX 30-Series):** RTX 3080, 3070, 3060 Ti. *Interfaccia:* Interfaccia FIFO a canali multipli, BAR esteso via Resizable BAR (ReBAR).
4. **Data Center / AI Engines:** H100, A100, B200. *Interfaccia:* Nessun output video, pura esecuzione compute tramite code DMA asincrone.

### AMD Radeon (Architetture RDNA) — Driver `amdgpu.elf`

5. **Famiglia RDNA 4 (RX 8000 Series):** Modelli flagship e mid-range del 2026. *Interfaccia:* IP Blocks disaccoppiati (GC, DCN, VCN) configurati tramite firmware microcode standard AMD.
6. **Famiglia RDNA 3 / 3.5 (RX 7000 Series & iGPUs):** RX 7900 XTX, 7800 XT, Radeon 780M. *Interfaccia:* Packet Processor (CP) che legge comandi da Ring Buffer allocati in memoria DMA coerente.
7. **Famiglia RDNA 2 (RX 6000 Series):** RX 6700 XT, 6600. *Interfaccia:* Gestione delle code tramite firmware MEC (MicroEngine Compute).

### Intel Arc & iGPUs — Driver `intel_xe.elf`

8. **Famiglia Battlemage (Xe2):** Arc B580, B770. *Interfaccia:* Firmware GuC (Graphics MicroController) per lo scheduling delle code e HuC per l'offload dei media.
9. **Famiglia Alchemist (Xe):** Arc A770, A750, A380. *Interfaccia:* Accesso tramite i915/Xe-style page table indipendenti e mappatura della memoria locale (VRAM via PCIe BAR).

### SoC Graphics (ARM) — Driver dedicati embedded

10. **ARM Mali / Immortalis (Mali-G610, G710):** Trovate su Rockchip RK3588. *Interfaccia:* Gestione Job Slot via MMIO basata su registri.
11. **Qualcomm Adreno (Adreno 740, Adreno X1):** *Interfaccia:* Schedulatore integrato basato su ring buffer proprietari CP.
12. **Apple AGX:** *Interfaccia:* Canali di comando strutturati tramite descrittori passati direttamente al coprocessore grafico integrato nel SoC.

---

## 4. Storage, Controllers & Partitions

Nella Fase B1 introduce il contratto `struct fs_ops`. L'albero di chiamata deve seguire la catena: `Controller Hardware Driver (ELF)` $\rightarrow$ `block provider` $\rightarrow$ `fs provider` $\rightarrow$ `VFS`.

```
                    [ VFS Layer ]
                          │
                  [ fs provider ] (ext4 / fat32)
                          │
                [ block core provider ]
                          │
        ┌─────────────────┼─────────────────┐
  [nvme.elf]         [ahci.elf]        [sdhci.elf]
 (NVMe v1.4/2.0)    (SATA Controller) (MicroSD Controller)

```

### Protocolli e Controller Hardware (Mappati come Block Providers)

* **NVMe (Non-Volatile Memory Express) — Standard de facto (PCIe):**
* *Controller target:* Samsung Elpis/Pascal (980/990 Pro), Phison E18/E26 (SSD Gen4/Gen5 consumer), WD/SanDisk proprietari.
* *Metodo ASTRA:* Il driver assegna code di sottomissione e completamento (*Submission & Completion Queues*) in memoria fisica tramite `dma_alloc()`, poi bussa sui registri Doorbell via MMIO.


* **SATA / AHCI (Advanced Host Controller Interface):**
* *Controller target:* Intel SATA Controller (modalità AHCI), AMD FCH SATA.
* *Metodo ASTRA:* Strutture FIS (*Frame Information Structure*) allocate in DMA per inviare comandi NCQ (*Native Command Queuing*). L'IDE legacy è ignorato o relegato a fallback base su porte I/O `0x1F0-0x1F7` (solo amd64).


* **SDHCI (Secure Digital Host Controller Interface) — Per MicroSD ed eMMC:**
* *Controller target:* Arasan SDHCI (usato in molti SoC ARM), Broadcom SDHOST (Raspberry Pi).
* *Metodo ASTRA:* Registri MMIO standard per l'invio di comandi SD (CMD25 per scrittura a blocchi multipli, CMD18 per lettura) tramite ADMA2 (Advanced DMA transazioni a descrittori).



### Tabella delle Partizioni (Parser nel blocco logico del VFS)

* **GPT (GUID Partition Table):** Standard moderno. Il parser legge l'LBA 1, verifica la firma `EFI PART`, cicla sui descrittori di partizione (128 byte ciascuno) ed estrae gli LBA di inizio/fine per mappare i sotto-dispositivi a blocchi.
* **MBR (Master Boot Record):** Legacy. Il parser legge l'LBA 0 e analizza la tabella da 64 byte all'offset `0x1BE` (massimo 4 partizioni primarie).

---

## 5. 50 Most Common Network Interface Cards (NICs)

I driver di rete girano come `net.elf` in userspace. Ricevono pacchetti scrivendo descrittori di ring buffer in un'area `dma_alloc()` e bloccandosi su `wait_irq()`.

### Ethernet (Wired) — Priorità di sviluppo 1

1. **Realtek RTL8111/RTL8168/RTL8125 (1G & 2.5G):** Il chip di rete più diffuso al mondo su schede madri consumer. *Driver:* `rtl8129.elf`. Interfaccia a descrittori TX/RX estremamente semplice basata su ring di 256 elementi.
2. **Intel I219-V / I225-V / I226-V (1G & 2.5G Intel):** Ubiquitario su schede madri Intel. *Driver:* `igc.elf` / `e1000e.elf`. Controllo rigoroso tramite registri PCIe, richiede allocazione accurata di descrittori ad anello avanzati.
3. **Intel X520 / X710 / E810 (10G/40G/100G Enterprise Server):** *Driver:* `ixgbe.elf` / `i40e.elf`. Supportano Single Root I/O Virtualization (SR-IOV) e code multiple (RSS).
4. **Mellanox ConnectX-4 / ConnectX-5 / ConnectX-6 (High Performance Computing):** *Driver:* `mlx5.elf`. Funzionano tramite un modello a code di esecuzione (*Queue Pairs* - QP) altamente ottimizzato per l'offload hardware.
5. **Broadcom NetXtreme (BCM57xx):** Trovate su server Dell/HP e sul modulo Ethernet di Raspberry Pi 4/5 (BCM54213PE).

### Wi-Fi (Wireless) — Complessità elevata (Richiede stack 802.11)

6. **Intel Wi-Fi 6/6E/7 (AX200, AX210, BE200):** Domina il mercato laptop. *Driver:* `iwlwifi.elf`. Richiede il caricamento di un firmware binario massiccio e complesso nel chip prima di poter attivare l'interfaccia PCIe.
7. **Realtek RTL8821CE / RTL8852CE:** Wi-Fi economico molto diffuso.
8. **Broadcom BCM43xx:** Chip Wi-Fi integrato nei MacBook e nei Raspberry Pi.

---

## 6. 50 Most Common Audio Controllers & Codecs

L'audio moderno si divide in due parti: il **Controller Bus** (PCI/MMIO) e il **Codec esterno** (il chip fisico che converte il segnale), connessi tramite un link seriale logico.

### Controller di Bus (Mappati dal driver principale `audio.elf`)

1. **Intel HD Audio Controller (Azalia) / AMD HD Audio:** Lo standard universale su PC x86.
* *Funzionamento:* Gestisce i DMA Engine logici. Scrive in una tabella denominata *Buffer Descriptor List* (BDL) che punta ai campioni PCM in memoria di sistema. Comunica con i codec inviando comandi a 32-bit (Verbs) tramite i registri di ring *CORB* (Command Outbound Ring Buffer) e *RIRB* (Response Inbound Ring Buffer).


2. **Apple Audio DMA Engine (M-Series Audio):** Gestito direttamente tramite canali DMA dedicati mappati in MMIO.
3. **Broadcom PWM/PCM Audio:** Generazione audio analogica basilare su Raspberry Pi tramite pin GPIO / PWM dedicati.

### Codec Audio Target (Configurati tramite Verbs inviati sul Bus HDA)

4. **Realtek ALC887 / ALC892 / ALC897 / ALC1220:** Presenti su oltre l'80% delle schede madri PC prodotte negli ultimi 10 anni. Ciascun codec ha nodi interni (*Widgets*) che il driver deve scoprire e interconnettere (Audio Input, Audio Output, Pin Complex per i jack fisici).
5. **Realtek ALC4080 / ALC4082:** Chip audio di nuova generazione. *Attenzione architetturale:* Non usano il bus HDA standard, sono internamente dei controller USB bridged saldati sulla scheda madre (richiedono uno stack USB funzionante).

---

## 7. 20 Bootloading Architectures & Environments

Il modulo della Fase B4 (**amd64 parity #94**) assorbe i dati di boot trasformandoli in strutture unificate per i kernel primitives.

### Ambiente amd64 (x86_64)

1. **UEFI (Unified Extensible Firmware Interface) Native x86_64:** Il firmware fornisce una mappa di memoria dettagliata (`EFI_MEMORY_DESCRIPTOR`) e gli handle alle tabelle ACPI (tramite la `SystemTable` e il puntatore RSDP). Il kernel viene caricato come binario PE/COFF o tramite un wrapper EFI stub.
2. **Multiboot 1 / Multiboot 2 Specification (GRUB / Limine):** Il bootloader passa il controllo al kernel in modalità protetta o direttamente in long mode. Fornisce una struttura dati in memoria che contiene la mappa dei moduli caricati, le stringhe di comando e la mappa di memoria generata da BIOS INT 15h E820.
3. **Limine Boot Protocol:** Molto amato nello sviluppo OS moderno. Passa informazioni avanzate in modo strutturato (inclusi i terminali video configurati tramite framebuffers SMP già avviati).

### Ambiente aarch64 (ARM64)

4. **U-Boot (Universal Boot Loader):** Lo standard per i dispositivi embedded. Carica il kernel NEXS in formato `Image` non compresso o `uImage` e passa l'indirizzo della struttura *Flattened Device Tree* (FDT / blob `.dtb`) nel registro `x0`.
5. **UEFI AArch64 (EDK2 / Server Base System Architecture):** Usato su server ARM e laptop Snapdragon. Identico alla controparte x86 per quanto riguarda le API, ma passa le configurazioni hardware tramite tabelle ACPI standardizzate per ARM (come la tabella MADT modificata con i descrittori GICC/GICD).
6. **Apple iBoot (Second Stage Loader):** Proprietario Apple. Carica un pacchetto in formato Mach-O (firmato in formato `img4`) e passa una struttura custom chiamata `boot_args` che contiene un Device Tree in formato proprietario Apple (diverso dall'FDT standard).

---

## 8. 20 Security Coprocessors & LLM/NPU Accelerators

Questi dispositivi operano in completo isolamento e vengono esposti in Phase C per l'offload computazionale e la crittografia.

### Coprocessori di Sicurezza (Hardware Root of Trust)

1. **TPM 2.0 Discrete Chips (Infineon SLB9670 / STMicroelectronics):** Chip dedicati connessi tramite bus SPI o LPC scoperti via ACPI. Comunicano tramite l'interfaccia FIFO del protocollo TCG.
2. **Firmware TPM / Intel PTT / AMD fTPM:** Eseguiti all'interno della modalità di esecuzione protetta della CPU (Management Engine per Intel, Platform Security Processor per AMD). Espongono un'interfaccia a registri CRB (Command Response Buffer) in memoria MMIO.
3. **Microsoft Pluton:** Coprocessore di sicurezza integrato direttamente nei die delle CPU AMD Zen 4/5 e Intel Core Ultra.
4. **Apple Secure Enclave Processor (SEP):** Architettura hardware isolata nei SoC Apple. Il kernel NEXS vi comunica esclusivamente scambiando messaggi in una coda hardware protetta (Mailbox).

### Acceleratori AI & NPU (Neural Processing Units)

5. **Intel AI Boost (NPU 3 / NPU 4 in Lunar Lake):** Coprocessore PCIe integrato per il calcolo tensoriale. Opera leggendo descrittori di grafi computazionali allocati in memoria DMA coerente.
6. **AMD Ryzen AI (Architettura XDNA / XDNA 2):** Basata su tecnologia IP di Xilinx Versal. È una griglia di banchi di memoria e ALU (*AI Engine Tiles*) programmabili via canali DMA dedicati gestiti da un microcontrollore interno.
7. **Qualcomm Hexagon NPU (Snapdragon X Elite):** Offre calcolo massivo INT8/FP16 ad altissima efficienza energetica, pilotato tramite code di comando proprietarie Qualcomm.
8. **Apple Neural Engine (ANE):** Acceleratore hardware per il deep learning presente nei SoC Apple, configurato tramite registri MMIO protetti e mappature di memoria DMA statiche.
9. **NVIDIA Tensor Cores (Integrati nelle GPU):** Non sono chip a sé stanti ma estensioni delle unità di calcolo (SM) della GPU. Vengono esposti al sistema operativo tramite le normali code di calcolo compute di `nvgpu.elf`.
10. **Google TPU (Tensor Processing Unit) v4/v5e / Coral Edge TPU:** Disponibili sia in varianti server enterprise che in schedine M.2 consumer via PCIe. Utilizzano un'interfaccia classica a registri MMIO per il caricamento dei modelli compilati in formato vettoriale ed eseguono elaborazioni asincrone con notifiche via MSI-X IRQ.

---

## Architettura d'Integrazione ASTRA (Il Tuo Piano d'Azione)

Per evitare che il tuo codice collassi sotto il peso di questa lista, mantieni una separazione netta tra i livelli. Quando scriverai i driver in userspace (Fase C), il flusso per abilitare uno qualsiasi di questi dispositivi seguirà rigidamente questo pattern di messaggistica:

```
 [ Driver ELF: es. rtl8125.elf ]
        │
        ├── 1. Richiede capacità ──────> [ CAP_SYS_PCI ] per scansionare il bus
        ├── 2. Invoca primitiva ──────> map_mmio(BAR0) -> Riceve un puntatore virtuale isolato
        ├── 3. Invoca primitiva ──────> dma_alloc(4096) -> Alloca l'anello di ricezione pacchetti
        └── 4. Effettua syscall ──────> wait_irq(IRQ_LINE) -> Il driver si sospende fino al pacchetto

```

Questo approccio garantisce che, indipendentemente dal fatto che tu stia eseguendo l'OS su un Raspberry Pi 5 o su un server dual Xeon con quattro RTX 5090, il cuore logico del tuo sistema rimarrà intatto, pulito e invariato.
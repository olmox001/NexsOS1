#include <hal/config.h>
#include "efi_exit_adapter.h"
#include "uefi_min.h"

#define NEXS_KERNEL_LOAD_ADDRESS 0x40080000ULL
#define NEXS_KERNEL_TEXT_OFFSET  0x00080000ULL
#define NEXS_KERNEL_MAX_SIZE     (128ULL * 1024ULL * 1024ULL)
#define NEXS_DTB_MAX_SIZE        (16ULL * 1024ULL * 1024ULL)
#define NEXS_PHYSICAL_MAP_LIMIT  0x0000002000000000ULL /* exclusive: 128 GiB */
#define NEXS_FDT_MAGIC           0xD00DFEEDU
#define NEXS_ARM64_IMAGE_MAGIC   0x644D5241U
#define NEXS_EFI_PREFER_FIRMWARE_DTB 0U

extern const UINT8 nexs_embedded_kernel_start[];
extern const UINT8 nexs_embedded_kernel_end[];
extern const UINT8 nexs_embedded_dtb_start[];
extern const UINT8 nexs_embedded_dtb_end[];
extern void nexs_efi_handoff(UINT64 KernelPA, UINT64 DtbPA,
                             UINT64 KernelSpan, UINT64 DtbSpan,
                             UINT64 FirmwareMapPA, UINT64 FirmwareRecordCount,
                             UINT64 EbsEvidence)
    __attribute__((noreturn));
extern void nexs_efi_post_exit_halt(void) __attribute__((noreturn));

static EFI_SYSTEM_TABLE *gSystemTable;
static EFI_BOOT_SERVICES *gBootServices;

static CHAR16 msg_start[] = {'N','E','X','S','O','S','_','E','F','I','_','S','T','U','B','_','S','T','A','R','T','\r','\n',0};
static CHAR16 msg_kernel[] = {'N','E','X','S','O','S','_','K','E','R','N','E','L','_','I','M','A','G','E','_','L','O','A','D','E','D','\r','\n',0};
static CHAR16 msg_dtb_embedded[] = {'N','E','X','S','O','S','_','D','T','B','_','E','M','B','E','D','D','E','D','\r','\n',0};
static CHAR16 msg_dtb_firmware[] = {'N','E','X','S','O','S','_','D','T','B','_','F','I','R','M','W','A','R','E','_','T','A','B','L','E','\r','\n',0};
static CHAR16 msg_exit[] = {'N','E','X','S','O','S','_','E','X','I','T','_','B','O','O','T','_','S','E','R','V','I','C','E','S','\r','\n',0};
static CHAR16 msg_error[] = {'N','E','X','S','O','S','_','E','F','I','_','S','T','U','B','_','E','R','R','O','R','\r','\n',0};

static void console(const CHAR16 *Text)
{
    if (gSystemTable && gSystemTable->ConOut && gSystemTable->ConOut->OutputString)
        gSystemTable->ConOut->OutputString(gSystemTable->ConOut, (CHAR16 *)Text);
}

static void copy_bytes(UINT8 *Destination, const UINT8 *Source, UINTN Size)
{
    UINTN i;
    for (i = 0; i < Size; ++i) Destination[i] = Source[i];
}

static void zero_bytes(volatile UINT8 *Destination, UINTN Size)
{
    UINTN i;
    for (i = 0; i < Size; ++i) Destination[i] = 0;
}

static UINT32 read_le32(const UINT8 *P)
{
    return (UINT32)P[0] | ((UINT32)P[1] << 8) |
           ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

static UINT64 read_le64(const UINT8 *P)
{
    return (UINT64)read_le32(P) | ((UINT64)read_le32(P + 4) << 32);
}

static UINT32 read_be32(const UINT8 *P)
{
    return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) |
           ((UINT32)P[2] << 8) | (UINT32)P[3];
}

static int guid_equal(const EFI_GUID *A, const EFI_GUID *B)
{
    const UINT8 *a = (const UINT8 *)A;
    const UINT8 *b = (const UINT8 *)B;
    UINTN i;
    for (i = 0; i < 16; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

static EFI_STATUS validate_fdt(UINT64 Address, UINT64 Available, UINT64 *FdtSize)
{
    const UINT8 *Fdt = (const UINT8 *)(UINTN)Address;
    UINT32 Total;
    if (!Fdt || Available < 8 || read_be32(Fdt) != NEXS_FDT_MAGIC)
        return EFI_LOAD_ERROR;
    Total = read_be32(Fdt + 4);
    if (Total < 40 || Total > NEXS_DTB_MAX_SIZE || Total > Available ||
        Address >= NEXS_PHYSICAL_MAP_LIMIT ||
        (UINT64)Total > NEXS_PHYSICAL_MAP_LIMIT - Address)
        return EFI_LOAD_ERROR;
    *FdtSize = Total;
    return EFI_SUCCESS;
}

static EFI_STATUS load_kernel(UINT64 *KernelPA, UINT64 *KernelSpan)
{
    const UINT8 *Image = nexs_embedded_kernel_start;
    UINT64 FileSize = (UINT64)(nexs_embedded_kernel_end -
                               nexs_embedded_kernel_start);
    UINT64 TextOffset, ImageSize, Flags, Address;
    UINT32 Magic;
    UINTN Pages;
    EFI_STATUS Status;

    if (FileSize < 64 || FileSize > NEXS_KERNEL_MAX_SIZE)
        return EFI_LOAD_ERROR;

    TextOffset = read_le64(Image + 8);
    ImageSize = read_le64(Image + 16);
    Flags = read_le64(Image + 24);
    Magic = read_le32(Image + 56);
    if (Magic != NEXS_ARM64_IMAGE_MAGIC ||
        TextOffset != NEXS_KERNEL_TEXT_OFFSET || Flags != 2 ||
        ImageSize < FileSize || ImageSize > NEXS_KERNEL_MAX_SIZE)
        return EFI_LOAD_ERROR;

    Address = NEXS_KERNEL_LOAD_ADDRESS;
    Pages = (UINTN)((ImageSize + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);
    Status = gBootServices->AllocatePages(EFI_ALLOCATE_ADDRESS,
                                          EFI_LOADER_DATA, Pages, &Address);
    if (EFI_ERROR(Status) || Address != NEXS_KERNEL_LOAD_ADDRESS)
        return EFI_OUT_OF_RESOURCES;

    copy_bytes((UINT8 *)(UINTN)Address, Image, (UINTN)FileSize);
    zero_bytes((volatile UINT8 *)(UINTN)(Address + FileSize),
               (UINTN)(ImageSize - FileSize));

    *KernelPA = Address;
    *KernelSpan = ImageSize;
    return EFI_SUCCESS;
}

static EFI_STATUS find_firmware_dtb(EFI_SYSTEM_TABLE *SystemTable,
                                    UINT64 *DtbPA, UINT64 *DtbSpan)
{
    UINTN i;
    for (i = 0; i < SystemTable->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *Entry = &SystemTable->ConfigurationTable[i];
        if (guid_equal(&Entry->VendorGuid, &EFI_DTB_TABLE_GUID)) {
            UINT64 Address = (UINT64)(UINTN)Entry->VendorTable;
            EFI_STATUS Status = validate_fdt(Address, NEXS_DTB_MAX_SIZE,
                                             DtbSpan);
            if (EFI_ERROR(Status)) return Status;
            *DtbPA = Address;
            return EFI_SUCCESS;
        }
    }
    return EFI_LOAD_ERROR;
}

static EFI_STATUS load_embedded_dtb(UINT64 *DtbPA, UINT64 *DtbSpan)
{
    const UINT8 *Image = nexs_embedded_dtb_start;
    UINT64 FileSize = (UINT64)(nexs_embedded_dtb_end -
                               nexs_embedded_dtb_start);
    UINT64 Address = NEXS_PHYSICAL_MAP_LIMIT - 1;
    UINTN Pages;
    EFI_STATUS Status;

    if (FileSize < 40 || FileSize > NEXS_DTB_MAX_SIZE)
        return EFI_LOAD_ERROR;

    Pages = (UINTN)((FileSize + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);
    Status = gBootServices->AllocatePages(EFI_ALLOCATE_MAX_ADDRESS,
                                          EFI_LOADER_DATA, Pages, &Address);
    if (EFI_ERROR(Status)) return Status;

    copy_bytes((UINT8 *)(UINTN)Address, Image, (UINTN)FileSize);
    Status = validate_fdt(Address, FileSize, DtbSpan);
    if (EFI_ERROR(Status)) return Status;

    *DtbPA = Address;
    return EFI_SUCCESS;
}

static EFI_STATUS load_dtb(EFI_SYSTEM_TABLE *SystemTable,
                           UINT64 *DtbPA, UINT64 *DtbSpan)
{
    EFI_STATUS Status;
#if NEXS_EFI_PREFER_FIRMWARE_DTB
    Status = find_firmware_dtb(SystemTable, DtbPA, DtbSpan);
    if (!EFI_ERROR(Status)) {
        console(msg_dtb_firmware);
        return EFI_SUCCESS;
    }
    Status = load_embedded_dtb(DtbPA, DtbSpan);
    if (!EFI_ERROR(Status)) console(msg_dtb_embedded);
#else
    Status = load_embedded_dtb(DtbPA, DtbSpan);
    if (!EFI_ERROR(Status)) {
        console(msg_dtb_embedded);
        return EFI_SUCCESS;
    }
    Status = find_firmware_dtb(SystemTable, DtbPA, DtbSpan);
    if (!EFI_ERROR(Status)) console(msg_dtb_firmware);
#endif
    return Status;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    struct nexs_efi_memory_handoff MemoryHandoff;
    enum nexs_efi_exit_result ExitResult;
    EFI_STATUS Status;
    UINT64 KernelPA = 0, KernelSpan = 0;
    UINT64 DtbPA = 0, DtbSpan = 0;
    UINT64 EbsEvidence;

    gSystemTable = SystemTable;
    gBootServices = SystemTable ? SystemTable->BootServices : 0;
    if (!gBootServices) return EFI_LOAD_ERROR;

    console(msg_start);
    if (gBootServices->SetWatchdogTimer)
        gBootServices->SetWatchdogTimer(0, 0, 0, 0);

    Status = load_kernel(&KernelPA, &KernelSpan);
    if (EFI_ERROR(Status)) {
        console(msg_error);
        return Status;
    }
    console(msg_kernel);

    Status = load_dtb(SystemTable, &DtbPA, &DtbSpan);
    if (EFI_ERROR(Status)) {
        console(msg_error);
        return Status;
    }

    console(msg_exit);
    ExitResult = nexs_efi_exit_and_normalize(
        ImageHandle, gBootServices, &MemoryHandoff);
    if (ExitResult == NEXS_EFI_EXIT_PRE_EXIT_FAILURE) {
        console(msg_error);
        return EFI_ABORTED;
    }
    if (ExitResult != NEXS_EFI_EXIT_OK)
        nexs_efi_post_exit_halt();

    EbsEvidence = (UINT64)MemoryHandoff.exit_attempts |
                  ((UINT64)MemoryHandoff.buffer_growths << 32);
    nexs_efi_handoff(KernelPA, DtbPA, KernelSpan, DtbSpan,
                     MemoryHandoff.record_base,
                     (UINT64)MemoryHandoff.record_count, EbsEvidence);
}

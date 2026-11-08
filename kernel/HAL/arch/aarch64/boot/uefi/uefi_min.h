#ifndef NEXSOS_UEFI_MIN_H
#define NEXSOS_UEFI_MIN_H

/* Minimal UEFI 2.x ABI declarations for the standalone AArch64 boot adapter.
 * The PE/COFF image is built with the AArch64 Windows target; its integer and
 * pointer calling convention matches the AArch64 UEFI AAPCS64 ABI. */
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef UINT64 UINTN;
typedef UINT16 CHAR16;
typedef UINT64 EFI_STATUS;
typedef void *EFI_HANDLE;

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (*EFI_TEXT_OUTPUT_STRING)(
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_OUTPUT_STRING OutputString;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

struct EFI_BOOT_SERVICES;
typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (*AllocatePages)(UINT32 Type, UINT32 MemoryType, UINTN Pages,
                                UINT64 *Memory);
    void *FreePages;
    EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize, void *MemoryMap,
                               UINTN *MapKey, UINTN *DescriptorSize,
                               UINT32 *DescriptorVersion);
    EFI_STATUS (*AllocatePool)(UINT32 PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (*FreePool)(void *Buffer);
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void *GetNextMonotonicCount;
    void *Stall;
    EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode,
                                   UINTN DataSize, CHAR16 *WatchdogData);
} EFI_BOOT_SERVICES;

#define EFI_SUCCESS              0x0000000000000000ULL
#define EFI_LOAD_ERROR           0x8000000000000001ULL
#define EFI_INVALID_PARAMETER    0x8000000000000002ULL
#define EFI_BUFFER_TOO_SMALL     0x8000000000000005ULL
#define EFI_OUT_OF_RESOURCES     0x8000000000000009ULL
#define EFI_ABORTED              0x8000000000000015ULL
#define EFI_ERROR(Status)        (((Status) & 0x8000000000000000ULL) != 0)

#define EFI_ALLOCATE_ANY_PAGES   0U
#define EFI_ALLOCATE_MAX_ADDRESS 1U
#define EFI_ALLOCATE_ADDRESS     2U
#define EFI_LOADER_DATA          2U
#define EFI_PAGE_SIZE            4096ULL

static const EFI_GUID EFI_DTB_TABLE_GUID =
    {0xB1B621D5,0xF19C,0x41A5,{0x83,0x0B,0xD9,0x15,0x2C,0x69,0xAA,0xE0}};

#endif

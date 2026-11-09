#ifndef NEXSOS_EFI_EXIT_ADAPTER_H
#define NEXSOS_EFI_EXIT_ADAPTER_H

#include "uefi_min.h"

#include <hal/boot_info.h>
#include <stdint.h>

enum nexs_efi_exit_result {
    NEXS_EFI_EXIT_OK = 0,
    NEXS_EFI_EXIT_PRE_EXIT_FAILURE = 1,
    NEXS_EFI_EXIT_POST_EXIT_FAILURE = 2
};

struct nexs_efi_memory_handoff {
    UINT64 record_base;
    UINT32 record_count;
    UINT32 record_size;
    UINT32 exit_attempts;
    UINT32 buffer_growths;
};

enum nexs_efi_exit_result nexs_efi_exit_and_normalize(
    EFI_HANDLE image_handle,
    EFI_BOOT_SERVICES *boot_services,
    struct nexs_efi_memory_handoff *handoff);

#endif

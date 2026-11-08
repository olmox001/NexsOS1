#include <hal/device_name.h>
#include <stdint.h>
#include <string.h>

int main(void) {
  char buf[32];
  char tiny[5] = {'x', 'x', 'x', 'x', 'x'};

  nxhal_device_name_virtio(buf, sizeof(buf), 1U);
  if (strcmp(buf, "VirtIO-1") != 0)
    return 1;

  nxhal_device_name_virtio(buf, sizeof(buf), UINT32_MAX);
  if (strcmp(buf, "VirtIO-4294967295") != 0)
    return 2;

  nxhal_device_name_pci(buf, sizeof(buf), UINT16_C(0x1AF4), UINT16_C(0x1042));
  if (strcmp(buf, "PCI-1af4:1042") != 0)
    return 3;

  nxhal_device_name_pci(buf, sizeof(buf), UINT16_C(0), UINT16_C(0));
  if (strcmp(buf, "PCI-0000:0000") != 0)
    return 4;

  nxhal_device_name_virtio(tiny, sizeof(tiny), 1234U);
  if (tiny[sizeof(tiny) - 1U] != '\0')
    return 5;
  if (strcmp(tiny, "Virt") != 0)
    return 6;

  nxhal_device_name_pci(NULL, 0U, 0U, 0U);
  nxhal_device_name_virtio(NULL, 0U, 0U);

  return 0;
}

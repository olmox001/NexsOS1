#include <hal/device_protocol.h>

#include <stdint.h>
#include <stdlib.h>

static unsigned publish_calls;
static struct nxhal_device_desc published_device;

void hal_irq_disable_all(void) {
  abort();
}

void hal_cpu_halt(void) {
  abort();
}

static void test_publish(const struct nxhal_device_desc *device) {
  if (device != NULL) {
    published_device = *device;
    ++publish_calls;
  }
}

static int test_get_count(void) {
  return 3;
}

static int test_get(int index, struct nxhal_device_desc *device) {
  if (device == NULL || (index != 1 && index != 2))
    return -1;
  device->vendor_id = UINT16_C(0x1af4);
  device->device_id = (uint16_t)index;
  device->pci_bdf = (uint32_t)index;
  return 0;
}

static struct nxhal_device_protocol_v1 valid_protocol(void) {
  struct nxhal_device_protocol_v1 protocol = {
      .abi_version = NXHAL_DEVICE_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_device_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .publish = test_publish,
      .get_count = test_get_count,
      .get = test_get,
  };
  return protocol;
}

int main(void) {
  struct nxhal_device_protocol_v1 protocol = valid_protocol();
  struct nxhal_device_desc device = {0};

  if (nxhal_device_bind(NULL) != NXHAL_DEVICE_BIND_E_NULL)
    return 1;
  protocol.abi_version = 0U;
  if (nxhal_device_bind(&protocol) != NXHAL_DEVICE_BIND_E_VERSION)
    return 2;
  protocol = valid_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_device_bind(&protocol) != NXHAL_DEVICE_BIND_E_SIZE)
    return 3;
  protocol = valid_protocol();
  protocol.flags = 1U;
  if (nxhal_device_bind(&protocol) != NXHAL_DEVICE_BIND_E_RESERVED)
    return 4;
  protocol = valid_protocol();
  protocol.publish = NULL;
  if (nxhal_device_bind(&protocol) != NXHAL_DEVICE_BIND_E_MISSING)
    return 5;

  protocol = valid_protocol();
  if (nxhal_device_bind(&protocol) != NXHAL_DEVICE_BIND_OK)
    return 6;
  if (!nxhal_device_is_bound())
    return 7;
  if (nxhal_device_bind(&protocol) != NXHAL_DEVICE_BIND_E_STATE)
    return 8;

  device.vendor_id = UINT16_C(0x1234);
  nxhal_device_publish(&device);
  device.vendor_id = UINT16_C(0x5678);
  nxhal_device_publish(&device);
  if (publish_calls != 2U ||
      published_device.vendor_id != UINT16_C(0x5678))
    return 9;

  if (nxhal_device_get_count() != 3 || nxhal_device_get_count() != 3)
    return 10;
  device = (struct nxhal_device_desc){0};
  if (nxhal_device_get(1, &device) != 0 ||
      device.device_id != UINT16_C(1))
    return 11;
  if (nxhal_device_get(2, &device) != 0 ||
      device.device_id != UINT16_C(2))
    return 12;

  return 0;
}

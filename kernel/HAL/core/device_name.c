#include <hal/device_name.h>
#include <stddef.h>
#include <stdint.h>

struct nxhal_name_writer {
  char *dst;
  size_t capacity;
  size_t used;
};

static void name_putc(struct nxhal_name_writer *writer, char value) {
  if (writer->capacity == 0U)
    return;
  if (writer->used + 1U < writer->capacity) {
    writer->dst[writer->used] = value;
    ++writer->used;
  }
  writer->dst[writer->used] = '\0';
}

static void name_literal(struct nxhal_name_writer *writer, const char *text) {
  while (*text != '\0') {
    name_putc(writer, *text);
    ++text;
  }
}

static void name_u32_decimal(struct nxhal_name_writer *writer, uint32_t value) {
  char digits[10];
  size_t count = 0U;

  do {
    digits[count] = (char)('0' + (value % 10U));
    ++count;
    value /= 10U;
  } while (value != 0U && count < sizeof(digits));

  while (count != 0U) {
    --count;
    name_putc(writer, digits[count]);
  }
}

static void name_u16_hex4(struct nxhal_name_writer *writer, uint16_t value) {
  static const char hex[] = "0123456789abcdef";
  for (unsigned shift = 12U;; shift -= 4U) {
    uint32_t nibble = ((uint32_t)value >> shift) & UINT32_C(0xF);
    name_putc(writer, hex[nibble]);
    if (shift == 0U)
      break;
  }
}

static struct nxhal_name_writer name_begin(char *dst, size_t capacity) {
  struct nxhal_name_writer writer = {
      .dst = dst,
      .capacity = capacity,
      .used = 0U,
  };
  if (dst != NULL && capacity != 0U)
    dst[0] = '\0';
  return writer;
}

void nxhal_device_name_virtio(char *dst, size_t capacity, uint32_t device_id) {
  struct nxhal_name_writer writer = name_begin(dst, capacity);
  if (dst == NULL || capacity == 0U)
    return;
  name_literal(&writer, "VirtIO-");
  name_u32_decimal(&writer, device_id);
}

void nxhal_device_name_pci(char *dst, size_t capacity,
                           uint16_t vendor_id, uint16_t device_id) {
  struct nxhal_name_writer writer = name_begin(dst, capacity);
  if (dst == NULL || capacity == 0U)
    return;
  name_literal(&writer, "PCI-");
  name_u16_hex4(&writer, vendor_id);
  name_putc(&writer, ':');
  name_u16_hex4(&writer, device_id);
}

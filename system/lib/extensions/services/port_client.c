/* Shared capability-message envelope for supervised services. */
#include <os1.h>

int OS1_port_packet_receive(int handle, struct os1_port_packet *packet,
                             int nonblocking) {
  if (packet == NULL) return -EINVAL;
  *packet = (struct os1_port_packet){0};
  long received = nonblocking ?
      OS1_object_ctl(handle, OBJ_CTL_PORT_TRY_RECV_PACKET, (long)packet) :
      OS1_object_read(handle, packet, sizeof(*packet));
  if (received < 0) return (int)received;
  if (received != (long)sizeof(*packet) ||
      packet->abi_version != OS1_PORT_PACKET_VERSION ||
      packet->struct_size != sizeof(*packet) ||
      packet->cap_count > OS1_PORT_CAPS_MAX || packet->reserved != 0U)
    return -EINVAL;
  return 0;
}

int OS1_port_packet_close_caps(struct os1_port_packet *packet) {
  if (packet == NULL || packet->cap_count > OS1_PORT_CAPS_MAX) return -EINVAL;
  int status = 0;
  for (unsigned i = 0U; i < OS1_PORT_CAPS_MAX && i < packet->cap_count; ++i) {
    int handle = -1;
    unsigned char *bytes = (unsigned char *)&handle;
    for (unsigned j = 0U; j < sizeof(handle); ++j)
      bytes[j] = (unsigned char)packet->message.payload[i * sizeof(handle) + j];
    long closed = OS1low_handle_close(handle);
    if (closed != 0 && status == 0) status = closed < 0 ? (int)closed : -EIO;
  }
  packet->cap_count = 0U;
  return status;
}

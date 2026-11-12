#include <hal/resource.h>

#include <stddef.h>

_Static_assert(sizeof(struct nxhal_object_ref) == 24U,
               "resource object-ref ABI size changed");
_Static_assert(_Alignof(struct nxhal_object_ref) == 8U,
               "resource object-ref ABI alignment changed");
_Static_assert(sizeof(struct nxhal_resource) == 120U,
               "resource ABI size changed");
_Static_assert(_Alignof(struct nxhal_resource) == 8U,
               "resource ABI alignment changed");
_Static_assert(offsetof(struct nxhal_resource, object) == 16U,
               "resource object offset changed");
_Static_assert(offsetof(struct nxhal_resource, parent) == 40U,
               "resource parent offset changed");
_Static_assert(offsetof(struct nxhal_resource, resource_offset) == 64U,
               "resource range offset changed");
_Static_assert(offsetof(struct nxhal_resource, policy_version) == 12U,
               "resource policy offset changed");
_Static_assert(offsetof(struct nxhal_resource, max_transfer_bytes) == 112U,
               "resource transfer offset changed");
_Static_assert(sizeof(struct nxhal_resource_grant) == 96U,
               "resource grant ABI size changed");
_Static_assert(_Alignof(struct nxhal_resource_grant) == 8U,
               "resource grant ABI alignment changed");
_Static_assert(offsetof(struct nxhal_resource_grant, resource) == 8U,
               "grant resource offset changed");
_Static_assert(offsetof(struct nxhal_resource_grant, principal) == 32U,
               "grant principal offset changed");
_Static_assert(offsetof(struct nxhal_resource_grant, rights) == 56U,
               "grant rights offset changed");
_Static_assert(offsetof(struct nxhal_resource_grant, max_transfer_bytes) == 88U,
               "grant transfer offset changed");

int main(void) {
  return 0;
}

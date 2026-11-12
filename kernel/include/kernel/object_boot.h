/*
 * Object registry bootstrap ABI for NX-CORE.
 *
 * Main is a composition root: storage providers and the object registry own
 * filesystem import, typed key publication and executable discovery.
 * Boot source syntax is intentionally not part of K1/K2/K3 core control flow.
 */
#ifndef NX_CORE_OBJECT_BOOT_H
#define NX_CORE_OBJECT_BOOT_H

#include <kernel/nx_contract.h>

#define NX_BOOT_REGISTRY_KEY "system.boot.entry"
#define NX_BOOT_EXEC_PATH_MAX 96U

/* Mount concrete storage behind the unified object namespace. */
void nx_object_storage_mount(void);
/* Initialize the authoritative registry and publish its initial boot object.
 * Return 0 only when an actual validated boot source was imported. */
int nx_object_registry_start(void) NX_MUST_USE;
/* Resolve the first user-mode process from the registry object, never VFS. */
int nx_object_bootstrap_launch(void) NX_MUST_USE;

#endif

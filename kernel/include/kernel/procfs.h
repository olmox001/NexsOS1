/*
 * kernel/include/kernel/procfs.h
 * /proc — a synthetic namespace provider exposing live processes as TYPED
 * capability objects (ASTRA §6.2 "everything is an object"): a path /proc/<pid>
 * resolves to an OBJ_TYPE_PROCESS object, read through the object/capability
 * mechanism (sys_object_read), NOT a side-channel.  Mounted by procfs_init().
 */
#ifndef _KERNEL_PROCFS_H
#define _KERNEL_PROCFS_H

/* procfs_init - mount /proc (Plan 9 namespace).  Call from the composition root
 * after vfs_init(). */
void procfs_init(void);

#endif /* _KERNEL_PROCFS_H */

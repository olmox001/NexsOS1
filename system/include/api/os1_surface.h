#ifndef OS1_SURFACE_H
#define OS1_SURFACE_H
#include <surface.h>
#include <os1_compositor.h>
/* Hardware resource client; handles may instead be delegated by a supervisor.
 * Notification carries a generation, not a pulse, so wakeups cannot be lost. */
long OS1_surface_events_open(unsigned rights);
long OS1_surface_state(int handle, struct os1_surface_state_v1 *state);
long OS1_surface_wait(int handle, uint64_t generation);
long OS1_surface_invalidate(void);
long OS1_surface_present(struct os1_compositor_present_v1 *present);
long OS1_surface_snapshot(struct os1_window_snapshot_v1 *snapshot);
long OS1_surface_move(int id, int x, int y);
long OS1_input_resource_read(struct os1_raw_input_event_v1 *event,
                             unsigned role, unsigned mode);
#endif

/* iOS proof of concept, milestone P1 (branch ios-poc): the runtime without its GPU library.
 * bb-probe runs with --cpu-only, so nothing is drawn; these answer the calls the runtime still
 * makes as the GPU library does with no window: no input filters, no faults handled, no
 * symbols, no overlay. Replaced by the real libbbgpu at milestone P2. */
#include "gpu/bbgpu.h"

#include <stdio.h>

void bbgpu_trophy_notification(const char *title, int id) { printf("Trophy %d: %s\n", id, title); }
void bbgpu_register_kernel(void) {}
int bbgpu_init(const BbGpuConfig *config) { (void)config; return -1; }
void bbgpu_window_loop(void) {}
uintptr_t bbgpu_resolve(const char *scoped_nid) { (void)scoped_nid; return 0; }
int bbgpu_handle_fault(void *ucontext, void *address) { (void)ucontext; (void)address; return 0; }
void bbgpu_dump_guest_writes(void *ucontext) { (void)ucontext; }
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8) { (void)initial_utf8; (void)prompt_utf8; return -1; }
int bbgpu_text_input_poll(char *out_utf8, uint64_t size) { (void)out_utf8; (void)size; return -1; }
int bbgpu_overlay_captures_input(void) { return 0; }
void bbgpu_mouse_take(double *dx, double *dy, int *wheel_up, int *wheel_down) { *dx = *dy = 0; *wheel_up = *wheel_down = 0; }
int bbgpu_mouse_captured(void) { return 0; }
void bbgpu_mouse_look_enable(int enabled) { (void)enabled; }
void bbgpu_mouse_capture(void) {}
void bbgpu_add_event_filter(BbGpuEventFilter filter) { (void)filter; }
void bbgpu_add_pointer_query(BbGpuPointerQuery query) { (void)query; }
int bbgpu_pointer_wanted(void) { return 0; }
void bbgpu_add_pad_filter(BbGpuPadFilter filter) { (void)filter; }
void bbgpu_filter_pad(uint32_t *buttons, const uint8_t *sticks) { (void)buttons; (void)sticks; }
float bbgpu_frame_aspect(void) { return 16.0f / 9.0f; }
int bbgpu_camera_look(float yaw, float pitch) { (void)yaw; (void)pitch; return 0; }
int bbgpu_free_camera(void) { return 0; }
void bbgpu_patch_image(unsigned char *image, uint64_t size) { (void)image; (void)size; }
unsigned bbgpu_symbol_count(void) { return 0; }

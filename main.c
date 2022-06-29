// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "loop.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct state {
	struct seat *seat;
	bool run_display;
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct surface *surface;
	struct loop *eventloop;
	struct zwlr_layer_shell_v1 *layer_shell;
};

struct seat {
	struct state *state;
	struct wl_keyboard *keyboard;
	struct {
		struct xkb_state *state;
		struct xkb_context *context;
		struct xkb_keymap *keymap;
	} xkb;
};

struct surface {
	struct state *state;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
};

static struct state state = { 0 };

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	/* nop */
}

static void
surface_destroy(struct surface *surface)
{
	if (surface->layer_surface) {
		zwlr_layer_surface_v1_destroy(surface->layer_surface);
	}
	if (surface->surface) {
		wl_surface_destroy(surface->surface);
	}
	free(surface);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
	struct surface *surface = data;
	surface_destroy(surface);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

void
surface_layer_surface_create(struct surface *surface)
{
	struct state *state = surface->state;

	assert(surface->surface);
	surface->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		state->layer_shell, surface->surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "keyboard-layout");
	assert(surface->layer_surface);

	zwlr_layer_surface_v1_set_size(surface->layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_anchor(surface->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(surface->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
			surface->layer_surface, true);
	zwlr_layer_surface_v1_add_listener(surface->layer_surface,
			&layer_surface_listener, surface);
	wl_surface_commit(surface->surface);
}

static void
handle_wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size)
{
	struct seat *seat = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		fprintf(stderr, "die: unknown keymap format %d", format);
		exit(EXIT_FAILURE);
	}
	char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map_shm == MAP_FAILED) {
		close(fd);
		fprintf(stderr, "die: unable to initialize keymap shm");
		exit(EXIT_FAILURE);
	}
	struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_string(
		seat->xkb.context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_shm, size);
	close(fd);

	struct xkb_state *xkb_state = xkb_state_new(xkb_keymap);
	xkb_keymap_unref(seat->xkb.keymap);
	xkb_state_unref(seat->xkb.state);
	seat->xkb.keymap = xkb_keymap;
	seat->xkb.state = xkb_state;

	seat->state->run_display = false;
}

static void
handle_wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface,
		struct wl_array *keys)
{
	/* nop */
}

static void
handle_wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface)
{
	/* nop */
}

static void
handle_wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t _state)
{
	/* nop */
}

static void
handle_wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group)
{
	struct seat *seat = data;
	if (!seat->xkb.state) {
		return;
	}
	xkb_state_update_mask(seat->xkb.state, mods_depressed, mods_latched,
		mods_locked, 0, 0, group);
}

static void
handle_wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay)
{
	/* nop */
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = handle_wl_keyboard_keymap,
	.enter = handle_wl_keyboard_enter,
	.leave = handle_wl_keyboard_leave,
	.key = handle_wl_keyboard_key,
	.modifiers = handle_wl_keyboard_modifiers,
	.repeat_info = handle_wl_keyboard_repeat_info,
};

static void
handle_wl_seat_capabilities(void *data, struct wl_seat *wl_seat,
		enum wl_seat_capability caps)
{
	struct seat *seat = data;
	if (seat->keyboard) {
		wl_keyboard_release(seat->keyboard);
		seat->keyboard = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
		seat->keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(seat->keyboard,
			&keyboard_listener, seat);
	}
}

static void
handle_wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
	/* nop */
}

const struct wl_seat_listener seat_listener = {
	.capabilities = handle_wl_seat_capabilities,
	.name = handle_wl_seat_name,
};

void
seat_init(struct state *state, struct wl_seat *wl_seat)
{
	struct seat *seat = calloc(1, sizeof(struct seat));
	seat->state = state;
	state->seat = seat;
	wl_seat_add_listener(wl_seat, &seat_listener, seat);
}

static void
handle_wl_registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	struct state *state = data;
	if (!strcmp(interface, wl_compositor_interface.name)) {
		state->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		state->shm = wl_registry_bind(registry, name,
				&wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		struct wl_seat *wl_seat = wl_registry_bind(registry, name,
				&wl_seat_interface, 7);
		seat_init(state, wl_seat);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		state->layer_shell = wl_registry_bind(
			registry, name, &zwlr_layer_shell_v1_interface, 4);
	}
}

static void
handle_wl_registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name)
{
	/* nop */
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_wl_registry_global,
	.global_remove = handle_wl_registry_global_remove,
};

void
globals_init(struct state *state)
{
	struct wl_registry *registry = wl_display_get_registry(state->display);
	wl_registry_add_listener(registry, &registry_listener, state);
}

static void
display_in(int fd, short mask, void *data)
{
	if (wl_display_dispatch(state.display) == -1) {
		state.run_display = false;
	}
}

static void
print_keyboard_layout(struct seat *seat)
{
	if (!seat || !seat->xkb.keymap) {
		return;
	}
	const char *layout_text = NULL;
	xkb_layout_index_t num_layout = xkb_keymap_num_layouts(seat->xkb.keymap);
	if (num_layout > 1) {
		xkb_layout_index_t curr_layout = 0;

		/* Advance to the first active layout (if any) */
		while (curr_layout < num_layout
				&& xkb_state_layout_index_is_active(seat->xkb.state,
				curr_layout, XKB_STATE_LAYOUT_EFFECTIVE) != 1) {
			++curr_layout;
		}
		/* Handle invalid index if none are active */
		layout_text = xkb_keymap_layout_get_name(seat->xkb.keymap, curr_layout);
		printf("%s\n", layout_text);
	}
}

#define DIE_ON(condition, message) do { \
	if ((condition) != 0) { \
		fprintf(stderr, message); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

int
main(int argc, char *argv[])
{
	state.display = wl_display_connect(NULL);
	DIE_ON(!state.display, "unable to connect to compositor");

	globals_init(&state);
	wl_display_roundtrip(state.display);
	DIE_ON(!state.compositor, "no compositor");
	DIE_ON(!state.shm, "no shm");
	DIE_ON(!state.seat, "no seat");
	DIE_ON(!state.layer_shell, "no layer-shell");

	state.seat->xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	state.surface = calloc(1, sizeof(struct surface));
	state.surface->state = &state;
	state.surface->surface = wl_compositor_create_surface(state.compositor);

	surface_layer_surface_create(state.surface);

	state.eventloop = loop_create();
	loop_add_fd(state.eventloop, wl_display_get_fd(state.display), POLLIN,
		display_in, NULL);

	state.run_display = true;
	while (state.run_display) {
		errno = 0;
		if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
			break;
		}
		loop_poll(state.eventloop);
	}
	print_keyboard_layout(state.seat);

	surface_destroy(state.surface);
	return 0;
}

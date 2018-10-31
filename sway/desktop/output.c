#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <wayland-server.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/util/region.h>
#include "config.h"
#include "log.h"
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/layers.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"

struct sway_output *output_by_name(const char *name) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		if (strcasecmp(output->wlr_output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

struct sway_output *output_by_identifier(const char *identifier) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		char output_identifier[128];
		snprintf(output_identifier, sizeof(output_identifier), "%s %s %s", output->wlr_output->make,
			output->wlr_output->model, output->wlr_output->serial);
		if (strcasecmp(output_identifier, identifier) == 0) {
			return output;
		}
	}
	return NULL;
}

/**
 * Rotate a child's position relative to a parent. The parent size is (pw, ph),
 * the child position is (*sx, *sy) and its size is (sw, sh).
 */
static void rotate_child_position(double *sx, double *sy, double sw, double sh,
		double pw, double ph, float rotation) {
	if (rotation == 0.0f) {
		return;
	}

	// Coordinates relative to the center of the subsurface
	double ox = *sx - pw/2 + sw/2,
		oy = *sy - ph/2 + sh/2;
	// Rotated coordinates
	double rx = cos(-rotation)*ox - sin(-rotation)*oy,
		ry = cos(-rotation)*oy + sin(-rotation)*ox;
	*sx = rx + pw/2 - sw/2;
	*sy = ry + ph/2 - sh/2;
}

struct surface_iterator_data {
	sway_surface_iterator_func_t user_iterator;
	void *user_data;

	struct sway_output *output;
	double ox, oy;
	int width, height;
	float rotation;
};

static bool get_surface_box(struct surface_iterator_data *data,
		struct wlr_surface *surface, int sx, int sy,
		struct wlr_box *surface_box) {
	struct sway_output *output = data->output;

	if (!wlr_surface_has_buffer(surface)) {
		return false;
	}

	int sw = surface->current.width;
	int sh = surface->current.height;

	double _sx = sx + surface->sx;
	double _sy = sy + surface->sy;
	rotate_child_position(&_sx, &_sy, sw, sh, data->width, data->height,
		data->rotation);

	struct wlr_box box = {
		.x = data->ox + _sx,
		.y = data->oy + _sy,
		.width = sw,
		.height = sh,
	};
	if (surface_box != NULL) {
		memcpy(surface_box, &box, sizeof(struct wlr_box));
	}

	struct wlr_box rotated_box;
	wlr_box_rotated_bounds(&box, data->rotation, &rotated_box);

	struct wlr_box output_box = {
		.width = output->width,
		.height = output->height,
	};

	struct wlr_box intersection;
	return wlr_box_intersection(&output_box, &rotated_box, &intersection);
}

static void output_for_each_surface_iterator(struct wlr_surface *surface,
		int sx, int sy, void *_data) {
	struct surface_iterator_data *data = _data;

	struct wlr_box box;
	bool intersects = get_surface_box(data, surface, sx, sy, &box);
	if (!intersects) {
		return;
	}

	data->user_iterator(data->output, surface, &box, data->rotation,
		data->user_data);
}

void output_surface_for_each_surface(struct sway_output *output,
		struct wlr_surface *surface, double ox, double oy,
		sway_surface_iterator_func_t iterator, void *user_data) {
	struct surface_iterator_data data = {
		.user_iterator = iterator,
		.user_data = user_data,
		.output = output,
		.ox = ox,
		.oy = oy,
		.width = surface->current.width,
		.height = surface->current.height,
		.rotation = 0,
	};

	wlr_surface_for_each_surface(surface,
		output_for_each_surface_iterator, &data);
}

void output_view_for_each_surface(struct sway_output *output,
		struct sway_view *view, sway_surface_iterator_func_t iterator,
		void *user_data) {
	struct surface_iterator_data data = {
		.user_iterator = iterator,
		.user_data = user_data,
		.output = output,
		.ox = view->container->current.view_x - output->wlr_output->lx
			- view->geometry.x,
		.oy = view->container->current.view_y - output->wlr_output->ly
			- view->geometry.y,
		.width = view->container->current.view_width,
		.height = view->container->current.view_height,
		.rotation = 0, // TODO
	};

	view_for_each_surface(view, output_for_each_surface_iterator, &data);
}

void output_view_for_each_popup(struct sway_output *output,
		struct sway_view *view, sway_surface_iterator_func_t iterator,
		void *user_data) {
	struct surface_iterator_data data = {
		.user_iterator = iterator,
		.user_data = user_data,
		.output = output,
		.ox = view->container->current.view_x - output->wlr_output->lx
			- view->geometry.x,
		.oy = view->container->current.view_y - output->wlr_output->ly
			- view->geometry.y,
		.width = view->container->current.view_width,
		.height = view->container->current.view_height,
		.rotation = 0, // TODO
	};

	view_for_each_popup(view, output_for_each_surface_iterator, &data);
}

void output_layer_for_each_surface(struct sway_output *output,
		struct wl_list *layer_surfaces, sway_surface_iterator_func_t iterator,
		void *user_data) {
	struct sway_layer_surface *layer_surface;
	wl_list_for_each(layer_surface, layer_surfaces, link) {
		struct wlr_layer_surface_v1 *wlr_layer_surface_v1 =
			layer_surface->layer_surface;
		output_surface_for_each_surface(output, wlr_layer_surface_v1->surface,
			layer_surface->geo.x, layer_surface->geo.y, iterator,
			user_data);
	}
}

#ifdef HAVE_XWAYLAND
void output_unmanaged_for_each_surface(struct sway_output *output,
		struct wl_list *unmanaged, sway_surface_iterator_func_t iterator,
		void *user_data) {
	struct sway_xwayland_unmanaged *unmanaged_surface;
	wl_list_for_each(unmanaged_surface, unmanaged, link) {
		struct wlr_xwayland_surface *xsurface =
			unmanaged_surface->wlr_xwayland_surface;
		double ox = unmanaged_surface->lx - output->wlr_output->lx;
		double oy = unmanaged_surface->ly - output->wlr_output->ly;

		output_surface_for_each_surface(output, xsurface->surface, ox, oy,
			iterator, user_data);
	}
}
#endif

void output_drag_icons_for_each_surface(struct sway_output *output,
		struct wl_list *drag_icons, sway_surface_iterator_func_t iterator,
		void *user_data) {
	struct sway_drag_icon *drag_icon;
	wl_list_for_each(drag_icon, drag_icons, link) {
		double ox = drag_icon->x - output->wlr_output->lx;
		double oy = drag_icon->y - output->wlr_output->ly;

		if (drag_icon->wlr_drag_icon->mapped) {
			output_surface_for_each_surface(output,
				drag_icon->wlr_drag_icon->surface, ox, oy,
				iterator, user_data);
		}
	}
}

static void for_each_surface_container_iterator(struct sway_container *con,
		void *_data) {
	if (!con->view || !view_is_visible(con->view)) {
		return;
	}

	struct surface_iterator_data *data = _data;
	output_view_for_each_surface(data->output, con->view,
		data->user_iterator, data->user_data);
}

static void output_for_each_surface(struct sway_output *output,
		sway_surface_iterator_func_t iterator, void *user_data) {
	if (output_has_opaque_overlay_layer_surface(output)) {
		goto overlay;
	}

	struct surface_iterator_data data = {
		.user_iterator = iterator,
		.user_data = user_data,
		.output = output,
	};

	struct sway_workspace *workspace = output_get_active_workspace(output);
	if (workspace->current.fullscreen) {
		for_each_surface_container_iterator(
			workspace->current.fullscreen, &data);
		container_for_each_child(workspace->current.fullscreen,
			for_each_surface_container_iterator, &data);
		for (int i = 0; i < workspace->current.floating->length; ++i) {
			struct sway_container *floater =
				workspace->current.floating->items[i];
			if (container_is_transient_for(floater,
					workspace->current.fullscreen)) {
				for_each_surface_container_iterator(floater, &data);
			}
		}
#ifdef HAVE_XWAYLAND
		output_unmanaged_for_each_surface(output, &root->xwayland_unmanaged,
			iterator, user_data);
#endif
	} else {
		output_layer_for_each_surface(output,
			&output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
			iterator, user_data);
		output_layer_for_each_surface(output,
			&output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM],
			iterator, user_data);

		workspace_for_each_container(workspace,
			for_each_surface_container_iterator, &data);

#ifdef HAVE_XWAYLAND
		output_unmanaged_for_each_surface(output, &root->xwayland_unmanaged,
			iterator, user_data);
#endif
		output_layer_for_each_surface(output,
			&output->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP],
			iterator, user_data);
	}

overlay:
	output_layer_for_each_surface(output,
		&output->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
		iterator, user_data);
	output_drag_icons_for_each_surface(output, &root->drag_icons,
		iterator, user_data);
}

static int scale_length(int length, int offset, float scale) {
	return round((offset + length) * scale) - round(offset * scale);
}

static void scale_box(struct wlr_box *box, float scale) {
	box->width = scale_length(box->width, box->x, scale);
	box->height = scale_length(box->height, box->y, scale);
	box->x = round(box->x * scale);
	box->y = round(box->y * scale);
}

struct sway_workspace *output_get_active_workspace(struct sway_output *output) {
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_node *focus = seat_get_active_tiling_child(seat, &output->node);
	if (!focus) {
		return output->workspaces->items[0];
	}
	return focus->sway_workspace;
}

bool output_has_opaque_overlay_layer_surface(struct sway_output *output) {
	struct wlr_layer_surface_v1 *wlr_layer_surface_v1;
	wl_list_for_each(wlr_layer_surface_v1, &server.layer_shell->surfaces, link) {
		if (wlr_layer_surface_v1->output != output->wlr_output ||
				wlr_layer_surface_v1->layer != ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
			continue;
		}
		struct wlr_surface *wlr_surface = wlr_layer_surface_v1->surface;
		struct sway_layer_surface *sway_layer_surface =
			layer_from_wlr_layer_surface_v1(wlr_layer_surface_v1);
		pixman_box32_t output_box = {
			.x2 = output->width,
			.y2 = output->height,
		};
		pixman_region32_t surface_opaque_box;
		pixman_region32_init(&surface_opaque_box);
		pixman_region32_copy(&surface_opaque_box, &wlr_surface->opaque_region);
		pixman_region32_translate(&surface_opaque_box,
			sway_layer_surface->geo.x, sway_layer_surface->geo.y);
		bool contains = pixman_region32_contains_rectangle(&surface_opaque_box,
			&output_box);
		pixman_region32_fini(&surface_opaque_box);
		if (contains) {
			return true;
		}
	}
	return false;
}

static void send_frame_done_iterator(struct sway_output *output,
		struct wlr_surface *surface, struct wlr_box *box, float rotation,
		void *data) {
	struct timespec *when = data;
	wlr_surface_send_frame_done(surface, when);
}

static void send_frame_done(struct sway_output *output, struct timespec *when) {
	output_for_each_surface(output, send_frame_done_iterator, when);
}

static void damage_handle_frame(struct wl_listener *listener, void *data) {
	struct sway_output *output =
		wl_container_of(listener, output, damage_frame);

	if (!output->wlr_output->enabled) {
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	bool needs_swap;
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	if (!wlr_output_damage_make_current(output->damage, &needs_swap, &damage)) {
		return;
	}

	if (needs_swap) {
		output_render(output, &now, &damage);
	}

	pixman_region32_fini(&damage);

	// Send frame done to all visible surfaces
	send_frame_done(output, &now);
}

void output_damage_whole(struct sway_output *output) {
	// The output can exist with no wlr_output if it's just been disconnected
	// and the transaction to evacuate it has't completed yet.
	if (output && output->wlr_output) {
		wlr_output_damage_add_whole(output->damage);
	}
}

static void damage_surface_iterator(struct sway_output *output,
		struct wlr_surface *surface, struct wlr_box *_box, float rotation,
		void *_data) {
	bool *data = _data;
	bool whole = *data;

	struct wlr_box box = *_box;
	scale_box(&box, output->wlr_output->scale);

	int center_x = box.x + box.width/2;
	int center_y = box.y + box.height/2;

	if (pixman_region32_not_empty(&surface->buffer_damage)) {
		enum wl_output_transform transform =
			wlr_output_transform_invert(surface->current.transform);

		pixman_region32_t damage;
		pixman_region32_init(&damage);
		pixman_region32_copy(&damage, &surface->buffer_damage);
		wlr_region_transform(&damage, &damage, transform,
			surface->current.buffer_width, surface->current.buffer_height);
		wlr_region_scale(&damage, &damage,
			output->wlr_output->scale / (float)surface->current.scale);
		if (ceil(output->wlr_output->scale) > surface->current.scale) {
			// When scaling up a surface, it'll become blurry so we need to
			// expand the damage region
			wlr_region_expand(&damage, &damage,
				ceil(output->wlr_output->scale) - surface->current.scale);
		}
		pixman_region32_translate(&damage, box.x, box.y);
		wlr_region_rotated_bounds(&damage, &damage, rotation,
			center_x, center_y);
		wlr_output_damage_add(output->damage, &damage);
		pixman_region32_fini(&damage);
	}

	if (whole) {
		wlr_box_rotated_bounds(&box, rotation, &box);
		wlr_output_damage_add_box(output->damage, &box);
	}

	wlr_output_schedule_frame(output->wlr_output);
}

void output_damage_surface(struct sway_output *output, double ox, double oy,
		struct wlr_surface *surface, bool whole) {
	output_surface_for_each_surface(output, surface, ox, oy,
		damage_surface_iterator, &whole);
}

static void output_damage_view(struct sway_output *output,
		struct sway_view *view, bool whole) {
	if (!view_is_visible(view)) {
		return;
	}
	output_view_for_each_surface(output, view, damage_surface_iterator, &whole);
}

void output_damage_from_view(struct sway_output *output,
		struct sway_view *view) {
	output_damage_view(output, view, false);
}

// Expecting an unscaled box in layout coordinates
void output_damage_box(struct sway_output *output, struct wlr_box *_box) {
	struct wlr_box box;
	memcpy(&box, _box, sizeof(struct wlr_box));
	box.x -= output->wlr_output->lx;
	box.y -= output->wlr_output->ly;
	scale_box(&box, output->wlr_output->scale);
	wlr_output_damage_add_box(output->damage, &box);
}

void output_damage_whole_container(struct sway_output *output,
		struct sway_container *con) {
	// Pad the box by 1px, because the width is a double and might be a fraction
	struct wlr_box box = {
		.x = con->current.con_x - output->wlr_output->lx - 1,
		.y = con->current.con_y - output->wlr_output->ly - 1,
		.width = con->current.con_width + 2,
		.height = con->current.con_height + 2,
	};
	scale_box(&box, output->wlr_output->scale);
	wlr_output_damage_add_box(output->damage, &box);
}

static void damage_handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_output *output =
		wl_container_of(listener, output, damage_destroy);
	output_disable(output);
	transaction_commit_dirty();
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, destroy);
	wl_signal_emit(&output->events.destroy, output);

	if (output->enabled) {
		output_disable(output);
	}
	output_begin_destroy(output);

	transaction_commit_dirty();
}

static void handle_mode(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, mode);
	arrange_layers(output);
	arrange_output(output);
	transaction_commit_dirty();
}

static void handle_transform(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, transform);
	arrange_layers(output);
	arrange_output(output);
	transaction_commit_dirty();
}

static void update_textures(struct sway_container *con, void *data) {
	container_update_title_textures(con);
	if (con->view) {
		view_update_marks_textures(con->view);
	}
}

static void handle_scale(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, scale);
	arrange_layers(output);
	output_for_each_container(output, update_textures, NULL);
	arrange_output(output);
	transaction_commit_dirty();
}

static void send_presented_iterator(struct sway_output *output,
		struct wlr_surface *surface, struct wlr_box *box, float rotation,
		void *data) {
	struct wlr_presentation_event *event = data;
	wlr_presentation_send_surface_presented(server.presentation,
		surface, event);
}

static void handle_present(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, present);
	struct wlr_output_event_present *output_event = data;

	struct wlr_presentation_event event = {
		.output = output->wlr_output,
		.tv_sec = (uint64_t)output_event->when->tv_sec,
		.tv_nsec = (uint32_t)output_event->when->tv_nsec,
		.refresh = (uint32_t)output_event->refresh,
		.seq = (uint64_t)output_event->seq,
		.flags = output_event->flags,
	};
	output_for_each_surface(output, send_presented_iterator, &event);
}

void handle_new_output(struct wl_listener *listener, void *data) {
	struct sway_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	wlr_log(WLR_DEBUG, "New output %p: %s", wlr_output, wlr_output->name);

	struct sway_output *output = output_create(wlr_output);
	if (!output) {
		return;
	}
	output->server = server;
	output->damage = wlr_output_damage_create(wlr_output);
	output->destroy.notify = handle_destroy;

	struct output_config *oc = output_find_config(output);

	if (!oc || oc->enabled) {
		output_enable(output, oc);
	}

	transaction_commit_dirty();
}

void output_add_listeners(struct sway_output *output) {
	output->mode.notify = handle_mode;
	output->transform.notify = handle_transform;
	output->scale.notify = handle_scale;
	output->present.notify = handle_present;
	output->damage_frame.notify = damage_handle_frame;
	output->damage_destroy.notify = damage_handle_destroy;
}

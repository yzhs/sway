#define _POSIX_C_SOURCE 200809L
#include <strings.h>
#include <wlr/util/log.h>
#include "config.h"
#include "log.h"
#include "sway/commands.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "stringop.h"

static const char* EXPECTED_SYNTAX =
	"Expected 'swap container with id|con_id|mark <arg>'";

static void swap_places(struct sway_container *con1,
		struct sway_container *con2) {
	struct sway_container *temp = malloc(sizeof(struct sway_container));
	temp->x = con1->x;
	temp->y = con1->y;
	temp->width = con1->width;
	temp->height = con1->height;
	temp->parent = con1->parent;
	temp->workspace = con1->workspace;

	con1->x = con2->x;
	con1->y = con2->y;
	con1->width = con2->width;
	con1->height = con2->height;

	con2->x = temp->x;
	con2->y = temp->y;
	con2->width = temp->width;
	con2->height = temp->height;

	int temp_index = container_sibling_index(con1);
	if (con2->parent) {
		container_insert_child(con2->parent, con1,
				container_sibling_index(con2));
	} else {
		workspace_insert_tiling(con2->workspace, con1,
				container_sibling_index(con2));
	}
	if (temp->parent) {
		container_insert_child(temp->parent, con2, temp_index);
	} else {
		workspace_insert_tiling(temp->workspace, con2, temp_index);
	}

	free(temp);
}

static void swap_focus(struct sway_container *con1,
		struct sway_container *con2, struct sway_seat *seat,
		struct sway_container *focus) {
	if (focus == con1 || focus == con2) {
		struct sway_workspace *ws1 = con1->workspace;
		struct sway_workspace *ws2 = con2->workspace;
		enum sway_container_layout layout1 = container_parent_layout(con1);
		enum sway_container_layout layout2 = container_parent_layout(con2);
		if (focus == con1 && layout2 == L_STACKED) {
			if (workspace_is_visible(ws2)) {
				seat_set_focus(seat, &con2->node);
			}
			seat_set_focus_container(seat, ws1 != ws2 ? con2 : con1);
		} else if (focus == con2 && layout1 == L_STACKED) {
			if (workspace_is_visible(ws1)) {
				seat_set_focus(seat, &con1->node);
			}
			seat_set_focus_container(seat, ws1 != ws2 ? con1 : con2);
		} else if (ws1 != ws2) {
			seat_set_focus_container(seat, focus == con1 ? con2 : con1);
		} else {
			seat_set_focus_container(seat, focus);
		}
	} else {
		seat_set_focus_container(seat, focus);
	}
}

static void container_swap(struct sway_container *con1,
		struct sway_container *con2) {
	if (!sway_assert(con1 && con2, "Cannot swap with nothing")) {
		return;
	}
	if (!sway_assert(!container_has_ancestor(con1, con2)
				&& !container_has_ancestor(con2, con1),
				"Cannot swap ancestor and descendant")) {
		return;
	}
	if (!sway_assert(!container_is_floating(con1)
				&& !container_is_floating(con2),
				"Swapping with floating containers is not supported")) {
		return;
	}

	wlr_log(WLR_DEBUG, "Swapping containers %zu and %zu",
			con1->node.id, con2->node.id);

	bool fs1 = con1->is_fullscreen;
	bool fs2 = con2->is_fullscreen;
	if (fs1) {
		container_set_fullscreen(con1, false);
	}
	if (fs2) {
		container_set_fullscreen(con2, false);
	}

	struct sway_seat *seat = input_manager_get_default_seat();
	struct sway_container *focus = seat_get_focused_container(seat);
	struct sway_workspace *vis1 =
		output_get_active_workspace(con1->workspace->output);
	struct sway_workspace *vis2 =
		output_get_active_workspace(con2->workspace->output);

	char *stored_prev_name = NULL;
	if (seat->prev_workspace_name) {
		stored_prev_name = strdup(seat->prev_workspace_name);
	}

	swap_places(con1, con2);

	if (!workspace_is_visible(vis1)) {
		seat_set_focus(seat, seat_get_focus_inactive(seat, &vis1->node));
	}
	if (!workspace_is_visible(vis2)) {
		seat_set_focus(seat, seat_get_focus_inactive(seat, &vis2->node));
	}

	swap_focus(con1, con2, seat, focus);

	if (stored_prev_name) {
		free(seat->prev_workspace_name);
		seat->prev_workspace_name = stored_prev_name;
	}

	if (fs1) {
		container_set_fullscreen(con2, true);
	}
	if (fs2) {
		container_set_fullscreen(con1, true);
	}
}

static bool test_con_id(struct sway_container *container, void *con_id) {
	return container->node.id == (size_t)con_id;
}

static bool test_id(struct sway_container *container, void *id) {
#ifdef HAVE_XWAYLAND
	xcb_window_t *wid = id;
	return (container->view && container->view->type == SWAY_VIEW_XWAYLAND
			&& container->view->wlr_xwayland_surface->window_id == *wid);
#else
	return false;
#endif
}

static bool test_mark(struct sway_container *container, void *mark) {
	if (container->view && container->view->marks->length) {
		return !list_seq_find(container->view->marks,
				(int (*)(const void *, const void *))strcmp, mark);
	}
	return false;
}

struct cmd_results *cmd_swap(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "swap", EXPECTED_AT_LEAST, 4))) {
		return error;
	}
	if (!root->outputs->length) {
		return cmd_results_new(CMD_INVALID, "swap",
				"Can't run this command while there's no outputs connected.");
	}

	if (strcasecmp(argv[0], "container") || strcasecmp(argv[1], "with")) {
		return cmd_results_new(CMD_INVALID, "swap", EXPECTED_SYNTAX);
	}

	struct sway_container *current = config->handler_context.container;
	struct sway_container *other;

	char *value = join_args(argv + 3, argc - 3);
	if (strcasecmp(argv[2], "id") == 0) {
#ifdef HAVE_XWAYLAND
		xcb_window_t id = strtol(value, NULL, 0);
		other = root_find_container(test_id, (void *)&id);
#endif
	} else if (strcasecmp(argv[2], "con_id") == 0) {
		size_t con_id = atoi(value);
		other = root_find_container(test_con_id, (void *)con_id);
	} else if (strcasecmp(argv[2], "mark") == 0) {
		other = root_find_container(test_mark, (void *)value);
	} else {
		free(value);
		return cmd_results_new(CMD_INVALID, "swap", EXPECTED_SYNTAX);
	}

	if (!other) {
		error = cmd_results_new(CMD_FAILURE, "swap",
				"Failed to find %s '%s'", argv[2], value);
	} else if (!current) {
		error = cmd_results_new(CMD_FAILURE, "swap",
				"Can only swap with containers and views");
	} else if (container_has_ancestor(current, other)
			|| container_has_ancestor(other, current)) {
		error = cmd_results_new(CMD_FAILURE, "swap",
				"Cannot swap ancestor and descendant");
	} else if (container_is_floating(current) || container_is_floating(other)) {
		error = cmd_results_new(CMD_FAILURE, "swap",
				"Swapping with floating containers is not supported");
	}

	free(value);

	if (error) {
		return error;
	}

	container_swap(current, other);

	arrange_node(node_get_parent(&current->node));
	if (node_get_parent(&other->node) != node_get_parent(&current->node)) {
		arrange_node(node_get_parent(&other->node));
	}

	return cmd_results_new(CMD_SUCCESS, NULL, NULL);
}

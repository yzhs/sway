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

struct cmd_results *cmd_move_to_master(int argc, char **argv) {
	struct cmd_results *error = NULL;

	struct sway_container *current = config->handler_context.container;
	struct sway_container *other = (struct sway_container *)container_get_current_siblings(current)->items[0];

	if (!other) {
		error = cmd_results_new(CMD_FAILURE, "move_to_master",
				"Cannot find master");
	} else if (!current) {
		error = cmd_results_new(CMD_FAILURE, "move_to_master",
				"Can only move containers and views to master");
	} else if (container_has_ancestor(current, other)
			|| container_has_ancestor(other, current)) {
		error = cmd_results_new(CMD_FAILURE, "move_to_master",
				"Cannot swap ancestor and descendant");
	} else if (container_is_floating(current) || container_is_floating(other)) {
		error = cmd_results_new(CMD_FAILURE, "move_to_master",
				"Moving floating containers to master is not supported");
	}

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
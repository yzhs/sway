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

void container_swap(struct sway_container *, struct sway_container *);

struct cmd_results *cmd_move_to_master(int argc, char **argv) {
	struct cmd_results *error = NULL;

	struct sway_container *current = config->handler_context.container;
	struct sway_container *other = (struct sway_container *)container_get_current_siblings(current)->items[0];

	if (current == other) {
		wlr_log(WLR_DEBUG, "The container is already at the master position");
		return cmd_results_new(CMD_SUCCESS, NULL, NULL);
	}

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
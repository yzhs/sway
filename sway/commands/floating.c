#include <string.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "list.h"

struct cmd_results *cmd_floating(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "floating", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (!root->outputs->length) {
		return cmd_results_new(CMD_INVALID, "floating",
				"Can't run this command while there's no outputs connected.");
	}
	struct sway_container *container = config->handler_context.container;
	struct sway_workspace *workspace = config->handler_context.workspace;
	if (!container && workspace->tiling->length == 0) {
		return cmd_results_new(CMD_INVALID, "floating",
				"Can't float an empty workspace");
	}
	if (!container) {
		// Wrap the workspace's children in a container so we can float it
		container = workspace_wrap_children(workspace);
		workspace->layout = L_TALL;
		seat_set_focus_container(config->handler_context.seat, container);
	}

	// If the container is in a floating split container,
	// operate on the split container instead of the child.
	if (container_is_floating_or_child(container)) {
		while (container->parent) {
			container = container->parent;
		}
	}

	bool wants_floating;
	if (strcasecmp(argv[0], "enable") == 0) {
		wants_floating = true;
	} else if (strcasecmp(argv[0], "disable") == 0) {
		wants_floating = false;
	} else if (strcasecmp(argv[0], "toggle") == 0) {
		wants_floating = !container_is_floating(container);
	} else {
		return cmd_results_new(CMD_FAILURE, "floating",
			"Expected 'floating <enable|disable|toggle>'");
	}

	container_set_floating(container, wants_floating);

	arrange_workspace(container->workspace);

	return cmd_results_new(CMD_SUCCESS, NULL, NULL);
}

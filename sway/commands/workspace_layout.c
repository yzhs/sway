#include <string.h>
#include <strings.h>
#include "sway/commands.h"

struct cmd_results *cmd_workspace_layout(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "workspace_layout", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (strcasecmp(argv[0], "default") == 0 || strcasecmp(argv[0], "tall") == 0) {
		config->default_layout = L_TALL;
	} else if (strcasecmp(argv[0], "stacking") == 0) {
		config->default_layout = L_STACKED;
	} else {
		return cmd_results_new(CMD_INVALID, "workspace_layout",
				"Expected 'workspace_layout <default|stacking|tabbed>'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL, NULL);
}

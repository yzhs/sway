#include <string.h>
#include <strings.h>
#include "sway/commands.h"

struct cmd_results *cmd_default_orientation(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "default_orientation", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (strcasecmp(argv[0], "horizontal") == 0) {
		// Do nothing
	} else if (strcasecmp(argv[0], "vertical") == 0) {
		// Do nothing
	} else if (strcasecmp(argv[0], "auto") == 0) {
		// Do nothing
	} else {
		return cmd_results_new(CMD_INVALID, "default_orientation",
				"Expected 'orientation <horizontal|vertical|auto>'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL, NULL);
}

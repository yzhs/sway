#define _POSIX_C_SOURCE 200809
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <json-c/json.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/criteria.h"
#include "sway/security.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/tree/view.h"
#include "stringop.h"
#include "log.h"

// Returns error object, or NULL if check succeeds.
struct cmd_results *checkarg(int argc, const char *name, enum expected_args type, int val) {
	const char *error_name = NULL;
	switch (type) {
	case EXPECTED_AT_LEAST:
		if (argc < val) {
			error_name = "at least ";
		}
		break;
	case EXPECTED_AT_MOST:
		if (argc > val) {
			error_name = "at most ";
		}
		break;
	case EXPECTED_EQUAL_TO:
		if (argc != val) {
			error_name = "";
		}
	}
	return error_name ?
		cmd_results_new(CMD_INVALID, name, "Invalid %s command "
				"(expected %s%d argument%s, got %d)",
				name, error_name, val, val != 1 ? "s" : "", argc)
		: NULL;
}

void apply_seat_config(struct seat_config *seat_config) {
	int i;
	i = list_seq_find(config->seat_configs, seat_name_cmp, seat_config->name);
	if (i >= 0) {
		// merge existing config
		struct seat_config *sc = config->seat_configs->items[i];
		merge_seat_config(sc, seat_config);
		free_seat_config(seat_config);
		seat_config = sc;
	} else {
		list_add(config->seat_configs, seat_config);
	}

	input_manager_apply_seat_config(seat_config);
}

/* Keep alphabetized */
static struct cmd_handler handlers[] = {
	{ "assign", cmd_assign },
	{ "bar", cmd_bar },
	{ "bindcode", cmd_bindcode },
	{ "bindsym", cmd_bindsym },
	{ "client.background", cmd_client_noop },
	{ "client.focused", cmd_client_focused },
	{ "client.focused_inactive", cmd_client_focused_inactive },
	{ "client.placeholder", cmd_client_noop },
	{ "client.unfocused", cmd_client_unfocused },
	{ "client.urgent", cmd_client_urgent },
	{ "default_border", cmd_default_border },
	{ "default_floating_border", cmd_default_floating_border },
	{ "exec", cmd_exec },
	{ "exec_always", cmd_exec_always },
	{ "floating_maximum_size", cmd_floating_maximum_size },
	{ "floating_minimum_size", cmd_floating_minimum_size },
	{ "floating_modifier", cmd_floating_modifier },
	{ "focus", cmd_focus },
	{ "focus_follows_mouse", cmd_focus_follows_mouse },
	{ "focus_on_window_activation", cmd_focus_on_window_activation },
	{ "focus_wrapping", cmd_focus_wrapping },
	{ "font", cmd_font },
	{ "for_window", cmd_for_window },
	{ "force_display_urgency_hint", cmd_force_display_urgency_hint },
	{ "force_focus_wrapping", cmd_force_focus_wrapping },
	{ "fullscreen", cmd_fullscreen },
	{ "gaps", cmd_gaps },
	{ "hide_edge_borders", cmd_hide_edge_borders },
	{ "include", cmd_include },
	{ "input", cmd_input },
	{ "mode", cmd_mode },
	{ "mouse_warping", cmd_mouse_warping },
	{ "new_float", cmd_default_floating_border },
	{ "new_window", cmd_default_border },
	{ "no_focus", cmd_no_focus },
	{ "output", cmd_output },
	{ "popup_during_fullscreen", cmd_popup_during_fullscreen },
	{ "seat", cmd_seat },
	{ "set", cmd_set },
	{ "show_marks", cmd_show_marks },
	{ "smart_borders", cmd_smart_borders },
	{ "smart_gaps", cmd_smart_gaps },
	{ "tiling_drag", cmd_tiling_drag },
	{ "workspace", cmd_workspace },
	{ "workspace_auto_back_and_forth", cmd_ws_auto_back_and_forth },
};

/* Config-time only commands. Keep alphabetized */
static struct cmd_handler config_handlers[] = {
	{ "default_orientation", cmd_default_orientation },
	{ "swaybg_command", cmd_swaybg_command },
	{ "swaynag_command", cmd_swaynag_command },
	{ "workspace_layout", cmd_workspace_layout },
};

/* Runtime-only commands. Keep alphabetized */
static struct cmd_handler command_handlers[] = {
	{ "border", cmd_border },
	{ "create_output", cmd_create_output },
	{ "exit", cmd_exit },
	{ "floating", cmd_floating },
	{ "fullscreen", cmd_fullscreen },
	{ "kill", cmd_kill },
	{ "layout", cmd_layout },
	{ "mark", cmd_mark },
	{ "move", cmd_move },
	{ "nop", cmd_nop },
	{ "opacity", cmd_opacity },
	{ "reload", cmd_reload },
	{ "rename", cmd_rename },
	{ "resize", cmd_resize },
	{ "scratchpad", cmd_scratchpad },
	{ "swap", cmd_swap },
	{ "title_format", cmd_title_format },
	{ "unmark", cmd_unmark },
	{ "urgent", cmd_urgent },
};

static int handler_compare(const void *_a, const void *_b) {
	const struct cmd_handler *a = _a;
	const struct cmd_handler *b = _b;
	return strcasecmp(a->command, b->command);
}

struct cmd_handler *find_handler(char *line, struct cmd_handler *cmd_handlers,
		int handlers_size) {
	struct cmd_handler d = { .command=line };
	struct cmd_handler *res = NULL;
	wlr_log(WLR_DEBUG, "find_handler(%s)", line);

	bool config_loading = config->reading || !config->active;

	if (!config_loading) {
		res = bsearch(&d, command_handlers,
				sizeof(command_handlers) / sizeof(struct cmd_handler),
				sizeof(struct cmd_handler), handler_compare);

		if (res) {
			return res;
		}
	}

	if (config->reading) {
		res = bsearch(&d, config_handlers,
				sizeof(config_handlers) / sizeof(struct cmd_handler),
				sizeof(struct cmd_handler), handler_compare);

		if (res) {
			return res;
		}
	}

	if (!cmd_handlers) {
		cmd_handlers = handlers;
		handlers_size = sizeof(handlers);
	}

	res = bsearch(&d, cmd_handlers,
			handlers_size / sizeof(struct cmd_handler),
			sizeof(struct cmd_handler), handler_compare);

	return res;
}

static void set_config_node(struct sway_node *node) {
	config->handler_context.node = node;
	config->handler_context.container = NULL;
	config->handler_context.workspace = NULL;

	if (node == NULL) {
		return;
	}

	switch (node->type) {
	case N_CONTAINER:
		config->handler_context.container = node->sway_container;
		config->handler_context.workspace = node->sway_container->workspace;
		break;
	case N_WORKSPACE:
		config->handler_context.workspace = node->sway_workspace;
		break;
	case N_ROOT:
	case N_OUTPUT:
		break;
	}
}

struct cmd_results *execute_command(char *_exec, struct sway_seat *seat,
		struct sway_container *con) {
	// Even though this function will process multiple commands we will only
	// return the last error, if any (for now). (Since we have access to an
	// error string we could e.g. concatenate all errors there.)
	struct cmd_results *results = NULL;
	char *exec = strdup(_exec);
	char *head = exec;
	char *cmdlist;
	char *cmd;
	list_t *views = NULL;

	if (seat == NULL) {
		// passing a NULL seat means we just pick the default seat
		seat = input_manager_get_default_seat();
		if (!sway_assert(seat, "could not find a seat to run the command on")) {
			return NULL;
		}
	}

	// This is the container or workspace which this command will run on.
	// Ignored if the command string contains criteria.
	struct sway_node *node;
	if (con) {
		node = &con->node;
	} else {
		node = seat_get_focus_inactive(seat, &root->node);
	}

	config->handler_context.seat = seat;

	head = exec;
	do {
		// Extract criteria (valid for this command list only).
		config->handler_context.using_criteria = false;
		if (*head == '[') {
			char *error = NULL;
			struct criteria *criteria = criteria_parse(head, &error);
			if (!criteria) {
				results = cmd_results_new(CMD_INVALID, head,
					"%s", error);
				free(error);
				goto cleanup;
			}
			views = criteria_get_views(criteria);
			head += strlen(criteria->raw);
			criteria_destroy(criteria);
			config->handler_context.using_criteria = true;
			// Skip leading whitespace
			head += strspn(head, whitespace);
		}
		// Split command list
		cmdlist = argsep(&head, ";");
		cmdlist += strspn(cmdlist, whitespace);
		do {
			// Split commands
			cmd = argsep(&cmdlist, ",");
			cmd += strspn(cmd, whitespace);
			if (strcmp(cmd, "") == 0) {
				wlr_log(WLR_INFO, "Ignoring empty command.");
				continue;
			}
			wlr_log(WLR_INFO, "Handling command '%s'", cmd);
			//TODO better handling of argv
			int argc;
			char **argv = split_args(cmd, &argc);
			if (strcmp(argv[0], "exec") != 0) {
				int i;
				for (i = 1; i < argc; ++i) {
					if (*argv[i] == '\"' || *argv[i] == '\'') {
						strip_quotes(argv[i]);
					}
				}
			}
			struct cmd_handler *handler = find_handler(argv[0], NULL, 0);
			if (!handler) {
				if (results) {
					free_cmd_results(results);
				}
				results = cmd_results_new(CMD_INVALID, cmd, "Unknown/invalid command");
				free_argv(argc, argv);
				goto cleanup;
			}

			// Var replacement, for all but first argument of set
			for (int i = handler->handle == cmd_set ? 2 : 1; i < argc; ++i) {
				argv[i] = do_var_replacement(argv[i]);
				unescape_string(argv[i]);
			}

			if (!config->handler_context.using_criteria) {
				set_config_node(node);
				struct cmd_results *res = handler->handle(argc-1, argv+1);
				if (res->status != CMD_SUCCESS) {
					free_argv(argc, argv);
					if (results) {
						free_cmd_results(results);
					}
					results = res;
					goto cleanup;
				}
				free_cmd_results(res);
			} else {
				for (int i = 0; i < views->length; ++i) {
					struct sway_view *view = views->items[i];
					set_config_node(&view->container->node);
					struct cmd_results *res = handler->handle(argc-1, argv+1);
					if (res->status != CMD_SUCCESS) {
						free_argv(argc, argv);
						if (results) {
							free_cmd_results(results);
						}
						results = res;
						goto cleanup;
					}
					free_cmd_results(res);
				}
			}
			free_argv(argc, argv);
		} while(cmdlist);
	} while(head);
cleanup:
	free(exec);
	list_free(views);
	if (!results) {
		results = cmd_results_new(CMD_SUCCESS, NULL, NULL);
	}
	return results;
}

// this is like execute_command above, except:
// 1) it ignores empty commands (empty lines)
// 2) it does variable substitution
// 3) it doesn't split commands (because the multiple commands are supposed to
//	  be chained together)
// 4) execute_command handles all state internally while config_command has
// some state handled outside (notably the block mode, in read_config)
struct cmd_results *config_command(char *exec) {
	struct cmd_results *results = NULL;
	int argc;
	char **argv = split_args(exec, &argc);
	if (!argc) {
		results = cmd_results_new(CMD_SUCCESS, NULL, NULL);
		goto cleanup;
	}

	// Start block
	if (argc > 1 && strcmp(argv[argc - 1], "{") == 0) {
		char *block = join_args(argv, argc - 1);
		results = cmd_results_new(CMD_BLOCK, block, NULL);
		free(block);
		goto cleanup;
	}

	// Endblock
	if (strcmp(argv[argc - 1], "}") == 0) {
		results = cmd_results_new(CMD_BLOCK_END, NULL, NULL);
		goto cleanup;
	}
	wlr_log(WLR_INFO, "handling config command '%s'", exec);
	struct cmd_handler *handler = find_handler(argv[0], NULL, 0);
	if (!handler) {
		char *input = argv[0] ? argv[0] : "(empty)";
		results = cmd_results_new(CMD_INVALID, input, "Unknown/invalid command");
		goto cleanup;
	}
	int i;
	// Var replacement, for all but first argument of set
	// TODO commands
	for (i = handler->handle == cmd_set ? 2 : 1; i < argc; ++i) {
		if (handler->handle != cmd_exec && handler->handle != cmd_exec_always
				&& handler->handle != cmd_bindsym
				&& handler->handle != cmd_bindcode
				&& handler->handle != cmd_set
				&& (*argv[i] == '\"' || *argv[i] == '\'')) {
			strip_quotes(argv[i]);
		}
		argv[i] = do_var_replacement(argv[i]);
		unescape_string(argv[i]);
	}
	if (handler->handle) {
		results = handler->handle(argc-1, argv+1);
	} else {
		results = cmd_results_new(CMD_INVALID, argv[0], "This command is shimmed, but unimplemented");
	}

cleanup:
	free_argv(argc, argv);
	return results;
}

struct cmd_results *config_subcommand(char **argv, int argc,
		struct cmd_handler *handlers, size_t handlers_size) {
	char *command = join_args(argv, argc);
	wlr_log(WLR_DEBUG, "Subcommand: %s", command);
	free(command);

	struct cmd_handler *handler = find_handler(argv[0], handlers,
			handlers_size);
	if (!handler) {
		char *input = argv[0] ? argv[0] : "(empty)";
		return cmd_results_new(CMD_INVALID, input, "Unknown/invalid command");
	}
	if (handler->handle) {
		return handler->handle(argc - 1, argv + 1);
	}
	return cmd_results_new(CMD_INVALID, argv[0],
			"This command is shimmed, but unimplemented");
}

struct cmd_results *config_commands_command(char *exec) {
	struct cmd_results *results = NULL;
	int argc;
	char **argv = split_args(exec, &argc);
	if (!argc) {
		results = cmd_results_new(CMD_SUCCESS, NULL, NULL);
		goto cleanup;
	}

	// Find handler for the command this is setting a policy for
	char *cmd = argv[0];

	if (strcmp(cmd, "}") == 0) {
		results = cmd_results_new(CMD_BLOCK_END, NULL, NULL);
		goto cleanup;
	}

	struct cmd_handler *handler = find_handler(cmd, NULL, 0);
	if (!handler && strcmp(cmd, "*") != 0) {
		results = cmd_results_new(CMD_INVALID, cmd, "Unknown/invalid command");
		goto cleanup;
	}

	enum command_context context = 0;

	struct {
		char *name;
		enum command_context context;
	} context_names[] = {
		{ "config", CONTEXT_CONFIG },
		{ "binding", CONTEXT_BINDING },
		{ "ipc", CONTEXT_IPC },
		{ "criteria", CONTEXT_CRITERIA },
		{ "all", CONTEXT_ALL },
	};

	for (int i = 1; i < argc; ++i) {
		size_t j;
		for (j = 0; j < sizeof(context_names) / sizeof(context_names[0]); ++j) {
			if (strcmp(context_names[j].name, argv[i]) == 0) {
				break;
			}
		}
		if (j == sizeof(context_names) / sizeof(context_names[0])) {
			results = cmd_results_new(CMD_INVALID, cmd,
					"Invalid command context %s", argv[i]);
			goto cleanup;
		}
		context |= context_names[j].context;
	}

	struct command_policy *policy = NULL;
	for (int i = 0; i < config->command_policies->length; ++i) {
		struct command_policy *p = config->command_policies->items[i];
		if (strcmp(p->command, cmd) == 0) {
			policy = p;
			break;
		}
	}
	if (!policy) {
		policy = alloc_command_policy(cmd);
		if (!sway_assert(policy, "Unable to allocate security policy")) {
			results = cmd_results_new(CMD_INVALID, cmd,
					"Unable to allocate memory");
			goto cleanup;
		}
		list_add(config->command_policies, policy);
	}
	policy->context = context;

	wlr_log(WLR_INFO, "Set command policy for %s to %d",
			policy->command, policy->context);

	results = cmd_results_new(CMD_SUCCESS, NULL, NULL);

cleanup:
	free_argv(argc, argv);
	return results;
}

struct cmd_results *cmd_results_new(enum cmd_status status,
		const char *input, const char *format, ...) {
	struct cmd_results *results = malloc(sizeof(struct cmd_results));
	if (!results) {
		wlr_log(WLR_ERROR, "Unable to allocate command results");
		return NULL;
	}
	results->status = status;
	if (input) {
		results->input = strdup(input); // input is the command name
	} else {
		results->input = NULL;
	}
	if (format) {
		char *error = malloc(256);
		va_list args;
		va_start(args, format);
		if (error) {
			vsnprintf(error, 256, format, args);
		}
		va_end(args);
		results->error = error;
	} else {
		results->error = NULL;
	}
	return results;
}

void free_cmd_results(struct cmd_results *results) {
	if (results->input) {
		free(results->input);
	}
	if (results->error) {
		free(results->error);
	}
	free(results);
}

char *cmd_results_to_json(struct cmd_results *results) {
	json_object *result_array = json_object_new_array();
	json_object *root = json_object_new_object();
	json_object_object_add(root, "success",
			json_object_new_boolean(results->status == CMD_SUCCESS));
	if (results->input) {
		json_object_object_add(
				root, "input", json_object_new_string(results->input));
	}
	if (results->error) {
		json_object_object_add(
				root, "error", json_object_new_string(results->error));
	}
	json_object_array_add(result_array, root);
	const char *json = json_object_to_json_string(result_array);
	char *res = strdup(json);
	json_object_put(result_array);
	return res;
}

/**
 * Check and add color to buffer.
 *
 * return error object, or NULL if color is valid.
 */
struct cmd_results *add_color(const char *name,
		char *buffer, const char *color) {
	int len = strlen(color);
	if (len != 7 && len != 9) {
		return cmd_results_new(CMD_INVALID, name,
				"Invalid color definition %s", color);
	}
	if (color[0] != '#') {
		return cmd_results_new(CMD_INVALID, name,
				"Invalid color definition %s", color);
	}
	for (int i = 1; i < len; ++i) {
		if (!isxdigit(color[i])) {
			return cmd_results_new(CMD_INVALID, name,
					"Invalid color definition %s", color);
		}
	}
	strcpy(buffer, color);
	// add default alpha channel if color was defined without it
	if (len == 7) {
		buffer[7] = 'f';
		buffer[8] = 'f';
	}
	buffer[9] = '\0';
	return NULL;
}

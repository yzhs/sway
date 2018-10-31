#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <wordexp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <libinput.h>
#include <limits.h>
#include <dirent.h>
#include <strings.h>
#ifdef __linux__
#include <linux/input-event-codes.h>
#elif __FreeBSD__
#include <dev/evdev/input-event-codes.h>
#endif
#include <wlr/types/wlr_output.h>
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/criteria.h"
#include "sway/swaynag.h"
#include "sway/tree/arrange.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"
#include "cairo.h"
#include "pango.h"
#include "readline.h"
#include "stringop.h"
#include "list.h"
#include "log.h"

struct sway_config *config = NULL;

static void free_mode(struct sway_mode *mode) {
	int i;

	if (!mode) {
		return;
	}
	free(mode->name);
	if (mode->keysym_bindings) {
		for (i = 0; i < mode->keysym_bindings->length; i++) {
			free_sway_binding(mode->keysym_bindings->items[i]);
		}
		list_free(mode->keysym_bindings);
	}
	if (mode->keycode_bindings) {
		for (i = 0; i < mode->keycode_bindings->length; i++) {
			free_sway_binding(mode->keycode_bindings->items[i]);
		}
		list_free(mode->keycode_bindings);
	}
	if (mode->mouse_bindings) {
		for (i = 0; i < mode->mouse_bindings->length; i++) {
			free_sway_binding(mode->mouse_bindings->items[i]);
		}
		list_free(mode->mouse_bindings);
	}
	free(mode);
}

void free_config(struct sway_config *config) {
	if (!config) {
		return;
	}

	memset(&config->handler_context, 0, sizeof(config->handler_context));

	// TODO: handle all currently unhandled lists as we add implementations
	if (config->symbols) {
		for (int i = 0; i < config->symbols->length; ++i) {
			free_sway_variable(config->symbols->items[i]);
		}
		list_free(config->symbols);
	}
	if (config->modes) {
		for (int i = 0; i < config->modes->length; ++i) {
			free_mode(config->modes->items[i]);
		}
		list_free(config->modes);
	}
	if (config->bars) {
		for (int i = 0; i < config->bars->length; ++i) {
			free_bar_config(config->bars->items[i]);
		}
		list_free(config->bars);
	}
	list_free(config->cmd_queue);
	if (config->workspace_configs) {
		for (int i = 0; i < config->workspace_configs->length; i++) {
			free_workspace_config(config->workspace_configs->items[i]);
		}
		list_free(config->workspace_configs);
	}
	if (config->output_configs) {
		for (int i = 0; i < config->output_configs->length; i++) {
			free_output_config(config->output_configs->items[i]);
		}
		list_free(config->output_configs);
	}
	if (config->input_configs) {
		for (int i = 0; i < config->input_configs->length; i++) {
			free_input_config(config->input_configs->items[i]);
		}
		list_free(config->input_configs);
	}
	if (config->seat_configs) {
		for (int i = 0; i < config->seat_configs->length; i++) {
			free_seat_config(config->seat_configs->items[i]);
		}
		list_free(config->seat_configs);
	}
	if (config->criteria) {
		for (int i = 0; i < config->criteria->length; ++i) {
			criteria_destroy(config->criteria->items[i]);
		}
		list_free(config->criteria);
	}
	list_free(config->no_focus);
	list_free(config->active_bar_modifiers);
	list_free(config->config_chain);
	list_free(config->command_policies);
	list_free(config->feature_policies);
	list_free(config->ipc_policies);
	free(config->floating_scroll_up_cmd);
	free(config->floating_scroll_down_cmd);
	free(config->floating_scroll_left_cmd);
	free(config->floating_scroll_right_cmd);
	free(config->font);
	free(config->swaybg_command);
	free(config->swaynag_command);
	free((char *)config->current_config_path);
	free((char *)config->current_config);
	free(config);
}

static void destroy_removed_seats(struct sway_config *old_config,
		struct sway_config *new_config) {
	struct seat_config *seat_config;
	struct sway_seat *seat;
	int i;
	for (i = 0; i < old_config->seat_configs->length; i++) {
		seat_config = old_config->seat_configs->items[i];
		/* Also destroy seats that aren't present in new config */
		if (new_config && list_seq_find(new_config->seat_configs,
				seat_name_cmp, seat_config->name) < 0) {
			seat = input_manager_get_seat(seat_config->name);
			seat_destroy(seat);
		}
	}
}

static void set_color(float dest[static 4], uint32_t color) {
	dest[0] = ((color >> 16) & 0xff) / 255.0;
	dest[1] = ((color >> 8) & 0xff) / 255.0;
	dest[2] = (color & 0xff) / 255.0;
	dest[3] = 1.0;
}

static void config_defaults(struct sway_config *config) {
	if (!(config->swaynag_command = strdup("swaynag"))) goto cleanup;
	config->swaynag_config_errors = (struct swaynag_instance){
		.args = "--type error "
			"--message 'There are errors in your config file' "
			"--detailed-message "
			"--button 'Exit sway' 'swaymsg exit' "
			"--button 'Reload sway' 'swaymsg reload'",
		.pid = -1,
		.detailed = true,
	};

	if (!(config->symbols = create_list())) goto cleanup;
	if (!(config->modes = create_list())) goto cleanup;
	if (!(config->bars = create_list())) goto cleanup;
	if (!(config->workspace_configs = create_list())) goto cleanup;
	if (!(config->criteria = create_list())) goto cleanup;
	if (!(config->no_focus = create_list())) goto cleanup;
	if (!(config->input_configs = create_list())) goto cleanup;
	if (!(config->seat_configs = create_list())) goto cleanup;
	if (!(config->output_configs = create_list())) goto cleanup;

	if (!(config->cmd_queue = create_list())) goto cleanup;

	if (!(config->current_mode = malloc(sizeof(struct sway_mode))))
		goto cleanup;
	if (!(config->current_mode->name = malloc(sizeof("default")))) goto cleanup;
	strcpy(config->current_mode->name, "default");
	if (!(config->current_mode->keysym_bindings = create_list())) goto cleanup;
	if (!(config->current_mode->keycode_bindings = create_list())) goto cleanup;
	if (!(config->current_mode->mouse_bindings = create_list())) goto cleanup;
	list_add(config->modes, config->current_mode);

	config->floating_mod = 0;
	config->floating_mod_inverse = false;
	config->dragging_key = BTN_LEFT;
	config->resizing_key = BTN_RIGHT;

	if (!(config->floating_scroll_up_cmd = strdup(""))) goto cleanup;
	if (!(config->floating_scroll_down_cmd = strdup(""))) goto cleanup;
	if (!(config->floating_scroll_left_cmd = strdup(""))) goto cleanup;
	if (!(config->floating_scroll_right_cmd = strdup(""))) goto cleanup;
	config->default_layout = L_TALL;
	config->default_orientation = L_TALL;
	if (!(config->font = strdup("monospace 10"))) goto cleanup;
	config->font_height = 17; // height of monospace 10
	config->urgent_timeout = 500;
	config->popup_during_fullscreen = POPUP_SMART;

	// floating view
	config->floating_maximum_width = 0;
	config->floating_maximum_height = 0;
	config->floating_minimum_width = 75;
	config->floating_minimum_height = 50;

	// Flags
	config->focus_follows_mouse = true;
	config->mouse_warping = WARP_OUTPUT;
	config->focus_wrapping = WRAP_YES;
	config->validating = false;
	config->reloading = false;
	config->active = false;
	config->failed = false;
	config->auto_back_and_forth = false;
	config->reading = false;
	config->show_marks = true;
	config->tiling_drag = true;

	config->smart_gaps = false;
	config->gaps_inner = 0;
	config->gaps_outer = 0;

	if (!(config->active_bar_modifiers = create_list())) goto cleanup;

	if (!(config->swaybg_command = strdup("swaybg"))) goto cleanup;

	if (!(config->config_chain = create_list())) goto cleanup;
	config->current_config_path = NULL;
	config->current_config = NULL;

	// borders
	config->border = B_NORMAL;
	config->floating_border = B_NORMAL;
	config->border_thickness = 2;
	config->floating_border_thickness = 2;
	config->hide_edge_borders = E_NONE;
	config->saved_edge_borders = E_NONE;

	// border colors
	set_color(config->border_colors.focused.border, 0x4C7899);
	set_color(config->border_colors.focused.border, 0x4C7899);
	set_color(config->border_colors.focused.background, 0x285577);
	set_color(config->border_colors.focused.text, 0xFFFFFFFF);
	set_color(config->border_colors.focused.indicator, 0x2E9EF4);
	set_color(config->border_colors.focused.child_border, 0x285577);

	set_color(config->border_colors.focused_inactive.border, 0x333333);
	set_color(config->border_colors.focused_inactive.background, 0x5F676A);
	set_color(config->border_colors.focused_inactive.text, 0xFFFFFFFF);
	set_color(config->border_colors.focused_inactive.indicator, 0x484E50);
	set_color(config->border_colors.focused_inactive.child_border, 0x5F676A);

	set_color(config->border_colors.unfocused.border, 0x333333);
	set_color(config->border_colors.unfocused.background, 0x222222);
	set_color(config->border_colors.unfocused.text, 0x88888888);
	set_color(config->border_colors.unfocused.indicator, 0x292D2E);
	set_color(config->border_colors.unfocused.child_border, 0x222222);

	set_color(config->border_colors.urgent.border, 0x2F343A);
	set_color(config->border_colors.urgent.background, 0x900000);
	set_color(config->border_colors.urgent.text, 0xFFFFFFFF);
	set_color(config->border_colors.urgent.indicator, 0x900000);
	set_color(config->border_colors.urgent.child_border, 0x900000);

	set_color(config->border_colors.placeholder.border, 0x000000);
	set_color(config->border_colors.placeholder.background, 0x0C0C0C);
	set_color(config->border_colors.placeholder.text, 0xFFFFFFFF);
	set_color(config->border_colors.placeholder.indicator, 0x000000);
	set_color(config->border_colors.placeholder.child_border, 0x0C0C0C);

	set_color(config->border_colors.background, 0xFFFFFF);

	// Security
	if (!(config->command_policies = create_list())) goto cleanup;
	if (!(config->feature_policies = create_list())) goto cleanup;
	if (!(config->ipc_policies = create_list())) goto cleanup;

	return;
cleanup:
	sway_abort("Unable to allocate config structures");
}

static bool file_exists(const char *path) {
	return path && access(path, R_OK) != -1;
}

static char *get_config_path(void) {
	static const char *config_paths[] = {
		"$HOME/.sway/config",
		"$XDG_CONFIG_HOME/sway/config",
		"$HOME/.i3/config",
		"$XDG_CONFIG_HOME/i3/config",
		SYSCONFDIR "/sway/config",
		SYSCONFDIR "/i3/config",
	};

	if (!getenv("XDG_CONFIG_HOME")) {
		char *home = getenv("HOME");
		char *config_home = malloc(strlen(home) + strlen("/.config") + 1);
		if (!config_home) {
			wlr_log(WLR_ERROR, "Unable to allocate $HOME/.config");
		} else {
			strcpy(config_home, home);
			strcat(config_home, "/.config");
			setenv("XDG_CONFIG_HOME", config_home, 1);
			wlr_log(WLR_DEBUG, "Set XDG_CONFIG_HOME to %s", config_home);
			free(config_home);
		}
	}

	wordexp_t p;
	char *path;

	int i;
	for (i = 0; i < (int)(sizeof(config_paths) / sizeof(char *)); ++i) {
		if (wordexp(config_paths[i], &p, 0) == 0) {
			path = strdup(p.we_wordv[0]);
			wordfree(&p);
			if (file_exists(path)) {
				return path;
			}
			free(path);
		}
	}

	return NULL; // Not reached
}

static bool load_config(const char *path, struct sway_config *config,
		struct swaynag_instance *swaynag) {
	if (path == NULL) {
		wlr_log(WLR_ERROR, "Unable to find a config file!");
		return false;
	}

	wlr_log(WLR_INFO, "Loading config from %s", path);

	struct stat sb;
	if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
		return false;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		wlr_log(WLR_ERROR, "Unable to open %s for reading", path);
		return false;
	}

	bool config_load_success = read_config(f, config, swaynag);
	fclose(f);

	if (!config_load_success) {
		wlr_log(WLR_ERROR, "Error(s) loading config!");
	}

	return true;
}

bool load_main_config(const char *file, bool is_active, bool validating) {
	char *path;
	if (file != NULL) {
		path = strdup(file);
	} else {
		path = get_config_path();
	}

	struct sway_config *old_config = config;
	config = calloc(1, sizeof(struct sway_config));
	if (!config) {
		sway_abort("Unable to allocate config");
	}

	config_defaults(config);
	config->validating = validating;
	if (is_active) {
		wlr_log(WLR_DEBUG, "Performing configuration file reload");
		config->reloading = true;
		config->active = true;

		swaynag_kill(&old_config->swaynag_config_errors);
		memcpy(&config->swaynag_config_errors,
				&old_config->swaynag_config_errors,
				sizeof(struct swaynag_instance));

		create_default_output_configs();
	}

	config->current_config_path = path;
	list_add(config->config_chain, path);

	config->reading = true;

	// Read security configs
	// TODO: Security
	bool success = true;
	/*
	DIR *dir = opendir(SYSCONFDIR "/sway/security.d");
	if (!dir) {
		wlr_log(WLR_ERROR,
			"%s does not exist, sway will have no security configuration"
			" and will probably be broken", SYSCONFDIR "/sway/security.d");
	} else {
		list_t *secconfigs = create_list();
		char *base = SYSCONFDIR "/sway/security.d/";
		struct dirent *ent = readdir(dir);
		struct stat s;
		while (ent != NULL) {
			char *_path = malloc(strlen(ent->d_name) + strlen(base) + 1);
			strcpy(_path, base);
			strcat(_path, ent->d_name);
			lstat(_path, &s);
			if (S_ISREG(s.st_mode) && ent->d_name[0] != '.') {
				list_add(secconfigs, _path);
			}
			else {
				free(_path);
			}
			ent = readdir(dir);
		}
		closedir(dir);

		list_qsort(secconfigs, qstrcmp);
		for (int i = 0; i < secconfigs->length; ++i) {
			char *_path = secconfigs->items[i];
			if (stat(_path, &s) || s.st_uid != 0 || s.st_gid != 0 ||
					(((s.st_mode & 0777) != 0644) &&
					(s.st_mode & 0777) != 0444)) {
				wlr_log(WLR_ERROR,
					"Refusing to load %s - it must be owned by root "
					"and mode 644 or 444", _path);
				success = false;
			} else {
				success = success && load_config(_path, config);
			}
		}

		free_flat_list(secconfigs);
	}
	*/

	success = success && load_config(path, config,
			&config->swaynag_config_errors);

	if (validating) {
		free_config(config);
		config = old_config;
		return success;
	}

	if (is_active) {
		for (int i = 0; i < config->output_configs->length; i++) {
			apply_output_config_to_outputs(config->output_configs->items[i]);
		}
		config->reloading = false;
		if (config->swaynag_config_errors.pid > 0) {
			swaynag_show(&config->swaynag_config_errors);
		}
	}

	if (old_config) {
		destroy_removed_seats(old_config, config);
		free_config(old_config);
	}
	config->reading = false;
	return success;
}

static bool load_include_config(const char *path, const char *parent_dir,
		struct sway_config *config, struct swaynag_instance *swaynag) {
	// save parent config
	const char *parent_config = config->current_config_path;

	char *full_path;
	int len = strlen(path);
	if (len >= 1 && path[0] != '/') {
		len = len + strlen(parent_dir) + 2;
		full_path = malloc(len * sizeof(char));
		if (!full_path) {
			wlr_log(WLR_ERROR,
				"Unable to allocate full path to included config");
			return false;
		}
		snprintf(full_path, len, "%s/%s", parent_dir, path);
	} else {
		full_path = strdup(path);
	}

	char *real_path = realpath(full_path, NULL);
	free(full_path);

	if (real_path == NULL) {
		wlr_log(WLR_DEBUG, "%s not found.", path);
		return false;
	}

	// check if config has already been included
	int j;
	for (j = 0; j < config->config_chain->length; ++j) {
		char *old_path = config->config_chain->items[j];
		if (strcmp(real_path, old_path) == 0) {
			wlr_log(WLR_DEBUG,
				"%s already included once, won't be included again.",
				real_path);
			free(real_path);
			return false;
		}
	}

	config->current_config_path = real_path;
	list_add(config->config_chain, real_path);
	int index = config->config_chain->length - 1;

	if (!load_config(real_path, config, swaynag)) {
		free(real_path);
		config->current_config_path = parent_config;
		list_del(config->config_chain, index);
		return false;
	}

	// restore current_config_path
	config->current_config_path = parent_config;
	return true;
}

bool load_include_configs(const char *path, struct sway_config *config,
		struct swaynag_instance *swaynag) {
	char *wd = getcwd(NULL, 0);
	char *parent_path = strdup(config->current_config_path);
	const char *parent_dir = dirname(parent_path);

	if (chdir(parent_dir) < 0) {
		free(parent_path);
		free(wd);
		return false;
	}

	wordexp_t p;

	if (wordexp(path, &p, 0) < 0) {
		free(parent_path);
		free(wd);
		return false;
	}

	char **w = p.we_wordv;
	size_t i;
	for (i = 0; i < p.we_wordc; ++i) {
		load_include_config(w[i], parent_dir, config, swaynag);
	}
	free(parent_path);
	wordfree(&p);

	// restore wd
	if (chdir(wd) < 0) {
		free(wd);
		wlr_log(WLR_ERROR, "failed to restore working directory");
		return false;
	}

	free(wd);
	return true;
}

static int detect_brace_on_following_line(FILE *file, char *line,
		int line_number) {
	int lines = 0;
	if (line[strlen(line) - 1] != '{' && line[strlen(line) - 1] != '}') {
		char *peeked = NULL;
		long position = 0;
		do {
			free(peeked);
			peeked = peek_line(file, lines, &position);
			if (peeked) {
				peeked = strip_whitespace(peeked);
			}
			lines++;
		} while (peeked && strlen(peeked) == 0);

		if (peeked && strlen(peeked) == 1 && peeked[0] == '{') {
			fseek(file, position, SEEK_SET);
		} else {
			lines = 0;
		}
		free(peeked);
	}
	return lines;
}

static char *expand_line(const char *block, const char *line, bool add_brace) {
	int size = (block ? strlen(block) + 1 : 0) + strlen(line)
		+ (add_brace ? 2 : 0) + 1;
	char *expanded = calloc(1, size);
	if (!expanded) {
		wlr_log(WLR_ERROR, "Cannot allocate expanded line buffer");
		return NULL;
	}
	snprintf(expanded, size, "%s%s%s%s", block ? block : "",
			block ? " " : "", line, add_brace ? " {" : "");
	return expanded;
}

bool read_config(FILE *file, struct sway_config *config,
		struct swaynag_instance *swaynag) {
	bool reading_main_config = false;
	char *this_config = NULL;
	size_t config_size = 0;
	if (config->current_config == NULL) {
		reading_main_config = true;

		int ret_seek = fseek(file, 0, SEEK_END);
		long ret_tell = ftell(file);
		if (ret_seek == -1 || ret_tell == -1) {
			wlr_log(WLR_ERROR, "Unable to get size of config file");
			return false;
		}
		config_size = ret_tell;
		rewind(file);

		config->current_config = this_config = calloc(1, config_size + 1);
		if (this_config == NULL) {
			wlr_log(WLR_ERROR, "Unable to allocate buffer for config contents");
			return false;
		}
	}

	bool success = true;
	int line_number = 0;
	char *line;
	list_t *stack = create_list();
	size_t read = 0;
	while (!feof(file)) {
		char *block = stack->length ? stack->items[0] : NULL;
		line = read_line(file);
		if (!line) {
			continue;
		}
		line_number++;
		wlr_log(WLR_DEBUG, "Read line %d: %s", line_number, line);

		if (reading_main_config) {
			size_t length = strlen(line);

			if (read + length > config_size) {
				wlr_log(WLR_ERROR, "Config file changed during reading");
				list_foreach(stack, free);
				list_free(stack);
				free(line);
				return false;
			}

			strcpy(this_config + read, line);
			if (line_number != 1) {
				this_config[read - 1] = '\n';
			}
			read += length + 1;
		}

		line = strip_whitespace(line);
		if (line[0] == '#') {
			free(line);
			continue;
		}
		if (strlen(line) == 0) {
			free(line);
			continue;
		}
		int brace_detected = detect_brace_on_following_line(file, line,
				line_number);
		if (brace_detected > 0) {
			line_number += brace_detected;
			wlr_log(WLR_DEBUG, "Detected open brace on line %d", line_number);
		}
		char *expanded = expand_line(block, line, brace_detected > 0);
		if (!expanded) {
			list_foreach(stack, free);
			list_free(stack);
			free(line);
			return false;
		}
		struct cmd_results *res;
		if (block && strcmp(block, "<commands>") == 0) {
			// Special case
			res = config_commands_command(expanded);
		} else {
			res = config_command(expanded);
		}
		switch(res->status) {
		case CMD_FAILURE:
		case CMD_INVALID:
			wlr_log(WLR_ERROR, "Error on line %i '%s': %s (%s)", line_number,
				line, res->error, config->current_config_path);
			if (!config->validating) {
				swaynag_log(config->swaynag_command, swaynag,
					"Error on line %i (%s) '%s': %s", line_number,
					config->current_config_path, line, res->error);
			}
			success = false;
			break;

		case CMD_DEFER:
			wlr_log(WLR_DEBUG, "Deferring command `%s'", line);
			list_add(config->cmd_queue, strdup(expanded));
			break;

		case CMD_BLOCK_COMMANDS:
			wlr_log(WLR_DEBUG, "Entering commands block");
			list_insert(stack, 0, "<commands>");
			break;

		case CMD_BLOCK:
			wlr_log(WLR_DEBUG, "Entering block '%s'", res->input);
			list_insert(stack, 0, strdup(res->input));
			if (strcmp(res->input, "bar") == 0) {
				config->current_bar = NULL;
			}
			break;

		case CMD_BLOCK_END:
			if (!block) {
				wlr_log(WLR_DEBUG, "Unmatched '}' on line %i", line_number);
				success = false;
				break;
			}
			if (strcmp(block, "bar") == 0) {
				config->current_bar = NULL;
			}

			wlr_log(WLR_DEBUG, "Exiting block '%s'", block);
			list_del(stack, 0);
			free(block);
			memset(&config->handler_context, 0,
					sizeof(config->handler_context));
		default:;
		}
		free(expanded);
		free(line);
		free_cmd_results(res);
	}
	list_foreach(stack, free);
	list_free(stack);

	return success;
}

char *do_var_replacement(char *str) {
	int i;
	char *find = str;
	while ((find = strchr(find, '$'))) {
		// Skip if escaped.
		if (find > str && find[-1] == '\\') {
			if (find == str + 1 || !(find > str + 1 && find[-2] == '\\')) {
				++find;
				continue;
			}
		}
		// Unescape double $ and move on
		if (find[1] == '$') {
			size_t length = strlen(find + 1);
			memmove(find, find + 1, length);
			find[length] = '\0';
			++find;
			continue;
		}
		// Find matching variable
		for (i = 0; i < config->symbols->length; ++i) {
			struct sway_variable *var = config->symbols->items[i];
			int vnlen = strlen(var->name);
			if (strncmp(find, var->name, vnlen) == 0) {
				int vvlen = strlen(var->value);
				char *newstr = malloc(strlen(str) - vnlen + vvlen + 1);
				if (!newstr) {
					wlr_log(WLR_ERROR,
						"Unable to allocate replacement "
						"during variable expansion");
					break;
				}
				char *newptr = newstr;
				int offset = find - str;
				strncpy(newptr, str, offset);
				newptr += offset;
				strncpy(newptr, var->value, vvlen);
				newptr += vvlen;
				strcpy(newptr, find + vnlen);
				free(str);
				str = newstr;
				find = str + offset + vvlen;
				break;
			}
		}
		if (i == config->symbols->length) {
			++find;
		}
	}
	return str;
}

// the naming is intentional (albeit long): a workspace_output_cmp function
// would compare two structs in full, while this method only compares the
// workspace.
int workspace_output_cmp_workspace(const void *a, const void *b) {
	const struct workspace_config *wsa = a, *wsb = b;
	return lenient_strcmp(wsa->workspace, wsb->workspace);
}

static void find_font_height_iterator(struct sway_container *con, void *data) {
	size_t amount_below_baseline = con->title_height - con->title_baseline;
	size_t extended_height = config->font_baseline + amount_below_baseline;
	if (extended_height > config->font_height) {
		config->font_height = extended_height;
	}
}

static void find_baseline_iterator(struct sway_container *con, void *data) {
	bool *recalculate = data;
	if (*recalculate) {
		container_calculate_title_height(con);
	}
	if (con->title_baseline > config->font_baseline) {
		config->font_baseline = con->title_baseline;
	}
}

void config_update_font_height(bool recalculate) {
	size_t prev_max_height = config->font_height;
	config->font_height = 0;
	config->font_baseline = 0;

	root_for_each_container(find_baseline_iterator, &recalculate);
	root_for_each_container(find_font_height_iterator, NULL);

	if (config->font_height != prev_max_height) {
		arrange_root();
	}
}

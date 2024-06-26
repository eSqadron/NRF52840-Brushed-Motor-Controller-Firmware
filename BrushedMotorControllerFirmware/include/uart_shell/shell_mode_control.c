// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 Maciej Baczmanski, Jakub Mazur

#if defined(CONFIG_UART_SHELL_SUPPORT)

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "driver.h"
#include "uart_shell/shell_utils.h"

static int cmd_mode(const struct shell *shell, size_t argc, char *argv[])
{
	return_codes_t ret;
	enum ControlModes mode;

	if (argc == 1) {
		ret = mode_get(&mode);
		switch (ret) {
		case (SUCCESS):
			shell_fprintf(shell, SHELL_NORMAL, "mode: %s\n", mode_names[mode]);

		break;
		case (ERR_NOT_INITIALISED):
			shell_fprintf(shell, SHELL_ERROR,
				"driver not initialised, could't get mode!\n");

		break;
		default:
			shell_fprintf(shell, SHELL_ERROR, "other error, code: %d\n", ret);
		break;
		}

		return 0;
	}

	return 0;
}

#if defined(CONFIG_POS_CONTROL_ENABLE)
static int cmd_mode_pos(const struct shell *shell, size_t argc, char *argv[])
{
	return_codes_t ret = mode_set(POSITION);

	if (ret == SUCCESS) {
		shell_fprintf(shell, SHELL_NORMAL, "mode set to: %s\n", argv[0]);

	} else {
		shell_fprintf(shell, SHELL_ERROR, "Error, code: %d\n", ret);
	}
	return 0;
}
#endif

#if defined(CONFIG_SPEED_CONTROL_ENABLE)
static int cmd_mode_speed(const struct shell *shell, size_t argc, char *argv[])
{
	return_codes_t ret = mode_set(SPEED);

	if (ret == SUCCESS) {
		shell_fprintf(shell, SHELL_NORMAL, "mode set to: %s\n", argv[0]);

	} else {
		shell_fprintf(shell, SHELL_ERROR, "Error, code: %d\n", ret);
	}
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(mode_tree,
#if defined(CONFIG_POS_CONTROL_ENABLE)
SHELL_CMD(pos,        NULL, "Set mode to position control", cmd_mode_pos),
#endif
#if defined(CONFIG_SPEED_CONTROL_ENABLE)
SHELL_CMD(speed,      NULL, "Set mode to speed control", cmd_mode_speed),
#endif
SHELL_SUBCMD_SET_END
);

SHELL_CMD_ARG_REGISTER(mode, &mode_tree, "choose mode. Default speed", cmd_mode, 1, 0);
// TODO - change default mode based on #if defined(CONFIG_POS_CONTROL_ENABLE) etc.

#endif

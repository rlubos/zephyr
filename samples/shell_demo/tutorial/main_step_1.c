/*
 * Copyright (c) 2020 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <shell/shell.h>
#include <sys/printk.h>

void main(void)
{
	printk("Shell demo\n");
}

static int cmd_radio_init(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "Initializing radio driver");

	return 0;
}

static int cmd_radio_foo(const struct shell *shell, size_t argc, char **argv)
{
	shell_print(shell, "argc = %d", argc);

	for (size_t cnt = 0; cnt < argc; cnt++) {
		shell_print(shell, "  argv[%d] = %s", cnt, argv[cnt]);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_radio,
	SHELL_CMD(init, NULL, "Initialize radio driver.", cmd_radio_init),
	SHELL_CMD(foo, NULL, "FOO radio command.", cmd_radio_foo),
	SHELL_SUBCMD_SET_END /* Array terminated. */
);
SHELL_CMD_REGISTER(radio, &sub_radio, "nRF 802.15.4 radio commands", NULL);

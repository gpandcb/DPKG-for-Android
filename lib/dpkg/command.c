/*
 * libdpkg - Debian packaging suite library routines
 * command.c - command execution support
 *
 * Copyright © 2010 Guillem Jover <guillem@debian.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <compat.h>

#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#include <dpkg/dpkg.h>
#include <dpkg/i18n.h>
#include <dpkg/command.h>

/**
 * Initialize a command structure.
 *
 * If name is NULL, then the last component of the filename path will be
 * used to initialize the name member.
 *
 * @param cmd The command structure to initialize.
 * @param filename The filename of the command to execute.
 * @param name The description of the command to execute.
 */
void
command_init(struct command *cmd, const char *filename, const char *name)
{
	cmd->filename = filename;
	if (name == NULL) {
		const char *progname = strrchr(filename, '/');

		cmd->name = progname ? progname + 1 : filename;
	} else
		cmd->name = name;
	cmd->argc = 0;
	cmd->argv_size = 10;
	cmd->argv = m_malloc(cmd->argv_size * sizeof(const char *));
	cmd->argv[0] = NULL;
}

/**
 * Destroy a command structure.
 *
 * Free the members managed by the command functions (i.e. the argv pointer
 * array), and zero all members of a command structure.
 *
 * @param cmd The command structure to free.
 */
void
command_destroy(struct command *cmd)
{
	cmd->filename = NULL;
	cmd->name = NULL;
	cmd->argc = 0;
	cmd->argv_size = 0;
	free(cmd->argv);
	cmd->argv = NULL;
}

static void
command_grow_argv(struct command *cmd, int need)
{
	/* Check if we already have enough room. */
	if ((cmd->argv_size - cmd->argc) >= need)
		return;

	cmd->argv_size = (cmd->argv_size + need) * 2;
	cmd->argv = m_realloc(cmd->argv, cmd->argv_size * sizeof(const char *));
}

/**
 * Append an argument to the command's argv.
 *
 * @param cmd The command structure to act on.
 * @param arg The argument to append to argv.
 */
void
command_add_arg(struct command *cmd, const char *arg)
{
	command_grow_argv(cmd, 1);

	cmd->argv[cmd->argc++] = arg;
	cmd->argv[cmd->argc] = NULL;
}

/**
 * Append an argument array to the command's argv.
 *
 * @param cmd The command structure to act on.
 * @param argv The NULL terminated argument array to append to argv.
 */
void
command_add_argl(struct command *cmd, const char **argv)
{
	int i, add_argc = 0;

	while (argv[add_argc] != NULL)
		add_argc++;

	command_grow_argv(cmd, add_argc);

	for (i = 0; i < add_argc; i++)
		cmd->argv[cmd->argc++] = argv[i];

	cmd->argv[cmd->argc] = NULL;
}

/**
 * Append a va_list of argument to the command's argv.
 *
 * @param cmd The command structure to act on.
 * @param al The NULL terminated va_list of argument array to append to argv.
 */
void
command_add_argv(struct command *cmd, va_list al)
{
	va_list at;
	int i, add_argc = 0;

	va_copy(at, al);
	while (va_arg(at, const char *) != NULL)
		add_argc++;
	va_end(at);

	command_grow_argv(cmd, add_argc);

	for (i = 0; i < add_argc; i++)
		cmd->argv[cmd->argc++] = va_arg(al, const char *);

	cmd->argv[cmd->argc] = NULL;
}

/**
 * Append a variable list of argument to the command's argv.
 *
 * @param cmd The command structure to act on.
 * @param ... The NULL terminated variable list of argument to append to argv.
 */
void
command_add_args(struct command *cmd, ...)
{
	va_list al;

	va_start(al, cmd);
	command_add_argv(cmd, al);
	va_end(al);
}

/**
 * Execute the command specified.
 *
 * The command is executed searching the PATH if the filename does not
 * contain any slashes, or using the full path if it's either a relative or
 * absolute pathname. This functions does not return.
 *
 * @param cmd The command structure to act on.
 */
void
command_exec(struct command *cmd)
{
	if (strchr(cmd->filename, '/'))
		execv(cmd->filename, (char * const *)cmd->argv);
	else
		execvp(cmd->filename, (char * const *)cmd->argv);
	ohshite(_("unable to execute %s (%s)"), cmd->name, cmd->filename);
}

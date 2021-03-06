/* Copyright (c) 2006-2014 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/tig.h"
#include "tig/argv.h"
#include "tig/options.h"
#include "tig/prompt.h"

bool
argv_to_string(const char *argv[SIZEOF_ARG], char *buf, size_t buflen, const char *sep)
{
	size_t bufpos, argc;

	for (bufpos = 0, argc = 0; argv[argc]; argc++)
		if (!string_nformat(buf, buflen, &bufpos, "%s%s",
				argc ? sep : "", argv[argc]))
			return FALSE;

	return TRUE;
}

static inline int
get_arg_valuelen(const char *arg, char *quoted)
{
	if (*arg == '"' || *arg == '\'') {
		const char *end = *arg == '"' ? "\"" : "'";
		int valuelen = strcspn(arg + 1, end);

		if (quoted)
			*quoted = *arg;
		return valuelen > 0 ? valuelen + 2 : strlen(arg);
	} else {
		if (quoted)
			*quoted = 0;
		return strcspn(arg, " \t");
	}
}

static bool
split_argv_string(const char *argv[SIZEOF_ARG], int *argc, char *cmd, bool remove_quotes)
{
	while (*cmd && *argc < SIZEOF_ARG) {
		char quoted = 0;
		int valuelen = get_arg_valuelen(cmd, &quoted);
		bool advance = cmd[valuelen] != 0;
		int quote_offset = !!(quoted && remove_quotes);

		cmd[valuelen - quote_offset] = 0;
		argv[(*argc)++] = chomp_string(cmd + quote_offset);
		cmd = chomp_string(cmd + valuelen + advance);
	}

	if (*argc < SIZEOF_ARG)
		argv[*argc] = NULL;
	return *argc < SIZEOF_ARG;
}

bool
argv_from_string_no_quotes(const char *argv[SIZEOF_ARG], int *argc, char *cmd)
{
	return split_argv_string(argv, argc, cmd, TRUE);
}

bool
argv_from_string(const char *argv[SIZEOF_ARG], int *argc, char *cmd)
{
	return split_argv_string(argv, argc, cmd, FALSE);
}

bool
argv_from_env(const char **argv, const char *name)
{
	char *env = argv ? getenv(name) : NULL;
	int argc = 0;

	if (env && *env)
		env = strdup(env);
	return !env || argv_from_string(argv, &argc, env);
}

void
argv_free(const char *argv[])
{
	int argc;

	if (!argv)
		return;
	for (argc = 0; argv[argc]; argc++)
		free((void *) argv[argc]);
	argv[0] = NULL;
}

size_t
argv_size(const char **argv)
{
	int argc = 0;

	while (argv && argv[argc])
		argc++;

	return argc;
}

bool
argv_contains(const char **argv, const char *arg)
{
	int i;

	for (i = 0; argv && argv[i]; i++)
		if (!strcmp(argv[i], arg))
			return TRUE;
	return FALSE;
}

DEFINE_ALLOCATOR(argv_realloc, const char *, SIZEOF_ARG)

bool
argv_append(const char ***argv, const char *arg)
{
	size_t argc = argv_size(*argv);
	char *alloc;

	if (!*arg && argc > 0)
		return TRUE;

	if (!argv_realloc(argv, argc, 2))
		return FALSE;

	alloc = strdup(arg);

	(*argv)[argc++] = alloc;
	(*argv)[argc] = NULL;

	return alloc != NULL;
}

bool
argv_append_array(const char ***dst_argv, const char *src_argv[])
{
	int i;

	for (i = 0; src_argv && src_argv[i]; i++)
		if (!argv_append(dst_argv, src_argv[i]))
			return FALSE;
	return TRUE;
}

bool
argv_remove_quotes(const char *argv[])
{
	int argc;

	for (argc = 0; argv[argc]; argc++) {
		char quoted = 0;
		const char *arg = argv[argc];
		const int arglen = get_arg_valuelen(arg, &quoted);
		const int unquotedlen = arglen - 1 - (arg[arglen - 1] == quoted);
		char *unquoted;

		if (!quoted)
			continue;

		unquoted = strndup(arg + 1, unquotedlen);
		if (!unquoted)
			return FALSE;
		free((void *) arg);
		argv[argc] = unquoted;
	}

	return TRUE;
}

bool
argv_copy(const char ***dst, const char *src[])
{
	int argc;

	argv_free(*dst);
	for (argc = 0; src[argc]; argc++)
		if (!argv_append(dst, src[argc]))
			return FALSE;
	return TRUE;
}

/*
 * Argument formatting.
 */

struct format_context;

struct format_var {
	const char *name;
	size_t namelen;
	bool (*formatter)(struct format_context *, struct format_var *);
	void *value_ref;
	const char *value_if_empty;
};

struct format_context {
	struct format_var *vars;
	size_t vars_size;
	char buf[SIZEOF_STR];
	size_t bufpos;
	bool file_filter;
};

#define ARGV_ENV_INIT(type, name, ifempty, initval)	initval,

struct argv_env argv_env = {
	ARGV_ENV_INFO(ARGV_ENV_INIT)
};

static bool
format_expand_arg(struct format_context *format, const char *name, const char *end)
{
	struct format_var *vars = format->vars;
	int i;

	if (!prefixcmp(name, "%(prompt")) {
		const char *prompt = "Command argument: ";
		char msgbuf[SIZEOF_STR];
		const char *value;
		const char *msgstart = name + STRING_SIZE("%(prompt");
		const int msglen = end - msgstart - 1;

		if (end && msglen > 0 && string_format(msgbuf, "%.*s", msglen, msgstart)) {
			const char *msg = msgbuf;

			while (isspace(*msg))
				msg++;
			if (*msg)
				prompt = msg;
		}

		value = read_prompt(prompt);
		if (value == NULL)
			return FALSE;
		return string_format_from(format->buf, &format->bufpos, "%s", value);
	}

	for (i = 0; i < format->vars_size; i++) {
		if (strncmp(name, vars[i].name, vars[i].namelen))
			continue;

		if (vars[i].value_ref == &argv_env.file && !format->file_filter)
			return TRUE;

		return vars[i].formatter(format, &vars[i]);
	}

	return FALSE;
}

static bool
format_append_arg(struct format_context *format, const char ***dst_argv, const char *arg)
{
	memset(format->buf, 0, sizeof(format->buf));
	format->bufpos = 0;

	while (arg) {
		const char *var = strstr(arg, "%(");
		const char *closing = var ? strchr(var, ')') : NULL;
		const char *next = closing ? closing + 1 : NULL;
		const int len = var ? var - arg : strlen(arg);

		if (var && !closing)
			return FALSE;

		if (len && !string_format_from(format->buf, &format->bufpos, "%.*s", len, arg))
			return FALSE;

		if (var && !format_expand_arg(format, var, next))
			return FALSE;

		arg = next;
	}

	return argv_append(dst_argv, format->buf);
}

static bool
format_append_argv(struct format_context *format, const char ***dst_argv, const char *src_argv[])
{
	int argc;

	if (!src_argv)
		return TRUE;

	for (argc = 0; src_argv[argc]; argc++)
		if (!format_append_arg(format, dst_argv, src_argv[argc]))
			return FALSE;

	return src_argv[argc] == NULL;
}

static bool
argv_string_formatter(struct format_context *format, struct format_var *var)
{
	argv_string *value_ref = var->value_ref;
	const char *value = *value_ref;

	if (!*value)
		value = var->value_if_empty;

	if (!*value)
		return TRUE;

	return string_format_from(format->buf, &format->bufpos, "%s", value);
}

static bool
argv_number_formatter(struct format_context *format, struct format_var *var)
{
	unsigned long value = *(unsigned long *) var->value_ref;

	return string_format_from(format->buf, &format->bufpos, "%ld", value);
}

bool
argv_format(struct argv_env *argv_env, const char ***dst_argv, const char *src_argv[], bool first, bool file_filter)
{
	struct format_var vars[] = {
#define FORMAT_VAR(type, name, ifempty, initval) \
	{ "%(" #name ")", STRING_SIZE("%(" #name ")"), type ## _formatter, &argv_env->name, ifempty },
		ARGV_ENV_INFO(FORMAT_VAR)
	};
	struct format_context format = { vars, ARRAY_SIZE(vars), "", 0, file_filter };
	int argc;

	argv_free(*dst_argv);

	for (argc = 0; src_argv[argc]; argc++) {
		const char *arg = src_argv[argc];

		if (!strcmp(arg, "%(fileargs)")) {
			if (file_filter && !argv_append_array(dst_argv, opt_file_args))
				break;

		} else if (!strcmp(arg, "%(diffargs)")) {
			if (!format_append_argv(&format, dst_argv, opt_diff_options))
				break;

		} else if (!strcmp(arg, "%(blameargs)")) {
			if (!format_append_argv(&format, dst_argv, opt_blame_options))
				break;

		} else if (!strcmp(arg, "%(logargs)")) {
			if (!format_append_argv(&format, dst_argv, opt_log_options))
				break;

		} else if (!strcmp(arg, "%(mainargs)")) {
			if (!format_append_argv(&format, dst_argv, opt_main_options))
				break;

		} else if (!strcmp(arg, "%(cmdlineargs)")) {
			if (!format_append_argv(&format, dst_argv, opt_cmdline_args))
				break;

		} else if (!strcmp(arg, "%(revargs)") ||
			   (first && !strcmp(arg, "%(commit)"))) {
			if (!argv_append_array(dst_argv, opt_rev_args))
				break;

		} else if (!format_append_arg(&format, dst_argv, arg)) {
			break;
		}
	}

	return src_argv[argc] == NULL;
}

static inline bool
argv_find_rev_flag(const char *argv[], size_t argc, const char *arg, size_t arglen,
		   size_t *search_offset, bool *with_graph, bool *with_reflog)
{
	int i;

	for (i = 0; i < argc; i++) {
		const char *flag = argv[i];
		size_t flaglen = strlen(flag);

		if (flaglen > arglen || strncmp(arg, flag, flaglen))
			continue;

		if (search_offset)
			*search_offset = flaglen;
		else if (flaglen != arglen && flag[flaglen - 1] != '=')
			continue;

		if (with_graph)
			*with_graph = FALSE;
		if (with_reflog)
			*with_reflog = TRUE;

		return TRUE;
	}

	return FALSE;
}

bool
argv_parse_rev_flag(const char *arg, struct rev_flags *rev_flags)
{
	static const char *with_graph[] = {
		"--after=",
		"--all",
		"--all-match",
		"--ancestry-path",
		"--author-date-order",
		"--author=",
		"--basic-regexp",
		"--before=",
		"--boundary",
		"--branches",
		"--branches=",
		"--cherry",
		"--cherry-mark",
		"--cherry-pick",
		"--committer=",
		"--date-order",
		"--dense",
		"--extended-regexp",
		"--first-parent",
		"--fixed-strings",
		"--full-history",
		"--graph",
		"--glob=",
		"--left-only",
		"--max-parents=",
		"--merge",
		"--merges",
		"--min-parents=",
		"--no-max-parents",
		"--no-merges",
		"--no-min-parents",
		"--no-walk",
		"--perl-regexp",
		"--pickaxe-all",
		"--pickaxe-regex",
		"--regexp-ignore-case",
		"--remotes",
		"--remotes=",
		"--remove-empty",
		"--reverse",
		"--right-only",
		"--simplify-by-decoration",
		"--simplify-merges",
		"--since=",
		"--skip=",
		"--sparse",
		"--stdin",
		"--tags",
		"--tags=",
		"--topo-order",
		"--until=",
		"-E",
		"-F",
		"-i",
	};
	static const char *no_graph[] = {
		"--follow",
	};
	static const char *with_reflog[] = {
		"--walk-reflogs",
		"-g",
	};
	static const char *search_no_graph[] = {
		"--grep-reflog=",
		"--grep=",
		"-G",
		"-S",
	};
	size_t arglen = strlen(arg);
	bool graph = TRUE;
	bool reflog = FALSE;
	size_t search = 0;

	if (argv_find_rev_flag(with_graph, ARRAY_SIZE(with_graph), arg, arglen, NULL, NULL, NULL) ||
	    argv_find_rev_flag(no_graph, ARRAY_SIZE(no_graph), arg, arglen, NULL, &graph, NULL) ||
	    argv_find_rev_flag(with_reflog, ARRAY_SIZE(with_reflog), arg, arglen, NULL, NULL, &reflog) ||
	    argv_find_rev_flag(search_no_graph, ARRAY_SIZE(search_no_graph), arg, arglen, &search, &graph, NULL)) {
		if (rev_flags) {
			rev_flags->search_offset = search ? search : arglen;
			rev_flags->with_graph = graph;
			rev_flags->with_reflog = reflog;
		}
		return TRUE;
	}

	return FALSE;
}

char *
argv_format_arg(struct argv_env *argv_env, const char *src_arg)
{
	const char *src_argv[] = { src_arg, NULL };
	const char **dst_argv = NULL;
	char *dst_arg = NULL;

	if (argv_format(argv_env, &dst_argv, src_argv, FALSE, TRUE))
		dst_arg = (char *) dst_argv[0];

	free(dst_argv);
	return dst_arg;
}

/* vim: set ts=8 sw=8 noexpandtab: */

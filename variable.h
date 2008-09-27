/* Yash: yet another shell */
/* variable.h: deals with shell variables and parameters */
/* (C) 2007-2008 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#ifndef YASH_VARIABLE_H
#define YASH_VARIABLE_H

#include <stddef.h>


extern char **environ;

/* variable names */
#define VAR_CDPATH	"CDPATH"
#define VAR_COLUMNS	"COLUMNS"
#define VAR_ENV		"ENV"
#define VAR_FCEDIT	"FCEDIT"
#define VAR_HISTFILE	"HISTFILE"
#define VAR_HISTSIZE	"HISTSIZE"
#define VAR_HOME	"HOME"
#define VAR_IFS		"IFS"
#define VAR_LANG	"LANG"
#define VAR_LC_ALL	"LC_ALL"
#define VAR_LC_COLLATE	"LC_COLLATE"
#define VAR_LC_CTYPE	"LC_CTYPE"
#define VAR_LC_MESSAGES	"LC_MESSAGES"
#define VAR_LC_MONETARY	"LC_MONETARY"
#define VAR_LC_NUMERIC	"LC_NUMERIC"
#define VAR_LC_TIME	"LC_TIME"
#define VAR_LINENO	"LINENO"
#define VAR_LINES	"LINES"
#define VAR_MAIL	"MAIL"
#define VAR_MAILCHECK	"MAILCHECK"
#define VAR_MAILPATH	"MAILPATH"
#define VAR_NLSPATH	"NLSPATH"
#define VAR_OLDPWD	"OLDPWD"
#define VAR_OPTARG	"OPTARG"
#define VAR_OPTIND	"OPTIND"
#define VAR_PATH	"PATH"
#define VAR_PPID	"PPID"
#define VAR_PROMPT_COMMAND "PROMPT_COMMAND"
#define VAR_PS1		"PS1"
#define VAR_PS2		"PS2"
#define VAR_PS3		"PS3"
#define VAR_PS4		"PS4"
#define VAR_PWD		"PWD"
#define VAR_RANDOM	"RANDOM"
#define VAR_YASH_VERSION "YASH_VERSION"

typedef enum path_T {
    PA_PATH, PA_CDPATH, PA_MAILPATH,
} path_T;
#define PATHTCOUNT 3
extern const char *const path_variables[PATHTCOUNT];


extern unsigned long current_lineno;

extern void init_variables(void);

extern bool is_name(const char *s)
    __attribute__((nonnull,pure));

extern bool set_variable(
	const char *name, wchar_t *value, bool local, bool export)
    __attribute__((nonnull(1)));
extern bool set_array(const char *name, void *const *values, bool local)
    __attribute__((nonnull));
extern void set_positional_parameters(void *const *values)
    __attribute__((nonnull));
struct assign_T;
extern bool do_assignments(
	const struct assign_T *assigns, bool temp, bool export);

struct get_variable {
    enum { GV_NOTFOUND, GV_SCALAR, GV_ARRAY, GV_ARRAY_CONCAT, } type;
    size_t count;
    void **values;
};
extern const wchar_t *getvar(const char *name)
    __attribute__((pure,nonnull));
extern struct get_variable get_variable(const char *name)
    __attribute__((nonnull,warn_unused_result));

extern void open_new_environment(bool temp);
extern void close_current_environment(void);

extern char **decompose_paths(const wchar_t *paths)
    __attribute__((malloc,warn_unused_result));
extern char *const *get_path_array(path_T name);

extern void clear_all_functions(void);

struct command_T;
extern bool define_function(const char *name, struct command_T *body)
    __attribute__((nonnull));
extern struct command_T *get_function(const char *name)
    __attribute__((nonnull));

extern int typeset_builtin(int argc, void **argv)
    __attribute__((nonnull));
extern int unset_builtin(int argc, void **argv)
    __attribute__((nonnull));
extern int shift_builtin(int argc, void **argv)
    __attribute__((nonnull));
extern int getopts_builtin(int argc, void **argv)
    __attribute__((nonnull));
extern int read_builtin(int argc, void **argv)
    __attribute__((nonnull));
extern const char typeset_help[], unset_help[], shift_help[],
       getopts_help[], read_help[];


#endif /* YASH_VARIABLE_H */


/* vim: set ts=8 sts=4 sw=4 noet: */

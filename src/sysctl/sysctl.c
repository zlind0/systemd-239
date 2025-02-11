/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf-files.h"
#include "def.h"
#include "fd-util.h"
#include "fileio.h"
#include "hashmap.h"
#include "log.h"
#include "pager.h"
#include "path-util.h"
#include "string-util.h"
#include "strv.h"
#include "sysctl-util.h"
#include "terminal-util.h"
#include "util.h"

static char **arg_prefixes = NULL;
static bool arg_cat_config = false;
static bool arg_no_pager = false;

typedef struct Option {
        char *key;
        char *value;
        bool ignore_failure;
} Option;

static Option *option_free(Option *o) {
        if (!o)
                return NULL;

        free(o->key);
        free(o->value);

        return mfree(o);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(Option*, option_free);
DEFINE_HASH_OPS_WITH_VALUE_DESTRUCTOR(option_hash_ops, char, string_hash_func, string_compare_func, Option, option_free);

static Option *option_new(
                const char *key,
                const char *value,
                bool ignore_failure) {

        _cleanup_(option_freep) Option *o = NULL;

        assert(key);
        assert(value);

        o = new(Option, 1);
        if (!o)
                return NULL;

        *o = (Option) {
                .key = strdup(key),
                .value = strdup(value),
                .ignore_failure = ignore_failure,
        };

        if (!o->key || !o->value)
                return NULL;

        return TAKE_PTR(o);
}

static int apply_all(OrderedHashmap *sysctl_options) {
        Option *option;
        Iterator i;
        int r = 0;

        ORDERED_HASHMAP_FOREACH(option, sysctl_options, i) {
                int k;

                k = sysctl_write(option->key, option->value);
                if (k < 0) {
                        /* If the sysctl is not available in the kernel or we are running with reduced
                         * privileges and cannot write it, then log about the issue, and proceed without
                         * failing. (EROFS is treated as a permission problem here, since that's how
                         * container managers usually protected their sysctls.) In all other cases log an
                         * error and make the tool fail. */

                        if (option->ignore_failure || k == -EROFS || ERRNO_IS_PRIVILEGE(k))
                                log_debug_errno(k, "Couldn't write '%s' to '%s', ignoring: %m", option->value, option->key);
                        else if (k == -ENOENT)
                                log_info_errno(k, "Couldn't write '%s' to '%s', ignoring: %m", option->value, option->key);
                        else {
                                log_error_errno(k, "Couldn't write '%s' to '%s': %m", option->value, option->key);
                                if (r == 0)
                                        r = k;
                        }
                }
        }

        return r;
}

static bool test_prefix(const char *p) {
        char **i;

        if (strv_isempty(arg_prefixes))
                return true;

        STRV_FOREACH(i, arg_prefixes) {
                const char *t;

                t = path_startswith(*i, "/proc/sys/");
                if (!t)
                        t = *i;
                if (path_startswith(p, t))
                        return true;
        }

        return false;
}

static int parse_file(OrderedHashmap *sysctl_options, const char *path, bool ignore_enoent) {
        _cleanup_fclose_ FILE *f = NULL;
        unsigned c = 0;
        int r;

        assert(path);

        r = search_and_fopen(path, "re", NULL, (const char**) CONF_PATHS_STRV("sysctl.d"), &f);
        if (r < 0) {
                if (ignore_enoent && r == -ENOENT)
                        return 0;

                return log_error_errno(r, "Failed to open file '%s', ignoring: %m", path);
        }

        log_debug("Parsing %s", path);
        for (;;) {
                _cleanup_(option_freep) Option *new_option = NULL;
                _cleanup_free_ char *l = NULL;
                bool ignore_failure;
                Option *existing;
                char *p, *value;
                int k;

                k = read_line(f, LONG_LINE_MAX, &l);
                if (k == 0)
                        break;
                if (k < 0)
                        return log_error_errno(k, "Failed to read file '%s', ignoring: %m", path);

                c++;

                p = strstrip(l);

                if (isempty(p))
                        continue;
                if (strchr(COMMENTS "\n", *p))
                        continue;

                value = strchr(p, '=');
                if (!value) {
                        log_error("Line is not an assignment at '%s:%u': %s", path, c, p);

                        if (r == 0)
                                r = -EINVAL;
                        continue;
                }

                *value = 0;
                value++;

                p = strstrip(p);
                ignore_failure = p[0] == '-';
                if (ignore_failure)
                        p++;

                p = sysctl_normalize(p);
                value = strstrip(value);

                if (!test_prefix(p))
                        continue;

                existing = ordered_hashmap_get(sysctl_options, p);
                if (existing) {
                        if (streq_ptr(value, existing->value)) {
                                existing->ignore_failure = existing->ignore_failure || ignore_failure;
                                continue;
                        }

                        log_debug("Overwriting earlier assignment of %s at '%s:%u'.", p, path, c);
                        option_free(ordered_hashmap_remove(sysctl_options, p));
                }

                new_option = option_new(p, value, ignore_failure);
                if (!new_option)
                        return log_oom();

                k = ordered_hashmap_put(sysctl_options, new_option->key, new_option);
                if (k < 0)
                        return log_error_errno(k, "Failed to add sysctl variable %s to hashmap: %m", p);

                TAKE_PTR(new_option);
        }

        return r;
}

static void help(void) {
        printf("%s [OPTIONS...] [CONFIGURATION FILE...]\n\n"
               "Applies kernel sysctl settings.\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "     --cat-config       Show configuration files\n"
               "     --prefix=PATH      Only apply rules with the specified prefix\n"
               "     --no-pager         Do not pipe output into a pager\n"
               , program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_CAT_CONFIG,
                ARG_PREFIX,
                ARG_NO_PAGER,
        };

        static const struct option options[] = {
                { "help",       no_argument,       NULL, 'h'            },
                { "version",    no_argument,       NULL, ARG_VERSION    },
                { "cat-config", no_argument,       NULL, ARG_CAT_CONFIG },
                { "prefix",     required_argument, NULL, ARG_PREFIX     },
                { "no-pager",   no_argument,       NULL, ARG_NO_PAGER   },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_CAT_CONFIG:
                        arg_cat_config = true;
                        break;

                case ARG_PREFIX: {
                        char *p;

                        /* We used to require people to specify absolute paths
                         * in /proc/sys in the past. This is kinda useless, but
                         * we need to keep compatibility. We now support any
                         * sysctl name available. */
                        sysctl_normalize(optarg);

                        if (path_startswith(optarg, "/proc/sys"))
                                p = strdup(optarg);
                        else
                                p = strappend("/proc/sys/", optarg);
                        if (!p)
                                return log_oom();

                        if (strv_consume(&arg_prefixes, p) < 0)
                                return log_oom();

                        break;
                }

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (arg_cat_config && argc > optind) {
                log_error("Positional arguments are not allowed with --cat-config");
                return -EINVAL;
        }

        return 1;
}

int main(int argc, char *argv[]) {
        _cleanup_(ordered_hashmap_freep) OrderedHashmap *sysctl_options = NULL;
        int r = 0, k;

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        sysctl_options = ordered_hashmap_new(&option_hash_ops);
        if (!sysctl_options) {
                r = log_oom();
                goto finish;
        }

        r = 0;

        if (argc > optind) {
                int i;

                for (i = optind; i < argc; i++) {
                        k = parse_file(sysctl_options, argv[i], false);
                        if (k < 0 && r == 0)
                                r = k;
                }
        } else {
                _cleanup_strv_free_ char **files = NULL;
                char **f;

                r = conf_files_list_strv(&files, ".conf", NULL, 0, (const char**) CONF_PATHS_STRV("sysctl.d"));
                if (r < 0) {
                        log_error_errno(r, "Failed to enumerate sysctl.d files: %m");
                        goto finish;
                }

                if (arg_cat_config) {
                        (void) pager_open(arg_no_pager, false);

                        r = cat_files(NULL, files, 0);
                        goto finish;
                }

                STRV_FOREACH(f, files) {
                        k = parse_file(sysctl_options, *f, true);
                        if (k < 0 && r == 0)
                                r = k;
                }
        }

        k = apply_all(sysctl_options);
        if (k < 0 && r == 0)
                r = k;

finish:
        pager_close();

        strv_free(arg_prefixes);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

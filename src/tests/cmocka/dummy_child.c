/*
    SSSD

    Tests -- a simple test process that echoes input back

    Authors:
        Jakub Hrozek <jhrozek@redhat.com>

    Copyright (C) 2014 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <popt.h>

#include "util/util.h"
#include "util/child_common.h"

int main(int argc, const char *argv[])
{
    int opt;
    int debug_fd = -1;
    poptContext pc;
    const char *action = NULL;
    const char *guitar;
    const char *drums;

    struct poptOption long_options[] = {
        POPT_AUTOHELP
        {"debug-level", 'd', POPT_ARG_INT, &debug_level, 0,
         _("Debug level"), NULL},
        {"debug-timestamps", 0, POPT_ARG_INT, &debug_timestamps, 0,
         _("Add debug timestamps"), NULL},
        {"debug-microseconds", 0, POPT_ARG_INT, &debug_microseconds, 0,
         _("Show timestamps with microseconds"), NULL},
        {"debug-fd", 0, POPT_ARG_INT, &debug_fd, 0,
         _("An open file descriptor for the debug logs"), NULL},
        {"debug-to-stderr", 0, POPT_ARG_NONE | POPT_ARGFLAG_DOC_HIDDEN, &debug_to_stderr, 0, \
         _("Send the debug output to stderr directly."), NULL },
        {"guitar", 0, POPT_ARG_STRING, &guitar, 0, _("Who plays guitar"), NULL },
        {"drums", 0, POPT_ARG_STRING, &drums, 0, _("Who plays drums"), NULL },
        POPT_TABLEEND
    };

    /* Set debug level to invalid value so we can decide if -d 0 was used. */
    debug_level = SSSDBG_INVALID;

    pc = poptGetContext(argv[0], argc, argv, long_options, 0);
    while((opt = poptGetNextOpt(pc)) != -1) {
        switch(opt) {
        default:
        fprintf(stderr, "\nInvalid option %s: %s\n\n",
                  poptBadOption(pc, 0), poptStrerror(opt));
            poptPrintUsage(pc, stderr, 0);
            poptFreeContext(pc);
            _exit(1);
        }
    }
    poptFreeContext(pc);

    action = getenv("TEST_CHILD_ACTION");
    if (action) {
        if (strcasecmp(action, "check_extra_args") == 0) {
            if (!(strcmp(guitar, "george") == 0 \
                        && strcmp(drums, "ringo") == 0)) {
                DEBUG(SSSDBG_CRIT_FAILURE, "This band sounds weird\n");
                _exit(1);
            }
        }
    }

    DEBUG(SSSDBG_TRACE_FUNC, "test_child completed successfully\n");
    _exit(0);
}

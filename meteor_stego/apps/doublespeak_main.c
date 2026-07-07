#include "doublespeak.h"

#include <stdio.h>

int main(int argc, char** argv)
{
    DoublespeakArgs args;
    if (doublespeak_parse_args(argc, argv, &args) != 0) {
        doublespeak_print_usage(argc > 0 ? argv[0] : "doublespeak");
        return 1;
    }

    int rc = doublespeak_run(&args);
    if (rc > 0) {
        /* rc == -1 means doublespeak_run() already printed a specific
           message to stderr (e.g. a file I/O failure); only the METEOR_ERR_*
           codes need this generic description. */
        fprintf(stderr, "doublespeak: %s (code %d)\n",
                doublespeak_error_string(rc), rc);
    }
    return rc == 0 ? 0 : 1;
}

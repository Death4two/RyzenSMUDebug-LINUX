/*
 * Entry point: dispatch to CLI or GUI.
 */
#include <stdio.h>
#include <string.h>
#include <libsmu.h>
#include "smu_common.h"

extern int cli_main(int argc, char **argv);
#ifdef HAVE_GTK
extern int gui_main(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
    smu_obj_t *o;

    smu_setup_signals();
    if (smu_elevate_if_necessary(argc, argv) == 0)
        return 1;

    o = smu_get_obj();
    if (smu_init(o) != SMU_Return_OK) {
        fprintf(stderr, "SMU init failed. Is the ryzen_smu module loaded?\n");
        fprintf(stderr, "  sudo modprobe ryzen_smu\n");
        return 1;
    }

#ifdef HAVE_GTK
    if (argc > 1 && strcmp(argv[1], "--gui") == 0)
        return gui_main(argc, argv);
#endif

    return cli_main(argc, argv);
}

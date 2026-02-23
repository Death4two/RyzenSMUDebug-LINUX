/*
 * Entry point: dispatch to CLI or GUI.
 */
#include <stdio.h>
#include <string.h>
#include <libsmu.h>
#include "smu_common.h"

static int wants_gui(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gui") == 0 || strcmp(argv[i], "-g") == 0)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    smu_obj_t *o;
    int elev;
    int gui = wants_gui(argc, argv);

#ifndef HAVE_GTK
    if (gui) {
        fprintf(stderr, "GUI not available (built without GTK4 support).\n");
        return 1;
    }
#endif

    smu_restore_env(&argc, argv);
    smu_setup_signals();
    elev = smu_elevate_if_necessary(argc, argv);
    if (elev <= 0)
        return elev == 0 ? 1 : 0;

    o = smu_get_obj();
    if (smu_init(o) != SMU_Return_OK) {
        fprintf(stderr, "SMU init failed. Is the ryzen_smu module loaded?\n");
        fprintf(stderr, "  sudo modprobe ryzen_smu\n");
        return 1;
    }

#ifdef HAVE_GTK
    if (gui)
        return gui_main(argc, argv);
#endif

    return cli_main(argc, argv);
}

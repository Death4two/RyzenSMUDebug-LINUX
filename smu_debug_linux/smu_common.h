/*
 * Shared API for Ryzen SMU Debug Tool (CLI and GUI).
 */
#ifndef SMU_COMMON_H
#define SMU_COMMON_H

#include <libsmu.h>

/* Get the global SMU object (valid after smu_init). */
smu_obj_t *smu_get_obj(void);

/* Launcher: call before smu_init. */
void smu_setup_signals(void);
int smu_elevate_if_necessary(int argc, char **argv);
void smu_restore_env(int *argc, char **argv);

/* System info (require smu_init). */
const char *smu_get_processor_name(void);
void smu_get_cpu_family_model(unsigned int *fam, unsigned int *model);
int smu_get_topology(unsigned int *ccds, unsigned int *ccxs,
                     unsigned int *cores_per_ccx, unsigned int *phys_cores);
int smu_get_if_version_int(void);

/* FMax (boost limit): Get 0x6E; Set: 0x5C (Zen2/Zen3), 0x70 SetBoostLimitFrequencyAllCores (Zen4/Zen5). Arg0 = MHz. */
int smu_get_fmax(unsigned int *mhz_out);
int smu_set_fmax(unsigned int mhz);

/* Curve Optimizer (PSM margin). Command ID may be platform-specific (e.g. 0x76). */
int smu_set_curve_optimizer(int core_index, int margin);
int smu_get_curve_optimizer(int core_index, int *margin_out);

/* Entry points (launcher.c calls these). */
int cli_main(int argc, char **argv);
#if defined(HAVE_GTK)
int gui_main(int argc, char **argv);
#endif

#endif

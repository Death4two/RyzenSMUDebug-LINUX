/*
 * Ryzen SMU Debug Tool for Linux
 * Copyright (C) 2025
 *
 * A Linux port of the Windows ZenStates SMU Debug Tool.
 * Uses the ryzen_smu kernel driver (via libsmu) to communicate with the
 * AMD System Management Unit through the SMN address space and PM tables.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE

#include <math.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <cpuid.h>
#include <stdio.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>

#include <libsmu.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Constants                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define TOOL_VERSION            "1.0.0"
#define PM_TABLE_MAX_BYTES      0x1AB0
#define SMU_SCAN_RETRIES        8192

/* Box-drawing characters for table output */
#define BOX_TL  "╭"
#define BOX_TR  "╮"
#define BOX_BL  "╰"
#define BOX_BR  "╯"
#define BOX_H   "─"
#define BOX_V   "│"
#define BOX_TJ  "┬"
#define BOX_BJ  "┴"
#define BOX_LJ  "├"
#define BOX_RJ  "┤"
#define BOX_CJ  "┼"

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Global State                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

static smu_obj_t obj;
static volatile sig_atomic_t g_running = 1;

/* Discovered mailbox addresses from scanning */
typedef struct {
    uint32_t msg_addr;
    uint32_t rsp_addr;
    uint32_t arg_addr;
} mailbox_match_t;

#define MAX_MAILBOX_MATCHES 32
static mailbox_match_t g_matches[MAX_MAILBOX_MATCHES];
static int g_match_count = 0;

/* RSMU command IDs (Matisse/Vermeer/Raphael - see rsmu_commands.md) */
#define SMU_CMD_GET_MAX_FREQUENCY    0x6E
#define SMU_CMD_SET_FMAX_ALL_CORES  0x5C
/* Curve Optimizer / PSM margin. Zen2/Zen3 SET = 0x76 (args[0]=mask, args[1]=margin).
 * Zen4/Zen5 use SET 0x6 with single combined arg (mask|margin in low 16 bits). */
#define SMU_CMD_SET_PSM_MARGIN       0x76  /* Zen2/Zen3 */
#define PSM_SET_ZEN4_ZEN5           0x6   /* Zen4, Zen5, Granite Ridge (Zen5Settings) */
#define CO_MARGIN_MIN                (-60)
#define CO_MARGIN_MAX                10
/* RSMU GetDldoPsmMargin IDs from ZenStates-Core per platform */
#define PSM_GET_ZEN3        0x7C  /* Matisse, Vermeer, Milan, Chagall (Zen3Settings) */
#define PSM_GET_ZEN4_ZEN5   0xD5  /* Raphael, Granite Ridge, Zen5 (Zen4Settings, Zen5Settings, DragonRange) */
#define PSM_GET_ZEN5_SP     0xA3  /* Shimada Peak (Zen5Settings_ShimadaPeak) */
#define PSM_GET_PHOENIX     0xE1  /* Phoenix APU */
#define PSM_GET_CEZANNE     0xC3  /* Cezanne APU */
#define PSM_GET_LEGACY      0x77  /* fallback */
#define PSM_GET_LEGACY_ALT  0x78  /* fallback */

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Signal Handling                                                           */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
    fprintf(stdout, "\033[?25h");
    fflush(stdout);
}

void smu_setup_signals(void)
{
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Utility Functions                                                         */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void append_u32_to_str(char *buf, unsigned int val)
{
    char tmp[8];
    snprintf(tmp, sizeof(tmp), "%c%c%c%c",
             val & 0xff, (val >> 8) & 0xff,
             (val >> 16) & 0xff, (val >> 24) & 0xff);
    strcat(buf, tmp);
}

static const char *get_processor_name(void)
{
    static char buffer[50] = {0};
    static int cached = 0;
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    char *p;
    size_t l;

    if (cached)
        return buffer;

    memset(buffer, 0, sizeof(buffer));

    __get_cpuid(0x80000002, &eax, &ebx, &ecx, &edx);
    append_u32_to_str(buffer, eax);
    append_u32_to_str(buffer, ebx);
    append_u32_to_str(buffer, ecx);
    append_u32_to_str(buffer, edx);

    __get_cpuid(0x80000003, &eax, &ebx, &ecx, &edx);
    append_u32_to_str(buffer, eax);
    append_u32_to_str(buffer, ebx);
    append_u32_to_str(buffer, ecx);
    append_u32_to_str(buffer, edx);

    __get_cpuid(0x80000004, &eax, &ebx, &ecx, &edx);
    append_u32_to_str(buffer, eax);
    append_u32_to_str(buffer, ebx);
    append_u32_to_str(buffer, ecx);
    append_u32_to_str(buffer, edx);

    p = buffer;
    l = strlen(p);
    while (l > 0 && isspace((unsigned char)p[l - 1]))
        p[--l] = 0;
    while (*p && isspace((unsigned char)*p))
        ++p;

    if (p != buffer)
        memmove(buffer, p, strlen(p) + 1);

    cached = 1;
    return buffer;
}

static unsigned int count_set_bits(unsigned int v)
{
    unsigned int c = 0;
    while (v) {
        c += v & 1;
        v >>= 1;
    }
    return c;
}

static int parse_hex(const char *str, uint32_t *out)
{
    char *end;
    unsigned long val;

    while (*str && isspace((unsigned char)*str))
        str++;

    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
        str += 2;

    val = strtoul(str, &end, 16);
    if (end == str || (*end && !isspace((unsigned char)*end)))
        return -1;

    *out = (uint32_t)val;
    return 0;
}

static void read_line(const char *prompt, char *buf, size_t len)
{
    fprintf(stdout, "%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)len, stdin) == NULL)
        buf[0] = '\0';
    buf[strcspn(buf, "\n\r")] = '\0';
}

static int kbhit(void)
{
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static void get_cpu_family_model(unsigned int *fam, unsigned int *model)
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    __get_cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    *fam = ((eax & 0xf00) >> 8) + ((eax & 0xff00000) >> 20);
    *model = ((eax & 0xf0000) >> 12) + ((eax & 0xf0) >> 4);
}

static int get_if_version_int(void)
{
    switch (obj.smu_if_version) {
    case IF_VERSION_9:  return 9;
    case IF_VERSION_10: return 10;
    case IF_VERSION_11: return 11;
    case IF_VERSION_12: return 12;
    case IF_VERSION_13: return 13;
    default:            return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Topology Detection (via SMN fuses, same as monitor_cpu.c)                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* Optional: pass non-NULL core_disable_map[2] to get per-CCD disable bitmap (1 = disabled). */
static int get_topology_ex(unsigned int *ccds, unsigned int *ccxs,
                           unsigned int *cores_per_ccx, unsigned int *phys_cores,
                           unsigned int core_disable_map[2])
{
    unsigned int fam, model, eax = 0, ebx = 0, ecx = 0, edx = 0;
    unsigned int ccds_present, ccds_down, ccds_enabled, ccds_disabled;
    unsigned int core_fuse, core_fuse_addr, core_disable, smt;
    unsigned int logical_cores;
    unsigned int ccd_fuse1, ccd_fuse2;

    __get_cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    fam = ((eax & 0xf00) >> 8) + ((eax & 0xff00000) >> 20);
    model = ((eax & 0xf0000) >> 12) + ((eax & 0xf0) >> 4);
    logical_cores = (ebx >> 16) & 0xFF;

    ccd_fuse1 = 0x5D218;
    ccd_fuse2 = 0x5D21C;

    if (fam == 0x17 && model != 0x71) {
        ccd_fuse1 += 0x40;
        ccd_fuse2 += 0x40;
    }

    if (smu_read_smn_addr(&obj, ccd_fuse1, &ccds_present) != SMU_Return_OK ||
        smu_read_smn_addr(&obj, ccd_fuse2, &ccds_down) != SMU_Return_OK)
        return -1;

    ccds_disabled = ((ccds_down & 0x3F) << 2) | ((ccds_present >> 30) & 0x3);
    ccds_present = (ccds_present >> 22) & 0xFF;
    ccds_enabled = ccds_present;

    if (fam == 0x19)
        core_fuse_addr = (0x30081800 + 0x598) |
            ((((ccds_disabled & ccds_present) & 1) == 1) ? 0x2000000 : 0);
    else
        core_fuse_addr = (0x30081800 + 0x238) |
            (((ccds_present & 1) == 0) ? 0x2000000 : 0);

    if (smu_read_smn_addr(&obj, core_fuse_addr, &core_fuse) != SMU_Return_OK)
        return -1;

    core_disable = core_fuse & 0xFF;
    smt = (core_fuse & (1 << 8)) != 0;

    if (core_disable_map) {
        core_disable_map[0] = core_disable & 0xFF;
        core_disable_map[1] = (core_fuse >> 8) & 0xFF;
        if (core_disable_map[1] == 0)
            core_disable_map[1] = core_disable_map[0]; /* single CCD */
    }

    *ccds = count_set_bits(ccds_enabled);

    if (fam == 0x19) {
        *ccxs = *ccds;
        *cores_per_ccx = 8 - count_set_bits(core_disable);
    } else {
        *ccxs = *ccds * 2;
        *cores_per_ccx = (8 - count_set_bits(core_disable)) / 2;
    }

    *phys_cores = logical_cores;
    if (smt)
        *phys_cores /= 2;

    return 0;
}

static int get_topology(unsigned int *ccds, unsigned int *ccxs,
                        unsigned int *cores_per_ccx, unsigned int *phys_cores)
{
    return get_topology_ex(ccds, ccxs, cores_per_ccx, phys_cores, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Shared API for GUI (see smu_common.h)                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

smu_obj_t *smu_get_obj(void) { return &obj; }
const char *smu_get_processor_name(void) { return get_processor_name(); }
void smu_get_cpu_family_model(unsigned int *fam, unsigned int *model) {
    get_cpu_family_model(fam, model);
}
int smu_get_topology(unsigned int *ccds, unsigned int *ccxs,
                     unsigned int *cores_per_ccx, unsigned int *phys_cores) {
    return get_topology(ccds, ccxs, cores_per_ccx, phys_cores);
}

int smu_get_core_enabled(int core_index) {
    unsigned int ccds, ccxs, cpc, phys;
    unsigned int core_disable_map[2] = {0, 0};
    if (get_topology_ex(&ccds, &ccxs, &cpc, &phys, core_disable_map) != 0)
        return 1;
    if (core_index < 0 || (unsigned)core_index >= phys)
        return 0;
    int map_index = core_index / 8;
    int core_in_group = core_index % 8;
    return (int)((~core_disable_map[map_index] >> core_in_group) & 1);
}

int smu_get_if_version_int(void) { return get_if_version_int(); }

unsigned int smu_encode_core_mask(int core_index) {
    /* APU: simple core index; Desktop: (ccd << 8 | local_core) << 20 */
    if (obj.codename == CODENAME_RENOIR || obj.codename == CODENAME_CEZANNE ||
        obj.codename == CODENAME_REMBRANDT || obj.codename == CODENAME_PHOENIX ||
        obj.codename == CODENAME_RAVENRIDGE || obj.codename == CODENAME_PICASSO ||
        obj.codename == CODENAME_LUCIENNE || obj.codename == CODENAME_DALI)
        return (unsigned int)core_index;
    int ccd = core_index / 8;
    int local = core_index % 8;
    return (unsigned int)((ccd << 8 | local) << 20);
}

int smu_get_fmax(unsigned int *mhz_out) {
    smu_arg_t args;
    memset(&args, 0, sizeof(args));
    if (smu_send_command(&obj, SMU_CMD_GET_MAX_FREQUENCY, &args, SMU_TYPE_RSMU) != SMU_Return_OK)
        return -1;
    *mhz_out = args.args[0];
    return 0;
}

int smu_set_fmax(unsigned int mhz) {
    smu_arg_t args;
    memset(&args, 0, sizeof(args));
    args.args[0] = mhz;
    return smu_send_command(&obj, SMU_CMD_SET_FMAX_ALL_CORES, &args, SMU_TYPE_RSMU) == SMU_Return_OK ? 0 : -1;
}

/* True if this codename uses Zen4/Zen5 PSM format: SET 0x6 with single combined arg. */
static int use_zen45_psm_set(void) {
    switch (obj.codename) {
    case CODENAME_RAPHAEL:
    case CODENAME_GRANITERIDGE:
    case CODENAME_STORMPEAK:
    case CODENAME_STRIXPOINT:
    case CODENAME_STRIXHALO:
    case CODENAME_HAWKPOINT:
    case CODENAME_REMBRANDT:
        return 1;
    default:
        return 0;
    }
}

int smu_set_curve_optimizer(int core_index, int margin) {
    smu_arg_t args;
    unsigned int mask = smu_encode_core_mask(core_index);
    memset(&args, 0, sizeof(args));
    if (use_zen45_psm_set()) {
        /* Zen4/Zen5: single arg (coreMask & 0xfff00000) | margin (low 16 bits, signed). */
        args.args[0] = (mask & 0xfff00000u) | ((unsigned int)(int16_t)margin & 0xFFFFu);
        return smu_send_command(&obj, PSM_SET_ZEN4_ZEN5, &args, SMU_TYPE_RSMU) == SMU_Return_OK ? 0 : -1;
    }
    args.args[0] = mask;
    args.args[1] = (unsigned int)(int)margin;
    return smu_send_command(&obj, SMU_CMD_SET_PSM_MARGIN, &args, SMU_TYPE_RSMU) == SMU_Return_OK ? 0 : -1;
}

/* Return preferred RSMU GetDldoPsmMargin command ID for current codename (ZenStates-Core mapping). 0 = use fallback list. */
static unsigned int get_psm_get_cmd_for_codename(void) {
    switch (obj.codename) {
    case CODENAME_CASTLEPEAK:
    case CODENAME_MATISSE:
    case CODENAME_VERMEER:
    case CODENAME_MILAN:
    case CODENAME_CHAGALL:
        return PSM_GET_ZEN3;      /* 0x7C */
    case CODENAME_RAPHAEL:
    case CODENAME_GRANITERIDGE:
    case CODENAME_STORMPEAK:
    case CODENAME_STRIXPOINT:
    case CODENAME_STRIXHALO:
    case CODENAME_HAWKPOINT:
    case CODENAME_REMBRANDT:
        return PSM_GET_ZEN4_ZEN5; /* 0xD5 */
    case CODENAME_PHOENIX:
        return PSM_GET_PHOENIX;   /* 0xE1 */
    case CODENAME_CEZANNE:
        return PSM_GET_CEZANNE;   /* 0xC3 */
    default:
        return 0;
    }
}

/* Try one GET PSM command. Returns: 1 = OK + non-zero margin, 0 = OK + zero, -1 = failed. */
/* Try one GET PSM command. Returns: 1 = OK + non-zero margin, 0 = OK + zero, -1 = failed/OOB. */
static int try_get_psm(unsigned int cmd, unsigned int arg0, int *margin_out) {
    smu_arg_t args;
    memset(&args, 0, sizeof(args));
    args.args[0] = arg0;
    if (smu_send_command(&obj, cmd, &args, SMU_TYPE_RSMU) != SMU_Return_OK)
        return -1;
    /* ZenStates reads margin from args[0] as signed int32 (Cpu.cs: (int)result.args[0]). */
    int val = (int)args.args[0];
    if (val >= CO_MARGIN_MIN && val <= CO_MARGIN_MAX) {
        *margin_out = val;
        return (val != 0) ? 1 : 0;
    }
    /* Zen4/Zen5 combined format: margin in low 16 bits of args[0]. */
    val = (int)(int16_t)(args.args[0] & 0xFFFF);
    if (val >= CO_MARGIN_MIN && val <= CO_MARGIN_MAX) {
        *margin_out = val;
        return (val != 0) ? 1 : 0;
    }
    return -1;
}

int smu_get_curve_optimizer(int core_index, int *margin_out) {
    unsigned int mask = smu_encode_core_mask(core_index);
    unsigned int preferred = get_psm_get_cmd_for_codename();

    /* Arg0 variants: encoded mask, 0-based core index. */
    unsigned int arg0_v[2] = { mask, (unsigned int)core_index };
    int got_zero = 0;

    /*
     * When platform is known, ONLY use that command — trying other command IDs
     * can return stale/unrelated OK+0 or garbage that passes range-check,
     * hiding the real value from the correct command on a later arg variant.
     */
    if (preferred != 0) {
        for (int pass = 0; pass < 2; pass++) {
            int rc = try_get_psm(preferred, arg0_v[pass], margin_out);
            if (rc > 0) return 0;
            if (rc == 0) got_zero = 1;
        }
        if (got_zero) { *margin_out = 0; return 0; }
        return -1;
    }

    /* Unknown platform: try all known command IDs with both arg formats. */
    static const unsigned int all_cmds[] = {
        PSM_GET_ZEN4_ZEN5, PSM_GET_ZEN3, PSM_GET_ZEN5_SP,
        PSM_GET_PHOENIX, PSM_GET_CEZANNE, PSM_GET_LEGACY, PSM_GET_LEGACY_ALT
    };
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sizeof(all_cmds) / sizeof(all_cmds[0]); i++) {
            int rc = try_get_psm(all_cmds[i], arg0_v[pass], margin_out);
            if (rc > 0) return 0;
            if (rc == 0) got_zero = 1;
        }
    }
    if (got_zero) { *margin_out = 0; return 0; }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Raw SMU Command (for mailbox scanning with arbitrary addresses)           */
/* ═══════════════════════════════════════════════════════════════════════════ */

static int raw_smu_cmd(uint32_t msg_addr, uint32_t rsp_addr, uint32_t arg_addr,
                       uint32_t cmd, uint32_t *args, int nargs)
{
    uint32_t val;
    int attempts;

    /* Wait for mailbox ready (RSP != 0) */
    for (attempts = 0; attempts < SMU_SCAN_RETRIES; attempts++) {
        if (smu_read_smn_addr(&obj, rsp_addr, &val) != SMU_Return_OK)
            return -1;
        if (val != 0)
            break;
        usleep(100);
    }
    if (attempts >= SMU_SCAN_RETRIES)
        return 0xFB;

    /* Clear response register */
    smu_write_smn_addr(&obj, rsp_addr, 0);

    /* Write arguments */
    if (arg_addr != 0xFFFFFFFF && args) {
        for (int i = 0; i < nargs && i < 6; i++)
            smu_write_smn_addr(&obj, arg_addr + (uint32_t)(i * 4), args[i]);
    }

    /* Write command to trigger execution */
    smu_write_smn_addr(&obj, msg_addr, cmd);

    /* Poll for response */
    for (attempts = 0; attempts < SMU_SCAN_RETRIES; attempts++) {
        if (smu_read_smn_addr(&obj, rsp_addr, &val) != SMU_Return_OK)
            return -1;
        if (val != 0)
            break;
        usleep(100);
    }
    if (attempts >= SMU_SCAN_RETRIES)
        return 0xFB;

    /* Read back args on success */
    if (val == 0x01 && arg_addr != 0xFFFFFFFF && args) {
        for (int i = 0; i < nargs && i < 6; i++)
            smu_read_smn_addr(&obj, arg_addr + (uint32_t)(i * 4), &args[i]);
    }

    return (int)val;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [1] System Information                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void show_system_info(void)
{
    unsigned int ccds = 0, ccxs = 0, cores_per_ccx = 0, phys_cores = 0;
    unsigned int fam, model;
    const char *name, *codename, *fw_ver;

    name     = get_processor_name();
    codename = smu_codename_to_str(&obj);
    fw_ver   = smu_get_fw_version(&obj);
    get_cpu_family_model(&fam, &model);

    printf("\n");
    printf("╭──────────────────────────────────────────────────────────────────╮\n");
    printf("│                      System Information                          │\n");
    printf("├──────────────────────────────────────────────────────────────────┤\n");
    printf("│ %-22s │ %-39s │\n", "CPU Model",         name);
    printf("│ %-22s │ %-39s │\n", "Codename",          codename);
    printf("│ %-22s │ 0x%-37X │\n", "Family",           fam);
    printf("│ %-22s │ 0x%-37X │\n", "Model",            model);
    printf("│ %-22s │ v%-38s │\n", "SMU FW Version",   fw_ver);
    printf("│ %-22s │ v%-38d │\n", "MP1 IF Version",   get_if_version_int());
    printf("│ %-22s │ 0x%-37X │\n", "SMU Version (raw)", obj.smu_version);

    if (smu_pm_tables_supported(&obj)) {
        printf("│ %-22s │ 0x%-37X │\n", "PM Table Version", obj.pm_table_version);
        printf("│ %-22s │ %-3u bytes (%-5u floats)              │\n",
               "PM Table Size", obj.pm_table_size, obj.pm_table_size / 4);
    } else {
        printf("│ %-22s │ %-39s │\n", "PM Table", "Not supported");
    }

    if (get_topology(&ccds, &ccxs, &cores_per_ccx, &phys_cores) == 0) {
        printf("│ %-22s │ %u CCD / %u CCX / %u cores per CCX     │\n",
               "Topology", ccds, ccxs, cores_per_ccx);
        printf("│ %-22s │ %-39u │\n", "Physical Cores", phys_cores);
    }

    printf("╰──────────────────────────────────────────────────────────────────╯\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [2] Send SMU Command                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void send_smu_command_interactive(void)
{
    char buf[256];
    uint32_t cmd;
    int mailbox_type;
    smu_arg_t args;
    smu_return_val ret;

    printf("\n--- Send SMU Command ---\n");
    printf("  Mailbox: [1] RSMU  [2] MP1  [3] HSMP\n");
    read_line("  Select mailbox: ", buf, sizeof(buf));
    mailbox_type = atoi(buf);

    if (mailbox_type < 1 || mailbox_type > 3) {
        fprintf(stderr, "  Invalid mailbox selection.\n");
        return;
    }

    read_line("  Command (hex): ", buf, sizeof(buf));
    if (parse_hex(buf, &cmd) != 0) {
        fprintf(stderr, "  Invalid hex value.\n");
        return;
    }

    memset(&args, 0, sizeof(args));

    read_line("  Arg0 (hex, enter for 0): ", buf, sizeof(buf));
    if (buf[0])
        parse_hex(buf, &args.args[0]);

    read_line("  Arg1 (hex, enter for 0): ", buf, sizeof(buf));
    if (buf[0])
        parse_hex(buf, &args.args[1]);

    read_line("  Arg2 (hex, enter for 0): ", buf, sizeof(buf));
    if (buf[0])
        parse_hex(buf, &args.args[2]);

    read_line("  Arg3 (hex, enter for 0): ", buf, sizeof(buf));
    if (buf[0])
        parse_hex(buf, &args.args[3]);

    read_line("  Arg4 (hex, enter for 0): ", buf, sizeof(buf));
    if (buf[0])
        parse_hex(buf, &args.args[4]);

    read_line("  Arg5 (hex, enter for 0): ", buf, sizeof(buf));
    if (buf[0])
        parse_hex(buf, &args.args[5]);

    enum smu_mailbox mb;
    switch (mailbox_type) {
    case 1:  mb = SMU_TYPE_RSMU; break;
    case 2:  mb = SMU_TYPE_MP1;  break;
    case 3:  mb = SMU_TYPE_HSMP; break;
    default: return;
    }

    printf("\n  Sending CMD 0x%02X to %s ...\n", cmd,
           mailbox_type == 1 ? "RSMU" : mailbox_type == 2 ? "MP1" : "HSMP");

    ret = smu_send_command(&obj, cmd, &args, mb);

    printf("  Status: 0x%02X (%s)\n", ret, smu_return_to_str(ret));

    if (ret == SMU_Return_OK) {
        printf("  Response Args:\n");
        for (int i = 0; i < 6; i++) {
            printf("    Arg%d: 0x%08X  (dec: %u, float: %f)\n",
                   i, args.args[i], args.args[i], args.args_f[i]);
        }
    }

    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [3] PM Table Monitor (live, with max tracking)                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void pm_table_monitor(void)
{
    char buf[64];
    int interval_ms = 2000;
    unsigned int num_entries, page_size, page, total_pages, start_idx;
    unsigned char *pm_buf;
    float *table, *max_values;
    int first_read = 1;
    struct termios oldt, newt;

    if (!smu_pm_tables_supported(&obj)) {
        fprintf(stderr, "  PM Tables not supported on this platform.\n");
        return;
    }

    num_entries = obj.pm_table_size / sizeof(float);

    read_line("\n  Update interval (ms, default 2000): ", buf, sizeof(buf));
    if (buf[0])
        interval_ms = atoi(buf);
    if (interval_ms < 100)
        interval_ms = 100;

    read_line("  Entries per page (0=all, default 50): ", buf, sizeof(buf));
    page_size = buf[0] ? (unsigned)atoi(buf) : 50;
    if (page_size == 0 || page_size > num_entries)
        page_size = num_entries;

    total_pages = (num_entries + page_size - 1) / page_size;
    page = 0;

    pm_buf = calloc(obj.pm_table_size, 1);
    max_values = calloc(num_entries, sizeof(float));
    if (!pm_buf || !max_values) {
        fprintf(stderr, "  Memory allocation failed.\n");
        free(pm_buf);
        free(max_values);
        return;
    }

    for (unsigned i = 0; i < num_entries; i++)
        max_values[i] = -FLT_MAX;

    /* Set terminal to raw for single-keypress detection */
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    printf("\n  PM Table Monitor: [q] quit  [n] next page  [p] prev page  [r] reset max\n");
    printf("  Showing %u entries (%u pages), refreshing every %d ms\n\n",
           num_entries, total_pages, interval_ms);

    g_running = 1;

    while (g_running) {
        if (smu_read_pm_table(&obj, pm_buf, obj.pm_table_size) != SMU_Return_OK) {
            usleep((unsigned)(interval_ms * 1000));
            continue;
        }

        table = (float *)pm_buf;

        /* Update max values */
        for (unsigned i = 0; i < num_entries; i++) {
            if (first_read || table[i] > max_values[i])
                max_values[i] = table[i];
        }
        first_read = 0;

        /* Clear screen and draw table */
        fprintf(stdout, "\033[1;1H\033[2J");
        fprintf(stdout, "Ryzen SMU Debug - PM Table Monitor  |  "
                "Page %u/%u  |  PM Version: 0x%06X  |  %u entries  |  [q]uit [n]ext [p]rev [r]eset\n",
                page + 1, total_pages, obj.pm_table_version, num_entries);
        fprintf(stdout, "──────┬──────────┬────────────────┬────────────────\n");
        fprintf(stdout, " Idx  │  Offset  │     Value      │      Max\n");
        fprintf(stdout, "──────┼──────────┼────────────────┼────────────────\n");

        start_idx = page * page_size;
        for (unsigned i = start_idx; i < start_idx + page_size && i < num_entries; i++) {
            fprintf(stdout, " %04u │ 0x%04X   │ %14.6f │ %14.6f\n",
                    i, i * 4, table[i], max_values[i]);
        }

        fprintf(stdout, "──────┴──────────┴────────────────┴────────────────\n");
        fprintf(stdout, "\033[?25l");
        fflush(stdout);

        /* Wait for interval, checking for keypresses */
        for (int elapsed = 0; elapsed < interval_ms && g_running; elapsed += 50) {
            if (kbhit()) {
                char c = (char)getchar();
                if (c == 'q' || c == 'Q') {
                    g_running = 0;
                    break;
                } else if (c == 'n' || c == 'N') {
                    if (page < total_pages - 1)
                        page++;
                    break;
                } else if (c == 'p' || c == 'P') {
                    if (page > 0)
                        page--;
                    break;
                } else if (c == 'r' || c == 'R') {
                    for (unsigned j = 0; j < num_entries; j++)
                        max_values[j] = table[j];
                    break;
                }
            }
            usleep(50000);
        }
    }

    fprintf(stdout, "\033[?25h");
    fflush(stdout);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    g_running = 1;

    free(pm_buf);
    free(max_values);

    printf("\n  Monitor stopped.\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [4] PM Table Dump / Export                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void pm_table_dump(void)
{
    char buf[256];
    unsigned char *pm_buf;
    float *table;
    unsigned int num_entries;
    FILE *fp = NULL;

    if (!smu_pm_tables_supported(&obj)) {
        fprintf(stderr, "  PM Tables not supported on this platform.\n");
        return;
    }

    num_entries = obj.pm_table_size / sizeof(float);

    read_line("\n  Output file (enter for stdout, or filename): ", buf, sizeof(buf));
    if (buf[0]) {
        fp = fopen(buf, "w");
        if (!fp) {
            perror("  Failed to open file");
            return;
        }
    }

    FILE *out = fp ? fp : stdout;

    pm_buf = calloc(obj.pm_table_size, 1);
    if (!pm_buf) {
        fprintf(stderr, "  Memory allocation failed.\n");
        if (fp) fclose(fp);
        return;
    }

    if (smu_read_pm_table(&obj, pm_buf, obj.pm_table_size) != SMU_Return_OK) {
        fprintf(stderr, "  Failed to read PM table.\n");
        free(pm_buf);
        if (fp) fclose(fp);
        return;
    }

    table = (float *)pm_buf;

    read_line("  Format: [1] Table  [2] CSV  [3] Raw binary: ", buf, sizeof(buf));
    int fmt = buf[0] ? atoi(buf) : 1;

    switch (fmt) {
    case 2: /* CSV */
        fprintf(out, "Index,Offset,Value\n");
        for (unsigned i = 0; i < num_entries; i++)
            fprintf(out, "%u,0x%04X,%.6f\n", i, i * 4, table[i]);
        break;
    case 3: /* Raw binary */
        if (fp) {
            fclose(fp);
            fp = fopen(buf[0] ? buf : "pm_table.bin", "wb");
            if (fp) {
                fwrite(pm_buf, 1, obj.pm_table_size, fp);
                printf("  Written %u bytes of raw PM table data.\n", obj.pm_table_size);
            }
        } else {
            fprintf(stderr, "  Raw binary requires a file path.\n");
        }
        break;
    default: /* Table */
        fprintf(out, "\nPM Table Dump - Version 0x%06X - %u entries (%u bytes)\n",
                obj.pm_table_version, num_entries, obj.pm_table_size);
        fprintf(out, "──────┬──────────┬────────────────\n");
        fprintf(out, " Idx  │  Offset  │     Value\n");
        fprintf(out, "──────┼──────────┼────────────────\n");
        for (unsigned i = 0; i < num_entries; i++)
            fprintf(out, " %04u │ 0x%04X   │ %14.6f\n", i, i * 4, table[i]);
        fprintf(out, "──────┴──────────┴────────────────\n");
        break;
    }

    free(pm_buf);
    if (fp) {
        fclose(fp);
        printf("  Exported to file.\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [5] SMN Read                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void smn_read_interactive(void)
{
    char buf[64];
    unsigned int addr, value;

    printf("\n--- SMN Read ---\n");
    read_line("  Address (hex): ", buf, sizeof(buf));
    if (parse_hex(buf, &addr) != 0) {
        fprintf(stderr, "  Invalid address.\n");
        return;
    }

    if (smu_read_smn_addr(&obj, addr, &value) != SMU_Return_OK) {
        fprintf(stderr, "  Failed to read SMN address 0x%08X.\n", addr);
        return;
    }

    printf("  Address: 0x%08X\n", addr);
    printf("  HEX:     0x%08X\n", value);
    printf("  DEC:     %u\n", value);
    printf("  BIN:     ");
    for (int i = 31; i >= 0; i--) {
        printf("%d", (value >> i) & 1);
        if (i % 8 == 0 && i > 0) printf(" ");
    }

    union { unsigned int u; float f; } pun;
    pun.u = value;
    printf("\n  FLOAT:   %f\n\n", pun.f);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [6] SMN Write                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void smn_write_interactive(void)
{
    char buf[64];
    unsigned int addr, value;

    printf("\n--- SMN Write ---\n");
    read_line("  Address (hex): ", buf, sizeof(buf));
    if (parse_hex(buf, &addr) != 0) {
        fprintf(stderr, "  Invalid address.\n");
        return;
    }

    read_line("  Value (hex): ", buf, sizeof(buf));
    if (parse_hex(buf, &value) != 0) {
        fprintf(stderr, "  Invalid value.\n");
        return;
    }

    if (smu_write_smn_addr(&obj, addr, value) != SMU_Return_OK) {
        fprintf(stderr, "  Failed to write to SMN address 0x%08X.\n", addr);
        return;
    }

    printf("  Written 0x%08X to address 0x%08X.\n\n", value, addr);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [7] SMN Range Scan                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void smn_range_scan(void)
{
    char buf[64];
    unsigned int start_addr, end_addr, value;
    FILE *fp = NULL;

    printf("\n--- SMN Range Scan ---\n");
    read_line("  Start address (hex): ", buf, sizeof(buf));
    if (parse_hex(buf, &start_addr) != 0) {
        fprintf(stderr, "  Invalid address.\n");
        return;
    }

    read_line("  End address (hex): ", buf, sizeof(buf));
    if (parse_hex(buf, &end_addr) != 0) {
        fprintf(stderr, "  Invalid address.\n");
        return;
    }

    if (end_addr <= start_addr) {
        fprintf(stderr, "  End address must be greater than start.\n");
        return;
    }

    read_line("  Output file (enter for stdout): ", buf, sizeof(buf));
    if (buf[0]) {
        fp = fopen(buf, "w");
        if (!fp) {
            perror("  Failed to open file");
            return;
        }
    }

    FILE *out = fp ? fp : stdout;

    fprintf(out, "\nSMN Range Scan: 0x%08X - 0x%08X\n", start_addr, end_addr);
    fprintf(out, "──────────────┬────────────┬────────────────────────────────────\n");
    fprintf(out, "  Address     │   Value    │   Binary\n");
    fprintf(out, "──────────────┼────────────┼────────────────────────────────────\n");

    for (unsigned int addr = start_addr; addr <= end_addr; addr += 4) {
        if (smu_read_smn_addr(&obj, addr, &value) != SMU_Return_OK) {
            fprintf(out, "  0x%08X  │ READ ERROR │\n", addr);
            continue;
        }

        fprintf(out, "  0x%08X  │ 0x%08X │ ", addr, value);
        for (int i = 31; i >= 0; i--) {
            fprintf(out, "%d", (value >> i) & 1);
            if (i % 8 == 0 && i > 0) fprintf(out, " ");
        }
        fprintf(out, "\n");
    }

    fprintf(out, "──────────────┴────────────┴────────────────────────────────────\n");

    if (fp) {
        fclose(fp);
        printf("  Scan exported to file.\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [8] SMU Mailbox Scan                                                      */
/*  Replicates ScanSmuRange from the Windows SettingsForm.cs                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t msg;
    uint32_t rsp;
} msg_rsp_pair_t;

static void scan_smu_range(uint32_t start, uint32_t end, uint32_t step,
                           uint32_t rsp_offset)
{
    uint32_t rsp_val, addr;
    msg_rsp_pair_t pairs[64];
    int pair_count = 0;

    printf("  Scanning 0x%08X - 0x%08X (step=%u, offset=0x%X) ...\n",
           start, end, step, rsp_offset);

    /* Phase 1: Discover CMD-RSP pairs */
    for (addr = start; addr <= end && pair_count < 64; addr += step) {
        uint32_t reg_val;
        if (smu_read_smn_addr(&obj, addr, &reg_val) != SMU_Return_OK)
            continue;
        if (reg_val == 0xFFFFFFFF)
            continue;

        /* Write unknown command 0xFF */
        smu_write_smn_addr(&obj, addr, 0xFF);
        usleep(10000);

        uint32_t rsp_addr = addr + rsp_offset;
        while (rsp_addr <= end) {
            if (smu_read_smn_addr(&obj, rsp_addr, &rsp_val) != SMU_Return_OK)
                break;

            if (rsp_val == 0xFE) {
                /* Got UnknownCmd - verify with GetSMUVersion (0x02) */
                smu_write_smn_addr(&obj, addr, 0x02);
                usleep(10000);

                if (smu_read_smn_addr(&obj, rsp_addr, &rsp_val) == SMU_Return_OK &&
                    rsp_val == 0x01) {
                    pairs[pair_count].msg = addr;
                    pairs[pair_count].rsp = rsp_addr;
                    pair_count++;
                    printf("    Found CMD/RSP pair: CMD=0x%08X RSP=0x%08X\n",
                           addr, rsp_addr);
                }
            }
            rsp_addr += step;
        }
    }

    if (pair_count == 0) {
        printf("    No mailbox pairs found in this range.\n");
        return;
    }

    /* Phase 2: Find ARG address for each pair */
    for (int p = 0; p < pair_count; p++) {
        uint32_t args[6] = {0};

        /* Send GetSMUVersion to populate the arg register with version */
        int ret = raw_smu_cmd(pairs[p].msg, pairs[p].rsp, 0xFFFFFFFF,
                              0x02, NULL, 0);
        if (ret != 0x01) {
            printf("    Pair CMD=0x%08X: GetSMUVersion failed (0x%02X)\n",
                   pairs[p].msg, ret);
            continue;
        }

        /* Scan for ARG address containing the SMU version */
        uint32_t arg_candidate = pairs[p].rsp + 4;
        uint32_t found_arg = 0;
        uint32_t scan_val;

        while (arg_candidate <= end) {
            if (smu_read_smn_addr(&obj, arg_candidate, &scan_val) == SMU_Return_OK &&
                scan_val == obj.smu_version) {
                /* Validate with TestMessage: send value, expect value+1 */
                int valid = 1;
                for (int attempt = 0; attempt < 3 && valid; attempt++) {
                    uint32_t test_val = 0xFAFAFAFA + (uint32_t)attempt;
                    args[0] = test_val;
                    memset(&args[1], 0, sizeof(uint32_t) * 5);

                    ret = raw_smu_cmd(pairs[p].msg, pairs[p].rsp,
                                      arg_candidate, 0x01, args, 6);
                    if (ret != 0x01) {
                        valid = 0;
                        break;
                    }

                    if (smu_read_smn_addr(&obj, arg_candidate, &scan_val) != SMU_Return_OK ||
                        scan_val != test_val + 1) {
                        valid = 0;
                    }
                }

                if (valid) {
                    found_arg = arg_candidate;
                    break;
                }
            }
            arg_candidate += step;
        }

        if (found_arg && g_match_count < MAX_MAILBOX_MATCHES) {
            g_matches[g_match_count].msg_addr = pairs[p].msg;
            g_matches[g_match_count].rsp_addr = pairs[p].rsp;
            g_matches[g_match_count].arg_addr = found_arg;
            g_match_count++;

            printf("    *** Validated Mailbox ***\n");
            printf("    CMD: 0x%08X\n", pairs[p].msg);
            printf("    RSP: 0x%08X\n", pairs[p].rsp);
            printf("    ARG: 0x%08X\n\n", found_arg);
        }
    }
}

static void smu_mailbox_scan(void)
{
    unsigned int fam, model;

    printf("\n--- SMU Mailbox Scan ---\n");
    printf("  WARNING: This may crash the system. Continue? [y/N]: ");
    fflush(stdout);

    char c = (char)getchar();
    while (getchar() != '\n');
    if (c != 'y' && c != 'Y') {
        printf("  Aborted.\n");
        return;
    }

    g_match_count = 0;
    get_cpu_family_model(&fam, &model);

    /*
     * Scan ranges from the Windows SMUDebugTool's SettingsForm.cs.
     * The ranges and offsets are CPU-family/codename dependent.
     */
    switch (obj.codename) {
    case CODENAME_RAVENRIDGE:
    case CODENAME_RAVENRIDGE2:
    case CODENAME_PICASSO:
    case CODENAME_DALI:
    case CODENAME_RENOIR:
    case CODENAME_LUCIENNE:
        scan_smu_range(0x03B10500, 0x03B10998, 8, 0x3C);
        scan_smu_range(0x03B10A00, 0x03B10AFF, 4, 0x60);
        break;
    case CODENAME_PINNACLERIDGE:
    case CODENAME_SUMMITRIDGE:
    case CODENAME_MATISSE:
    case CODENAME_COLFAX:
    case CODENAME_THREADRIPPER:
    case CODENAME_CASTLEPEAK:
    case CODENAME_VERMEER:
        scan_smu_range(0x03B10500, 0x03B10998, 8, 0x3C);
        scan_smu_range(0x03B10500, 0x03B10AFF, 4, 0x4C);
        break;
    case CODENAME_RAPHAEL:
    case CODENAME_GRANITERIDGE:
        scan_smu_range(0x03B10500, 0x03B10998, 8, 0x3C);
        break;
    default:
        printf("  No scan ranges defined for codename '%s'.\n",
               smu_codename_to_str(&obj));
        printf("  Trying generic ranges...\n");
        scan_smu_range(0x03B10500, 0x03B10998, 8, 0x3C);
        break;
    }

    printf("\n  Scan complete. Found %d validated mailbox(es).\n\n", g_match_count);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [9] Memory Timings (replicates monitor_cpu.c print_memory_timings)        */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void show_memory_timings(void)
{
    const char *bool_str[] = {"Disabled", "Enabled"};
    unsigned int v1, v2, offset;

    printf("\n--- Memory Timings (via SMN) ---\n\n");

    if (smu_read_smn_addr(&obj, 0x50200, &v1) != SMU_Return_OK)
        goto read_err;
    offset = (v1 == 0x300) ? 0x100000 : 0;

#define RD1(a) do { if (smu_read_smn_addr(&obj, (a)+offset, &v1) != SMU_Return_OK) goto read_err; } while(0)
#define RD2(a) do { if (smu_read_smn_addr(&obj, (a)+offset, &v2) != SMU_Return_OK) goto read_err; } while(0)

    RD1(0x50050); RD2(0x50058);
    printf("  BankGroupSwap:      %s\n",
           bool_str[!(v1 == v2 && v1 == 0x87654321)]);

    RD1(0x500D0); RD2(0x500D4);
    printf("  BankGroupSwapAlt:   %s\n",
           bool_str[((v1 >> 4) & 0x7F) != 0 || ((v2 >> 4) & 0x7F) != 0]);

    RD1(0x50200); RD2(0x50204);
    printf("  Memory Clock:       %.0f MHz\n", (v1 & 0x7f) / 3.f * 100.f);
    printf("  GDM:                %s\n", bool_str[((v1 >> 11) & 1) == 1]);
    printf("  CR:                 %s\n", ((v1 & 0x400) >> 10) != 0 ? "2T" : "1T");
    printf("  Tcl:                %u\n", v2 & 0x3f);
    printf("  Tras:               %u\n", (v2 >> 8) & 0x7f);
    printf("  Trcdrd:             %u\n", (v2 >> 16) & 0x3f);
    printf("  Trcdwr:             %u\n", (v2 >> 24) & 0x3f);

    RD1(0x50208); RD2(0x5020C);
    printf("  Trc:                %u\n", v1 & 0xff);
    printf("  Trp:                %u\n", (v1 >> 16) & 0x3f);
    printf("  Trrds:              %u\n", v2 & 0x1f);
    printf("  Trrdl:              %u\n", (v2 >> 8) & 0x1f);
    printf("  Trtp:               %u\n", (v2 >> 24) & 0x1f);

    RD1(0x50210); RD2(0x50214);
    printf("  Tfaw:               %u\n", v1 & 0xff);
    printf("  Tcwl:               %u\n", v2 & 0x3f);
    printf("  Twtrs:              %u\n", (v2 >> 8) & 0x1f);
    printf("  Twtrl:              %u\n", (v2 >> 16) & 0x3f);

    RD1(0x50218); RD2(0x50220);
    printf("  Twr:                %u\n", v1 & 0xff);
    printf("  Trdrddd:            %u\n", v2 & 0xf);
    printf("  Trdrdsd:            %u\n", (v2 >> 8) & 0xf);
    printf("  Trdrdsc:            %u\n", (v2 >> 16) & 0xf);
    printf("  Trdrdscl:           %u\n", (v2 >> 24) & 0x3f);

    RD1(0x50224); RD2(0x50228);
    printf("  Twrwrdd:            %u\n", v1 & 0xf);
    printf("  Twrwrsd:            %u\n", (v1 >> 8) & 0xf);
    printf("  Twrwrsc:            %u\n", (v1 >> 16) & 0xf);
    printf("  Twrwrscl:           %u\n", (v1 >> 24) & 0x3f);
    printf("  Twrrd:              %u\n", v2 & 0xf);
    printf("  Trdwr:              %u\n", (v2 >> 8) & 0x1f);

    RD1(0x50254);
    printf("  Tcke:               %u\n", (v1 >> 24) & 0x1f);

    RD1(0x50260); RD2(0x50264);
    if (v1 != v2 && v1 == 0x21060138)
        v1 = v2;
    printf("  Trfc:               %u\n", v1 & 0x3ff);
    printf("  Trfc2:              %u\n", (v1 >> 11) & 0x3ff);
    printf("  Trfc4:              %u\n", (v1 >> 22) & 0x3ff);

    printf("\n");
    return;

#undef RD1
#undef RD2

read_err:
    fprintf(stderr, "  Failed to read SMN address for memory timings.\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [A] Export JSON Report                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void export_json_report(void)
{
    char filename[256];
    unsigned int ccds = 0, ccxs = 0, cores_per_ccx = 0, phys_cores = 0;
    unsigned int fam, model;
    time_t now;
    FILE *fp;

    time(&now);
    snprintf(filename, sizeof(filename), "SMUDebug_%ld.json", (long)now);

    printf("\n--- Export JSON Report ---\n");
    printf("  Saving to: %s\n", filename);

    fp = fopen(filename, "w");
    if (!fp) {
        perror("  Failed to create file");
        return;
    }

    get_cpu_family_model(&fam, &model);
    get_topology(&ccds, &ccxs, &cores_per_ccx, &phys_cores);

    fprintf(fp, "{\n");
    fprintf(fp, "  \"ToolVersion\": \"%s\",\n", TOOL_VERSION);
    fprintf(fp, "  \"Timestamp\": %ld,\n", (long)now);
    fprintf(fp, "  \"CpuName\": \"%s\",\n", get_processor_name());
    fprintf(fp, "  \"Codename\": \"%s\",\n", smu_codename_to_str(&obj));
    fprintf(fp, "  \"Family\": \"0x%02X\",\n", fam);
    fprintf(fp, "  \"Model\": \"0x%02X\",\n", model);
    fprintf(fp, "  \"SmuVersion\": \"v%s\",\n", smu_get_fw_version(&obj));
    fprintf(fp, "  \"SmuVersionRaw\": \"0x%08X\",\n", obj.smu_version);
    fprintf(fp, "  \"Mp1IfVersion\": %d,\n", get_if_version_int());
    fprintf(fp, "  \"PmTableVersion\": \"0x%06X\",\n", obj.pm_table_version);
    fprintf(fp, "  \"PmTableSize\": %u,\n", obj.pm_table_size);
    fprintf(fp, "  \"Topology\": {\n");
    fprintf(fp, "    \"CCDs\": %u,\n", ccds);
    fprintf(fp, "    \"CCXs\": %u,\n", ccxs);
    fprintf(fp, "    \"CoresPerCCX\": %u,\n", cores_per_ccx);
    fprintf(fp, "    \"PhysicalCores\": %u\n", phys_cores);
    fprintf(fp, "  },\n");

    /* Discovered mailboxes */
    fprintf(fp, "  \"Mailboxes\": [\n");
    for (int i = 0; i < g_match_count; i++) {
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"MsgAddress\": \"0x%08X\",\n", g_matches[i].msg_addr);
        fprintf(fp, "      \"RspAddress\": \"0x%08X\",\n", g_matches[i].rsp_addr);
        fprintf(fp, "      \"ArgAddress\": \"0x%08X\"\n", g_matches[i].arg_addr);
        fprintf(fp, "    }%s\n", (i < g_match_count - 1) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    /* PM Table snapshot */
    if (smu_pm_tables_supported(&obj)) {
        unsigned char *pm_buf = calloc(obj.pm_table_size, 1);
        if (pm_buf && smu_read_pm_table(&obj, pm_buf, obj.pm_table_size) == SMU_Return_OK) {
            float *table = (float *)pm_buf;
            unsigned int num_entries = obj.pm_table_size / sizeof(float);

            fprintf(fp, "  \"PmTable\": [\n");
            for (unsigned i = 0; i < num_entries; i++) {
                fprintf(fp, "    { \"index\": %u, \"offset\": \"0x%04X\", \"value\": %.6f }%s\n",
                        i, i * 4, table[i], (i < num_entries - 1) ? "," : "");
            }
            fprintf(fp, "  ]\n");
        } else {
            fprintf(fp, "  \"PmTable\": null\n");
        }
        free(pm_buf);
    } else {
        fprintf(fp, "  \"PmTable\": null\n");
    }

    fprintf(fp, "}\n");
    fclose(fp);

    printf("  Report saved as %s\n\n", filename);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  [B] PM Table Named Fields (for known versions - from monitor_cpu.c)       */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* Matisse/CastlePeak PM table version 0x240903 structure */
typedef struct {
    float PPT_LIMIT, PPT_VALUE, TDC_LIMIT, TDC_VALUE;
    float THM_LIMIT, THM_VALUE, FIT_LIMIT, FIT_VALUE;
    float EDC_LIMIT, EDC_VALUE, VID_LIMIT, VID_VALUE;
    float PPT_WC, PPT_ACTUAL, TDC_WC, TDC_ACTUAL;
    float THM_WC, THM_ACTUAL, FIT_WC, FIT_ACTUAL;
    float EDC_WC, EDC_ACTUAL, VID_WC, VID_ACTUAL;
    float VDDCR_CPU_POWER, VDDCR_SOC_POWER;
    float VDDIO_MEM_POWER, VDD18_POWER, ROC_POWER, SOCKET_POWER;
    float PPT_FREQUENCY, TDC_FREQUENCY, THM_FREQUENCY;
    float PROCHOT_FREQUENCY, VOLTAGE_FREQUENCY, CCA_FREQUENCY;
    float FIT_VOLTAGE, FIT_PRE_VOLTAGE, LATCHUP_VOLTAGE;
    float CPU_SET_VOLTAGE, CPU_TELEMETRY_VOLTAGE;
    float CPU_TELEMETRY_CURRENT, CPU_TELEMETRY_POWER, CPU_TELEMETRY_POWER_ALT;
    float SOC_SET_VOLTAGE, SOC_TELEMETRY_VOLTAGE;
    float SOC_TELEMETRY_CURRENT, SOC_TELEMETRY_POWER;
    float FCLK_FREQ, FCLK_FREQ_EFF, UCLK_FREQ, MEMCLK_FREQ;
    float FCLK_DRAM_SETPOINT, FCLK_DRAM_BUSY;
    float FCLK_GMI_SETPOINT, FCLK_GMI_BUSY;
    float FCLK_IOHC_SETPOINT, FCLK_IOHC_BUSY;
    float FCLK_XGMI_SETPOINT, FCLK_XGMI_BUSY;
    float CCM_READS, CCM_WRITES, IOMS, XGMI;
    float CS_UMC_READS, CS_UMC_WRITES;
    float FCLK_RESIDENCY[4], FCLK_FREQ_TABLE[4];
    float UCLK_FREQ_TABLE[4], MEMCLK_FREQ_TABLE[4], FCLK_VOLTAGE[4];
    float LCLK_SETPOINT_0, LCLK_BUSY_0, LCLK_FREQ_0, LCLK_FREQ_EFF_0;
    float LCLK_MAX_DPM_0, LCLK_MIN_DPM_0;
    float LCLK_SETPOINT_1, LCLK_BUSY_1, LCLK_FREQ_1, LCLK_FREQ_EFF_1;
    float LCLK_MAX_DPM_1, LCLK_MIN_DPM_1;
    float LCLK_SETPOINT_2, LCLK_BUSY_2, LCLK_FREQ_2, LCLK_FREQ_EFF_2;
    float LCLK_MAX_DPM_2, LCLK_MIN_DPM_2;
    float LCLK_SETPOINT_3, LCLK_BUSY_3, LCLK_FREQ_3, LCLK_FREQ_EFF_3;
    float LCLK_MAX_DPM_3, LCLK_MIN_DPM_3;
    float XGMI_SETPOINT, XGMI_BUSY, XGMI_LANE_WIDTH, XGMI_DATA_RATE;
    float SOC_POWER, SOC_TEMP;
    float DDR_VDDP_POWER, DDR_VDDIO_MEM_POWER;
    float GMI2_VDDG_POWER, IO_VDDCR_SOC_POWER;
    float IOD_VDDIO_MEM_POWER, IO_VDD18_POWER;
    float TDP, DETERMINISM, V_VDDM, V_VDDP, V_VDDG;
    float PEAK_TEMP, PEAK_VOLTAGE, AVG_CORE_COUNT, CCLK_LIMIT;
    float MAX_VOLTAGE, DC_BTC, CSTATE_BOOST, PROCHOT, PC6, PWM;
    float SOCCLK, SHUBCLK, MP0CLK, MP1CLK, MP5CLK;
    float SMNCLK, TWIXCLK, WAFLCLK, DPM_BUSY, MP1_BUSY;
    float CORE_POWER[8], CORE_VOLTAGE[8], CORE_TEMP[8], CORE_FIT[8];
    float CORE_IDDMAX[8], CORE_FREQ[8], CORE_FREQEFF[8];
    float CORE_C0[8], CORE_CC1[8], CORE_CC6[8];
    float CORE_CKS_FDD[8], CORE_CI_FDD[8], CORE_IRM[8], CORE_PSTATE[8];
    float CORE_CPPC_MAX[8], CORE_CPPC_MIN[8];
    float CORE_SC_LIMIT[8], CORE_SC_CAC[8], CORE_SC_RESIDENCY[8];
    float L3_LOGIC_POWER[2], L3_VDDM_POWER[2], L3_TEMP[2], L3_FIT[2];
    float L3_IDDMAX[2], L3_FREQ[2], L3_CKS_FDD[2];
    float L3_CCA_THRESHOLD[2], L3_CCA_CAC[2], L3_CCA_ACTIVATION[2];
    float L3_EDC_LIMIT[2], L3_EDC_CAC[2], L3_EDC_RESIDENCY[2];
    float MP5_BUSY[1];
} pm_table_matisse_t;

static void show_named_pm_summary(void)
{
    unsigned char *pm_buf;
    unsigned int ccds = 0, ccxs = 0, cores_per_ccx = 0, phys_cores = 0;

    if (!smu_pm_tables_supported(&obj)) {
        fprintf(stderr, "  PM Tables not supported on this platform.\n");
        return;
    }

    /* Currently only Matisse 0x240903 has a named struct */
    if (obj.pm_table_version != 0x240903) {
        printf("\n  Named PM table fields not available for version 0x%06X.\n",
               obj.pm_table_version);
        printf("  Use the PM Table Dump/Monitor for raw index+offset view.\n\n");
        return;
    }

    pm_buf = calloc(obj.pm_table_size, 1);
    if (!pm_buf) {
        fprintf(stderr, "  Memory allocation failed.\n");
        return;
    }

    if (smu_read_pm_table(&obj, pm_buf, obj.pm_table_size) != SMU_Return_OK) {
        fprintf(stderr, "  Failed to read PM table.\n");
        free(pm_buf);
        return;
    }

    pm_table_matisse_t *pmt = (pm_table_matisse_t *)pm_buf;
    get_topology(&ccds, &ccxs, &cores_per_ccx, &phys_cores);

    float total_usage = 0, total_c6 = 0, peak_freq = 0;
    float total_core_voltage = 0;
    float package_sleep_time = pmt->PC6 / 100.f;
    float avg_voltage = (pmt->CPU_TELEMETRY_VOLTAGE - (0.2f * package_sleep_time)) /
                        (1.0f - package_sleep_time);

    printf("\n");
    printf("╭────────────────────────────────────────────────┬─────────────────────────────────╮\n");

    /* Per-core info */
    for (unsigned i = 0; i < phys_cores && i < 8; i++) {
        float core_freq = pmt->CORE_FREQEFF[i] * 1000.f;
        float core_sleep = pmt->CORE_CC6[i] / 100.f;
        float core_v = ((1.0f - core_sleep) * avg_voltage) + (0.2f * core_sleep);

        if (core_freq > peak_freq)
            peak_freq = core_freq;
        total_usage += pmt->CORE_C0[i];
        total_c6 += pmt->CORE_CC6[i];
        if (pmt->CORE_FREQ[i] != 0.f)
            total_core_voltage += core_v;

        if (pmt->CORE_C0[i] >= 6.f) {
            printf("│ Core %u: %4.0f MHz │ %5.3f W │ %1.3f V │ %5.1f C │ C0:%5.1f%% C1:%5.1f%% C6:%5.1f%% │\n",
                   i, core_freq, pmt->CORE_POWER[i], core_v, pmt->CORE_TEMP[i],
                   pmt->CORE_C0[i], pmt->CORE_CC1[i], pmt->CORE_CC6[i]);
        } else {
            printf("│ Core %u: Sleeping │ %5.3f W │ %1.3f V │ %5.1f C │ C0:%5.1f%% C1:%5.1f%% C6:%5.1f%% │\n",
                   i, pmt->CORE_POWER[i], core_v, pmt->CORE_TEMP[i],
                   pmt->CORE_C0[i], pmt->CORE_CC1[i], pmt->CORE_CC6[i]);
        }
    }

    printf("├────────────────────────────────────────────────┼─────────────────────────────────┤\n");

    float edc_value = pmt->EDC_VALUE * (total_usage / (float)phys_cores / 100.f);
    if (edc_value < pmt->TDC_VALUE)
        edc_value = pmt->TDC_VALUE;
    total_c6 /= (float)phys_cores;
    float avg_core_v = total_core_voltage / (float)phys_cores;

    printf("│ %-22s │ %8.0f MHz                     │\n", "Peak Core Freq",    peak_freq);
    printf("│ %-22s │ %8.2f C                       │\n", "Peak Temperature",  pmt->PEAK_TEMP);
    printf("│ %-22s │ %8.4f W                       │\n", "Package Power",     pmt->SOCKET_POWER);
    printf("│ %-22s │ %8.6f V                       │\n", "Peak Core(s) Volt", pmt->CPU_TELEMETRY_VOLTAGE);
    printf("│ %-22s │ %8.6f V                       │\n", "Average Core Volt", avg_core_v);
    printf("│ %-22s │ %8.6f %%                       │\n", "Package C6",        pmt->PC6);
    printf("│ %-22s │ %8.6f %%                       │\n", "Core C6 Avg",       total_c6);
    printf("├────────────────────────────────────────────────┼─────────────────────────────────┤\n");
    printf("│ %-22s │ %8.2f C                       │\n", "Tjunction Limit",   pmt->THM_LIMIT);
    printf("│ %-22s │ %8.2f C                       │\n", "Current Temp",      pmt->THM_VALUE);
    printf("│ %-22s │ %8.2f C                       │\n", "SoC Temperature",   pmt->SOC_TEMP);
    printf("│ %-22s │ %8.4f W                       │\n", "Core Power",        pmt->VDDCR_CPU_POWER);
    printf("│ %-22s │ %8.4f W │ %6.4f A │ %6.4f V  │\n", "SoC Power",
           pmt->SOC_TELEMETRY_POWER, pmt->SOC_TELEMETRY_CURRENT, pmt->SOC_TELEMETRY_VOLTAGE);
    printf("│ %-22s │ %7.2f W / %5.0f W (%5.1f%%)    │\n", "PPT",
           pmt->PPT_VALUE, pmt->PPT_LIMIT, pmt->PPT_VALUE / pmt->PPT_LIMIT * 100.f);
    printf("│ %-22s │ %7.2f A / %5.0f A (%5.1f%%)    │\n", "TDC",
           pmt->TDC_VALUE, pmt->TDC_LIMIT, pmt->TDC_VALUE / pmt->TDC_LIMIT * 100.f);
    printf("│ %-22s │ %7.2f A / %5.0f A (%5.1f%%)    │\n", "EDC",
           edc_value, pmt->EDC_LIMIT, edc_value / pmt->EDC_LIMIT * 100.f);
    printf("│ %-22s │ %8.0f MHz                     │\n", "Frequency Limit",   pmt->CCLK_LIMIT * 1000.f);
    printf("├────────────────────────────────────────────────┼─────────────────────────────────┤\n");
    printf("│ %-22s │ %8s                         │\n", "Coupled Mode",
           pmt->UCLK_FREQ == pmt->MEMCLK_FREQ ? "ON" : "OFF");
    printf("│ %-22s │ %5.0f MHz                        │\n", "Fabric Clock (Avg)",pmt->FCLK_FREQ_EFF);
    printf("│ %-22s │ %5.0f MHz                        │\n", "Fabric Clock",      pmt->FCLK_FREQ);
    printf("│ %-22s │ %5.0f MHz                        │\n", "Uncore Clock",      pmt->UCLK_FREQ);
    printf("│ %-22s │ %5.0f MHz                        │\n", "Memory Clock",      pmt->MEMCLK_FREQ);
    printf("│ %-22s │ %8.3f GiB/s                   │\n", "DRAM Read BW",      pmt->CS_UMC_READS);
    printf("│ %-22s │ %8.3f GiB/s                   │\n", "DRAM Write BW",     pmt->CS_UMC_WRITES);
    printf("│ %-22s │ %8.4f V                       │\n", "VDDCR_SoC",         pmt->SOC_SET_VOLTAGE);
    printf("│ %-22s │ %8.4f V                       │\n", "cLDO_VDDM",         pmt->V_VDDM);
    printf("│ %-22s │ %8.4f V                       │\n", "cLDO_VDDP",         pmt->V_VDDP);
    printf("│ %-22s │ %8.4f V                       │\n", "cLDO_VDDG",         pmt->V_VDDG);
    printf("╰────────────────────────────────────────────────┴─────────────────────────────────╯\n\n");

    free(pm_buf);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Privilege Elevation                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

int smu_elevate_if_necessary(int argc, char **argv)
{
    static const char *paths[] = {"/bin", "/sbin", "/usr/bin", "/usr/sbin"};
    char sudo_path[256], cmd[2048];
    char self[1024];
    ssize_t len;
    int found = 0;

    if (geteuid() == 0)
        return 1;

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        snprintf(sudo_path, sizeof(sudo_path), "%s/sudo", paths[i]);
        if (access(sudo_path, F_OK) == 0) {
            found = 1;
            break;
        }
    }

    len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (!found || len <= 0) {
        fprintf(stderr, "This tool must be run as root.\n");
        return 0;
    }
    self[len] = '\0';

    snprintf(cmd, sizeof(cmd), "%s -S %s", sudo_path, self);
    for (int i = 1; i < argc; i++) {
        strcat(cmd, " ");
        strcat(cmd, argv[i]);
    }

    return system(cmd) == 0 ? -1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Main Menu                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void print_banner(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║           Ryzen SMU Debug Tool for Linux v" TOOL_VERSION "            ║\n");
    printf("║       Using ryzen_smu driver via libsmu interface          ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  CPU:          %s\n", get_processor_name());
    printf("  Codename:     %s\n", smu_codename_to_str(&obj));
    printf("  SMU FW:       v%s\n", smu_get_fw_version(&obj));
    printf("  MP1 IF:       v%d\n", get_if_version_int());
    if (smu_pm_tables_supported(&obj))
        printf("  PM Table:     v0x%06X (%u bytes, %u floats)\n",
               obj.pm_table_version, obj.pm_table_size, obj.pm_table_size / 4);
    printf("\n");
}

static void print_menu(void)
{
    printf("╭──────────────────────────────────────╮\n");
    printf("│  [1] System Information              │\n");
    printf("│  [2] Send SMU Command                │\n");
    printf("│  [3] PM Table Monitor (live)         │\n");
    printf("│  [4] PM Table Dump / Export          │\n");
    printf("│  [5] SMN Read                        │\n");
    printf("│  [6] SMN Write                       │\n");
    printf("│  [7] SMN Range Scan                  │\n");
    printf("│  [8] SMU Mailbox Scan                │\n");
    printf("│  [9] Memory Timings                  │\n");
    printf("│  [A] Export JSON Report              │\n");
    printf("│  [B] PM Table Named Summary          │\n");
    printf("│  [0] Exit                            │\n");
    printf("╰──────────────────────────────────────╯\n");
}

int cli_main(int argc, char **argv)
{
    char choice[16];
    (void)argc;
    (void)argv;

    print_banner();

    while (1) {
        print_menu();
        read_line("\n  Select: ", choice, sizeof(choice));

        if (choice[0] == '0' || choice[0] == 'q' || choice[0] == 'Q')
            break;

        switch (choice[0]) {
        case '1': show_system_info();               break;
        case '2': send_smu_command_interactive();    break;
        case '3': pm_table_monitor();                break;
        case '4': pm_table_dump();                   break;
        case '5': smn_read_interactive();            break;
        case '6': smn_write_interactive();           break;
        case '7': smn_range_scan();                  break;
        case '8': smu_mailbox_scan();                break;
        case '9': show_memory_timings();             break;
        case 'A': case 'a': export_json_report();    break;
        case 'B': case 'b': show_named_pm_summary(); break;
        default:
            printf("  Unknown option.\n");
            break;
        }
    }

    smu_free(&obj);
    printf("\nGoodbye.\n");
    return 0;
}

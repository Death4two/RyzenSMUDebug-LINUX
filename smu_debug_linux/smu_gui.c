/*
 * Ryzen SMU Debug Tool - GTK3 GUI
 * Displays system info, PM table, per-core Curve Optimizer, FMax override, SMU/SMN access.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <libsmu.h>
#include "smu_common.h"

#define CO_MAX_CORES  16
#define CO_MIN_MARGIN -60
#define CO_MAX_MARGIN  10
#define FMAX_MIN       0
#define FMAX_MAX       6000

static GtkWidget *log_text;
static GtkListStore *pm_store;
static GtkWidget *co_spins[CO_MAX_CORES];
static GtkWidget *co_set_buttons[CO_MAX_CORES];
static GtkWidget *fmax_spin;
static gboolean pm_timer_active;
static float *pm_max_values;
static unsigned int pm_num_entries;

static void log_append(const char *msg)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_text));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, msg, -1);
    gtk_text_buffer_insert(buf, &end, "\n", 1);
}

static void log_appendf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_append(buf);
}

static GtkWidget *add_tab(GtkWidget *notebook, const char *label, GtkWidget *child)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), child);
    gtk_widget_show(child);
    GtkWidget *tab_label = gtk_label_new(label);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll, tab_label);
    gtk_widget_show(scroll);
    return scroll;
}

/* ─── System Info ─── */
static GtkWidget *build_system_info_tab(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    unsigned int ccds, ccxs, cpc, phys;
    unsigned int fam, model;
    int row = 0;

#define ROW(lab, val) do { \
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(lab), 0, row, 1, 1); \
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(val), 1, row, 1, 1); \
    gtk_widget_set_halign(gtk_grid_get_child_at(GTK_GRID(grid), 1, row), GTK_ALIGN_START); \
    row++; } while(0)

    ROW("CPU Model:", smu_get_processor_name());
    ROW("Codename:", smu_codename_to_str(smu_get_obj()));
    smu_get_cpu_family_model(&fam, &model);
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%02X / 0x%02X", fam, model);
    ROW("Family / Model:", buf);
    ROW("SMU FW Version:", smu_get_fw_version(smu_get_obj()));
    snprintf(buf, sizeof(buf), "v%d", smu_get_if_version_int());
    ROW("MP1 IF Version:", buf);
    if (smu_pm_tables_supported(smu_get_obj())) {
        snprintf(buf, sizeof(buf), "0x%06X (%u bytes)", smu_get_obj()->pm_table_version, smu_get_obj()->pm_table_size);
        ROW("PM Table:", buf);
    } else {
        ROW("PM Table:", "Not supported");
    }
    if (smu_get_topology(&ccds, &ccxs, &cpc, &phys) == 0) {
        snprintf(buf, sizeof(buf), "%u CCD / %u CCX / %u cores per CCX", ccds, ccxs, cpc);
        ROW("Topology:", buf);
        snprintf(buf, sizeof(buf), "%u", phys);
        ROW("Physical Cores:", buf);
    }
#undef ROW
    return grid;
}

/* ─── PM Table ─── */
static void pm_table_refresh(void)
{
    smu_obj_t *obj = smu_get_obj();
    if (!smu_pm_tables_supported(obj) || !pm_store)
        return;
    unsigned char *pm_buf = calloc(obj->pm_table_size, 1);
    if (!pm_buf) return;
    if (smu_read_pm_table(obj, pm_buf, obj->pm_table_size) != SMU_Return_OK) {
        log_append("PM table read failed.");
        free(pm_buf);
        return;
    }
    float *table = (float *)pm_buf;
    unsigned int n = obj->pm_table_size / sizeof(float);
    if (pm_max_values && n == pm_num_entries) {
        for (unsigned int i = 0; i < n; i++) {
            if (table[i] > pm_max_values[i])
                pm_max_values[i] = table[i];
        }
    } else {
        free(pm_max_values);
        pm_num_entries = n;
        pm_max_values = malloc(n * sizeof(float));
        if (pm_max_values)
            for (unsigned int i = 0; i < n; i++)
                pm_max_values[i] = table[i];
    }
    gtk_list_store_clear(pm_store);
    for (unsigned int i = 0; i < n; i++) {
        GtkTreeIter iter;
        char idx[16], off[16], val[24], max[24];
        snprintf(idx, sizeof(idx), "%04u", i);
        snprintf(off, sizeof(off), "0x%04X", i * 4);
        snprintf(val, sizeof(val), "%.6f", table[i]);
        snprintf(max, sizeof(max), "%.6f", pm_max_values ? pm_max_values[i] : table[i]);
        gtk_list_store_append(pm_store, &iter);
        gtk_list_store_set(pm_store, &iter, 0, idx, 1, off, 2, val, 3, max, -1);
    }
    free(pm_buf);
}

static gboolean pm_timer_cb(gpointer data)
{
    (void)data;
    if (!pm_timer_active) return G_SOURCE_REMOVE;
    pm_table_refresh();
    return G_SOURCE_CONTINUE;
}

static void pm_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;
    pm_table_refresh();
    log_append("PM table refreshed.");
}

static void pm_auto_toggled(GtkToggleButton *tb, gpointer data)
{
    (void)data;
    pm_timer_active = gtk_toggle_button_get_active(tb);
    if (pm_timer_active)
        g_timeout_add(2000, pm_timer_cb, NULL);
}

static GtkWidget *build_pm_table_tab(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_refresh = gtk_button_new_with_label("Refresh");
    GtkWidget *btn_auto = gtk_check_button_new_with_label("Auto-refresh (2 s)");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(pm_refresh_clicked), NULL);
    g_signal_connect(btn_auto, "toggled", G_CALLBACK(pm_auto_toggled), NULL);
    gtk_container_add(GTK_CONTAINER(toolbar), btn_refresh);
    gtk_container_add(GTK_CONTAINER(toolbar), btn_auto);
    gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, FALSE, 0);

    pm_store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(pm_store));
    g_object_unref(pm_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Index", gtk_cell_renderer_text_new(), "text", 0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Offset", gtk_cell_renderer_text_new(), "text", 1, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Value", gtk_cell_renderer_text_new(), "text", 2, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Max", gtk_cell_renderer_text_new(), "text", 3, NULL));

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_widget_show_all(box);
    pm_table_refresh();
    return box;
}

/* ─── PBO (Curve Optimizer + FMax) ─── */
static void fmax_apply_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;
    unsigned int mhz = (unsigned int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(fmax_spin));
    if (smu_set_fmax(mhz) == 0) {
        log_appendf("FMax set to %u MHz.", mhz);
        unsigned int read_back;
        if (smu_get_fmax(&read_back) == 0)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(fmax_spin), (double)read_back);
    } else {
        log_append("FMax set failed (check RSMU / platform).");
    }
}

static void fmax_read_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;
    unsigned int mhz;
    if (smu_get_fmax(&mhz) == 0) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(fmax_spin), (double)mhz);
        log_appendf("FMax read: %u MHz.", mhz);
    } else {
        log_append("FMax read failed.");
    }
}

static void co_value_changed(GtkSpinButton *spin, gpointer data)
{
    (void)spin;
    int i = GPOINTER_TO_INT(data);
    if (i >= 0 && i < CO_MAX_CORES && co_set_buttons[i] != NULL)
        gtk_widget_set_sensitive(co_set_buttons[i], TRUE);
}

static void co_apply_single(int core_index)
{
    gdouble v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(co_spins[core_index]));
    int val = (int)v;
    if (val < CO_MIN_MARGIN) val = CO_MIN_MARGIN;
    if (val > CO_MAX_MARGIN) val = CO_MAX_MARGIN;
    if (smu_set_curve_optimizer(core_index, val) == 0)
        log_appendf("Core %d: set CO to %d.", core_index, val);
    else
        log_appendf("Core %d: CO set failed.", core_index);
}

static void co_apply_one_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    int core_index = GPOINTER_TO_INT(data);
    co_apply_single(core_index);
}

static void co_read_all_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;
    unsigned int ccds, ccxs, cpc, phys;
    int read_ok = 0;
    if (smu_get_topology(&ccds, &ccxs, &cpc, &phys) != 0) return;
    for (unsigned int i = 0; i < phys && i < CO_MAX_CORES; i++) {
        int val;
        if (smu_get_curve_optimizer((int)i, &val) == 0) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(co_spins[i]), (double)val);
            read_ok++;
        }
        usleep(2000);
    }
    if (read_ok > 0)
        log_appendf("Curve Optimizer: read %d core(s).", read_ok);
    else
        log_append("CO read not supported on this platform (GET failed). Set values and click Apply all CO.");
}

static GtkWidget *build_pbo_tab(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    g_object_set(grid, "margin-top", 16, NULL);

    /* FMax - label centered, then controls in a centered row */
    GtkWidget *fmax_label = gtk_label_new("FMax override (MHz):");
    gtk_widget_set_halign(fmax_label, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), fmax_label, 0, 0, 7, 1);
    GtkWidget *fmax_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(fmax_row, GTK_ALIGN_CENTER);
    fmax_spin = gtk_spin_button_new_with_range((gdouble)FMAX_MIN, (gdouble)FMAX_MAX, 25.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fmax_spin), 0.0);
    gtk_box_pack_start(GTK_BOX(fmax_row), fmax_spin, FALSE, FALSE, 0);
    GtkWidget *btn_fmax_read = gtk_button_new_with_label("Read");
    GtkWidget *btn_fmax_apply = gtk_button_new_with_label("Apply");
    g_signal_connect(btn_fmax_read, "clicked", G_CALLBACK(fmax_read_clicked), NULL);
    g_signal_connect(btn_fmax_apply, "clicked", G_CALLBACK(fmax_apply_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(fmax_row), btn_fmax_read, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fmax_row), btn_fmax_apply, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), fmax_row, 0, 1, 7, 1);

    /* Curve Optimizer - two CCD columns: Label | Spin | Set  (spacer)  Label | Spin | Set
     * Grid cols: 0=label, 1=spin, 2=set, 3=spacer, 4=label, 5=spin, 6=set */
    GtkWidget *co_heading = gtk_label_new("Per-core Curve Optimizer:");
    gtk_widget_set_tooltip_text(co_heading, "Enter offsets (-60 to +10). Click Set per-core.");
    gtk_widget_set_halign(co_heading, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), co_heading, 0, 2, 7, 1);

    GtkWidget *ccd0_label = gtk_label_new("CCD 0");
    GtkWidget *ccd1_label = gtk_label_new("CCD 1");
    gtk_widget_set_halign(ccd0_label, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(ccd1_label, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), ccd0_label, 0, 3, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), ccd1_label, 4, 3, 3, 1);

    unsigned int ccds, ccxs, cpc, phys;
    smu_get_topology(&ccds, &ccxs, &cpc, &phys);
    for (unsigned int i = 0; i < CO_MAX_CORES; i++) {
        int col_base = (i < 8) ? 0 : 4;
        int row = 4 + (int)(i % 8);
        GtkWidget *l = gtk_label_new(NULL);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        g_object_set(l, "margin-end", 6, NULL);
        char buf[32];
        snprintf(buf, sizeof(buf), "Core %u", i);
        gtk_label_set_text(GTK_LABEL(l), buf);
        gtk_grid_attach(GTK_GRID(grid), l, col_base, row, 1, 1);
        co_spins[i] = gtk_spin_button_new_with_range((gdouble)CO_MIN_MARGIN, (gdouble)CO_MAX_MARGIN, 1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(co_spins[i]), 0.0);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(co_spins[i]), TRUE);
        gtk_editable_set_editable(GTK_EDITABLE(co_spins[i]), TRUE);
        gtk_widget_set_can_focus(co_spins[i], TRUE);
        gtk_widget_set_sensitive(co_spins[i], TRUE);
        g_signal_connect(co_spins[i], "value-changed", G_CALLBACK(co_value_changed), GINT_TO_POINTER((int)i));
        gtk_grid_attach(GTK_GRID(grid), co_spins[i], col_base + 1, row, 1, 1);
        GtkWidget *set_btn = gtk_button_new_with_label("Set");
        co_set_buttons[i] = set_btn;
        g_signal_connect(set_btn, "clicked", G_CALLBACK(co_apply_one_clicked), GINT_TO_POINTER((int)i));
        gtk_grid_attach(GTK_GRID(grid), set_btn, col_base + 2, row, 1, 1);
        gtk_widget_set_sensitive(set_btn, FALSE);  /* enabled when user changes the offset (value-changed) */
    }
    /* Single CCD (8 or fewer physical cores): make CCD1 (cores 8-15) uneditable */
    if (phys <= 8) {
        for (unsigned int i = 8; i < CO_MAX_CORES; i++) {
            gtk_widget_set_sensitive(co_spins[i], FALSE);
            gtk_editable_set_editable(GTK_EDITABLE(co_spins[i]), FALSE);
            gtk_widget_set_sensitive(co_set_buttons[i], FALSE);
        }
    }
    GtkWidget *spacer = gtk_label_new("  ");
    gtk_grid_attach(GTK_GRID(grid), spacer, 3, 4, 1, 8);

    int btn_row = 4 + 8;
    GtkWidget *btn_co_read = gtk_button_new_with_label("Read current CO");
    g_signal_connect(btn_co_read, "clicked", G_CALLBACK(co_read_all_clicked), NULL);
    gtk_grid_attach(GTK_GRID(grid), btn_co_read, 2, btn_row, 3, 1);  /* center bottom */

    /* Center the whole grid in the tab */
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    /* Initial FMax read */
    unsigned int mhz;
    if (smu_get_fmax(&mhz) == 0)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(fmax_spin), (double)mhz);
    return box;
}

/* ─── SMU Command ─── */
static void smu_cmd_send_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkWidget *win = (GtkWidget *)data;
    GtkWidget *cmd_entry = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smu_cmd");
    GtkWidget *arg_entries = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smu_args");
    GtkWidget *combo = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smu_mailbox");
    GtkWidget *resp_tv = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smu_resp");
    if (!cmd_entry || !arg_entries || !combo || !resp_tv) return;
    unsigned int cmd_val;
    if (sscanf(gtk_entry_get_text(GTK_ENTRY(cmd_entry)), "%x", &cmd_val) != 1) {
        log_append("Invalid command (use hex).");
        return;
    }
    smu_arg_t args;
    memset(&args, 0, sizeof(args));
    GtkWidget **args_arr = (GtkWidget **)g_object_get_data(G_OBJECT(arg_entries), "entries");
    if (args_arr) {
        for (int i = 0; i < 6; i++) {
            const char *t = gtk_entry_get_text(GTK_ENTRY(args_arr[i]));
            unsigned int v;
            if (sscanf(t, "%x", &v) == 1)
                args.args[i] = v;
        }
    }
    enum smu_mailbox mb = (enum smu_mailbox)gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    smu_return_val ret = smu_send_command(smu_get_obj(), cmd_val, &args, mb);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(resp_tv));
    gtk_text_buffer_set_text(buf, "", -1);
    char line[256];
    snprintf(line, sizeof(line), "Status: 0x%02X %s\n", ret, smu_return_to_str(ret));
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf, &iter);
    gtk_text_buffer_insert(buf, &iter, line, -1);
    for (int i = 0; i < 6; i++) {
        snprintf(line, sizeof(line), "Arg%d: 0x%08X\n", i, args.args[i]);
        gtk_text_buffer_get_end_iter(buf, &iter);
        gtk_text_buffer_insert(buf, &iter, line, -1);
    }
    log_appendf("SMU command 0x%02X sent.", cmd_val);
}

static GtkWidget *build_smu_cmd_tab(GtkWidget *window)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    int row = 0;
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Command (hex):"), 0, row, 1, 1);
    GtkWidget *cmd_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(cmd_entry), "01");
    gtk_grid_attach(GTK_GRID(grid), cmd_entry, 1, row, 1, 1);
    row++;
    GtkWidget *args_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *args_arr[6];
    for (int i = 0; i < 6; i++) {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        char buf[16];
        snprintf(buf, sizeof(buf), "Arg%d:", i);
        gtk_container_add(GTK_CONTAINER(r), gtk_label_new(buf));
        args_arr[i] = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(args_arr[i]), "0");
        gtk_container_add(GTK_CONTAINER(r), args_arr[i]);
        gtk_container_add(GTK_CONTAINER(args_box), r);
    }
    g_object_set_data(G_OBJECT(args_box), "entries", args_arr);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Mailbox:"), 0, row, 1, 1);
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), NULL, "RSMU");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), NULL, "MP1");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), NULL, "HSMP");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_grid_attach(GTK_GRID(grid), combo, 1, row, 1, 1);
    row++;
    gtk_grid_attach(GTK_GRID(grid), args_box, 0, row, 2, 1);
    row++;
    GtkWidget *btn_send = gtk_button_new_with_label("Send");
    g_signal_connect(btn_send, "clicked", G_CALLBACK(smu_cmd_send_clicked), window);
    gtk_grid_attach(GTK_GRID(grid), btn_send, 0, row, 1, 1);
    row++;
    GtkWidget *resp_tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(resp_tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(resp_tv), GTK_WRAP_NONE);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), resp_tv);
    gtk_grid_attach(GTK_GRID(grid), sw, 0, row, 2, 3);
    g_object_set_data(G_OBJECT(window), "smu_cmd", cmd_entry);
    g_object_set_data(G_OBJECT(window), "smu_args", args_box);
    g_object_set_data(G_OBJECT(window), "smu_mailbox", combo);
    g_object_set_data(G_OBJECT(window), "smu_resp", resp_tv);
    gtk_widget_show_all(grid);
    return grid;
}

/* ─── SMN ─── */
static void smn_read_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkWidget *win = (GtkWidget *)data;
    GtkWidget *addr_e = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smn_addr");
    GtkWidget *val_e = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smn_val");
    GtkWidget *resp = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smn_resp");
    unsigned int addr, val;
    if (sscanf(gtk_entry_get_text(GTK_ENTRY(addr_e)), "%x", &addr) != 1) {
        log_append("Invalid SMN address.");
        return;
    }
    if (smu_read_smn_addr(smu_get_obj(), addr, &val) != SMU_Return_OK) {
        gtk_label_set_text(GTK_LABEL(resp), "Read failed.");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08X", val);
    gtk_entry_set_text(GTK_ENTRY(val_e), buf);
    snprintf(buf, sizeof(buf), "0x%08X = 0x%08X (%u)", addr, val, val);
    gtk_label_set_text(GTK_LABEL(resp), buf);
}

static void smn_write_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkWidget *win = (GtkWidget *)data;
    GtkWidget *addr_e = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smn_addr");
    GtkWidget *val_e = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smn_val");
    GtkWidget *resp = (GtkWidget *)g_object_get_data(G_OBJECT(win), "smn_resp");
    unsigned int addr, val;
    if (sscanf(gtk_entry_get_text(GTK_ENTRY(addr_e)), "%x", &addr) != 1 ||
        sscanf(gtk_entry_get_text(GTK_ENTRY(val_e)), "%x", &val) != 1) {
        log_append("Invalid SMN address or value.");
        return;
    }
    if (smu_write_smn_addr(smu_get_obj(), addr, val) != SMU_Return_OK) {
        gtk_label_set_text(GTK_LABEL(resp), "Write failed.");
        return;
    }
    gtk_label_set_text(GTK_LABEL(resp), "Write OK.");
}

static GtkWidget *build_smn_tab(GtkWidget *window)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    GtkWidget *addr_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(addr_e), "Address (hex)");
    GtkWidget *val_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(val_e), "Value (hex)");
    GtkWidget *resp = gtk_label_new("");
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Address:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), addr_e, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Value:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), val_e, 1, 1, 1, 1);
    GtkWidget *btn_r = gtk_button_new_with_label("Read");
    GtkWidget *btn_w = gtk_button_new_with_label("Write");
    g_signal_connect(btn_r, "clicked", G_CALLBACK(smn_read_clicked), window);
    g_signal_connect(btn_w, "clicked", G_CALLBACK(smn_write_clicked), window);
    gtk_grid_attach(GTK_GRID(grid), btn_r, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_w, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), resp, 0, 3, 2, 1);
    g_object_set_data(G_OBJECT(window), "smn_addr", addr_e);
    g_object_set_data(G_OBJECT(window), "smn_val", val_e);
    g_object_set_data(G_OBJECT(window), "smn_resp", resp);
    gtk_widget_show_all(grid);
    return grid;
}

/* ─── Log ─── */
static GtkWidget *build_log_tab(void)
{
    log_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_text), TRUE);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), log_text);
    log_append("Ryzen SMU Debug Tool GUI ready.");
    return sw;
}

static void window_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    pm_timer_active = FALSE;
    free(pm_max_values);
    pm_max_values = NULL;
    smu_free(smu_get_obj());
    gtk_main_quit();
}

int gui_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    GtkWidget *window, *notebook;

    gtk_init(&argc, &argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
    gtk_window_set_title(GTK_WINDOW(window), "Ryzen SMU Debug Tool");
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(window_destroy), NULL);

    notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(window), notebook);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             gtk_scrolled_window_new(NULL, NULL),
                             gtk_label_new("System Info"));
    GtkWidget *sys_scroll = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sys_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *sys_child = build_system_info_tab();
    gtk_container_add(GTK_CONTAINER(sys_scroll), sys_child);

    add_tab(notebook, "PM Table", build_pm_table_tab());
    add_tab(notebook, "PBO / Tuning", build_pbo_tab());

    GtkWidget *smu_grid = build_smu_cmd_tab(window);
    GtkWidget *smu_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(smu_sw), smu_grid);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), smu_sw, gtk_label_new("SMU Command"));

    GtkWidget *smn_grid = build_smn_tab(window);
    GtkWidget *smn_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(smn_sw), smn_grid);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), smn_sw, gtk_label_new("SMN"));

    GtkWidget *log_sw = build_log_tab();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), log_sw, gtk_label_new("Log"));

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}

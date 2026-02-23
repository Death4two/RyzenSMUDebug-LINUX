/*
 * Ryzen SMU Debug Tool - GTK4 GUI
 * Displays system info, PM table, per-core Curve Optimizer, FMax override, SMU/SMN access.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
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

/* ─── PmRow GObject for ColumnView ─── */

#define PM_ROW_TYPE (pm_row_get_type())
G_DECLARE_FINAL_TYPE(PmRow, pm_row, PM, ROW, GObject)

struct _PmRow {
    GObject parent;
    char idx[16];
    char offset[16];
    char value[24];
    char max[24];
};

G_DEFINE_TYPE(PmRow, pm_row, G_TYPE_OBJECT)

static void pm_row_class_init(PmRowClass *klass) { (void)klass; }
static void pm_row_init(PmRow *self) { (void)self; }

static PmRow *pm_row_new(const char *idx, const char *offset, const char *value, const char *max)
{
    PmRow *r = g_object_new(PM_ROW_TYPE, NULL);
    g_strlcpy(r->idx, idx, sizeof(r->idx));
    g_strlcpy(r->offset, offset, sizeof(r->offset));
    g_strlcpy(r->value, value, sizeof(r->value));
    g_strlcpy(r->max, max, sizeof(r->max));
    return r;
}

/* ─── Globals ─── */

static GtkWidget *log_text;
static GListStore *pm_store;
static GtkWidget *co_spins[CO_MAX_CORES];
static GtkWidget *co_set_buttons[CO_MAX_CORES];
static GtkWidget *fmax_spin;
static gboolean pm_timer_active;
static float *pm_max_values;
static unsigned int pm_num_entries;

/* ─── Log ─── */

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
    GtkWidget *_v = gtk_label_new(val); \
    gtk_widget_set_halign(_v, GTK_ALIGN_START); \
    gtk_grid_attach(GTK_GRID(grid), _v, 1, row, 1, 1); \
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
    g_list_store_remove_all(pm_store);
    for (unsigned int i = 0; i < n; i++) {
        char idx[16], off[16], val[24], max[24];
        snprintf(idx, sizeof(idx), "%04u", i);
        snprintf(off, sizeof(off), "0x%04X", i * 4);
        snprintf(val, sizeof(val), "%.6f", table[i]);
        snprintf(max, sizeof(max), "%.6f", pm_max_values ? pm_max_values[i] : table[i]);
        PmRow *row = pm_row_new(idx, off, val, max);
        g_list_store_append(pm_store, row);
        g_object_unref(row);
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
    (void)btn; (void)data;
    pm_table_refresh();
    log_append("PM table refreshed.");
}

static void pm_auto_toggled(GtkCheckButton *cb, gpointer data)
{
    (void)data;
    pm_timer_active = gtk_check_button_get_active(cb);
    if (pm_timer_active)
        g_timeout_add(2000, pm_timer_cb, NULL);
}

/* Column factory helpers */
static void setup_label_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkWidget *label = gtk_label_new("");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_list_item_set_child(item, label);
}

static void bind_idx_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    PmRow *row = gtk_list_item_get_item(item);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), row->idx);
}

static void bind_offset_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    PmRow *row = gtk_list_item_get_item(item);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), row->offset);
}

static void bind_value_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    PmRow *row = gtk_list_item_get_item(item);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), row->value);
}

static void bind_max_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    PmRow *row = gtk_list_item_get_item(item);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), row->max);
}

static void add_pm_column(GtkColumnView *cv, const char *title, GCallback bind_cb)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(setup_label_cb), NULL);
    g_signal_connect(factory, "bind", bind_cb, NULL);
    GtkColumnViewColumn *col = gtk_column_view_column_new(title, factory);
    gtk_column_view_column_set_expand(col, TRUE);
    gtk_column_view_append_column(cv, col);
}

static GtkWidget *build_pm_table_tab(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_refresh = gtk_button_new_with_label("Refresh");
    GtkWidget *btn_auto = gtk_check_button_new_with_label("Auto-refresh (2 s)");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(pm_refresh_clicked), NULL);
    g_signal_connect(btn_auto, "toggled", G_CALLBACK(pm_auto_toggled), NULL);
    gtk_box_append(GTK_BOX(toolbar), btn_refresh);
    gtk_box_append(GTK_BOX(toolbar), btn_auto);
    gtk_box_append(GTK_BOX(box), toolbar);

    pm_store = g_list_store_new(PM_ROW_TYPE);
    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(pm_store));
    GtkWidget *cv = gtk_column_view_new(GTK_SELECTION_MODEL(sel));

    add_pm_column(GTK_COLUMN_VIEW(cv), "Index", G_CALLBACK(bind_idx_cb));
    add_pm_column(GTK_COLUMN_VIEW(cv), "Offset", G_CALLBACK(bind_offset_cb));
    add_pm_column(GTK_COLUMN_VIEW(cv), "Value", G_CALLBACK(bind_value_cb));
    add_pm_column(GTK_COLUMN_VIEW(cv), "Max", G_CALLBACK(bind_max_cb));

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), cv);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);
    pm_table_refresh();
    return box;
}

/* ─── PBO (Curve Optimizer + FMax) ─── */
static void fmax_apply_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
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
    (void)btn; (void)data;
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
    (void)btn; (void)data;
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

    /* FMax */
    GtkWidget *fmax_label = gtk_label_new("FMax override (MHz):");
    gtk_widget_set_halign(fmax_label, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), fmax_label, 0, 0, 7, 1);
    GtkWidget *fmax_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(fmax_row, GTK_ALIGN_CENTER);
    fmax_spin = gtk_spin_button_new_with_range((gdouble)FMAX_MIN, (gdouble)FMAX_MAX, 25.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fmax_spin), 0.0);
    gtk_box_append(GTK_BOX(fmax_row), fmax_spin);
    GtkWidget *btn_fmax_read = gtk_button_new_with_label("Read");
    GtkWidget *btn_fmax_apply = gtk_button_new_with_label("Apply");
    g_signal_connect(btn_fmax_read, "clicked", G_CALLBACK(fmax_read_clicked), NULL);
    g_signal_connect(btn_fmax_apply, "clicked", G_CALLBACK(fmax_apply_clicked), NULL);
    gtk_box_append(GTK_BOX(fmax_row), btn_fmax_read);
    gtk_box_append(GTK_BOX(fmax_row), btn_fmax_apply);
    gtk_grid_attach(GTK_GRID(grid), fmax_row, 0, 1, 7, 1);

    /* Curve Optimizer */
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
        gtk_widget_set_sensitive(co_spins[i], TRUE);
        g_signal_connect(co_spins[i], "value-changed", G_CALLBACK(co_value_changed), GINT_TO_POINTER((int)i));
        gtk_grid_attach(GTK_GRID(grid), co_spins[i], col_base + 1, row, 1, 1);
        GtkWidget *set_btn = gtk_button_new_with_label("Set");
        co_set_buttons[i] = set_btn;
        g_signal_connect(set_btn, "clicked", G_CALLBACK(co_apply_one_clicked), GINT_TO_POINTER((int)i));
        gtk_grid_attach(GTK_GRID(grid), set_btn, col_base + 2, row, 1, 1);
        gtk_widget_set_sensitive(set_btn, FALSE);
    }
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
    gtk_grid_attach(GTK_GRID(grid), btn_co_read, 2, btn_row, 3, 1);

    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), grid);
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
    if (sscanf(gtk_editable_get_text(GTK_EDITABLE(cmd_entry)), "%x", &cmd_val) != 1) {
        log_append("Invalid command (use hex).");
        return;
    }
    smu_arg_t args;
    memset(&args, 0, sizeof(args));
    GtkWidget **args_arr = (GtkWidget **)g_object_get_data(G_OBJECT(arg_entries), "entries");
    if (args_arr) {
        for (int i = 0; i < 6; i++) {
            const char *t = gtk_editable_get_text(GTK_EDITABLE(args_arr[i]));
            unsigned int v;
            if (sscanf(t, "%x", &v) == 1)
                args.args[i] = v;
        }
    }
    enum smu_mailbox mb = (enum smu_mailbox)gtk_drop_down_get_selected(GTK_DROP_DOWN(combo));
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
    gtk_editable_set_text(GTK_EDITABLE(cmd_entry), "01");
    gtk_grid_attach(GTK_GRID(grid), cmd_entry, 1, row, 1, 1);
    row++;
    GtkWidget *args_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget **args_arr = g_new(GtkWidget *, 6);
    for (int i = 0; i < 6; i++) {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        char buf[16];
        snprintf(buf, sizeof(buf), "Arg%d:", i);
        gtk_box_append(GTK_BOX(r), gtk_label_new(buf));
        args_arr[i] = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(args_arr[i]), "0");
        gtk_box_append(GTK_BOX(r), args_arr[i]);
        gtk_box_append(GTK_BOX(args_box), r);
    }
    g_object_set_data(G_OBJECT(args_box), "entries", args_arr);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Mailbox:"), 0, row, 1, 1);
    const char *mailboxes[] = {"RSMU", "MP1", "HSMP", NULL};
    GtkStringList *sl = gtk_string_list_new(mailboxes);
    GtkWidget *combo = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(combo), 0);
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
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), resp_tv);
    gtk_grid_attach(GTK_GRID(grid), sw, 0, row, 2, 3);
    g_object_set_data(G_OBJECT(window), "smu_cmd", cmd_entry);
    g_object_set_data(G_OBJECT(window), "smu_args", args_box);
    g_object_set_data(G_OBJECT(window), "smu_mailbox", combo);
    g_object_set_data(G_OBJECT(window), "smu_resp", resp_tv);
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
    if (sscanf(gtk_editable_get_text(GTK_EDITABLE(addr_e)), "%x", &addr) != 1) {
        log_append("Invalid SMN address.");
        return;
    }
    if (smu_read_smn_addr(smu_get_obj(), addr, &val) != SMU_Return_OK) {
        gtk_label_set_text(GTK_LABEL(resp), "Read failed.");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08X", val);
    gtk_editable_set_text(GTK_EDITABLE(val_e), buf);
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
    if (sscanf(gtk_editable_get_text(GTK_EDITABLE(addr_e)), "%x", &addr) != 1 ||
        sscanf(gtk_editable_get_text(GTK_EDITABLE(val_e)), "%x", &val) != 1) {
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
    return grid;
}

/* ─── Log ─── */
static GtkWidget *build_log_tab(void)
{
    log_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_text), TRUE);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), log_text);
    log_append("Ryzen SMU Debug Tool GUI ready.");
    return sw;
}

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
    (void)win; (void)data;
    pm_timer_active = FALSE;
    free(pm_max_values);
    pm_max_values = NULL;
    smu_free(smu_get_obj());
    return FALSE;
}

static void activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    GtkWidget *window, *notebook;

    window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
    gtk_window_set_title(GTK_WINDOW(window), "Ryzen SMU Debug Tool");
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), NULL);

    notebook = gtk_notebook_new();
    gtk_window_set_child(GTK_WINDOW(window), notebook);

    /* System Info tab */
    GtkWidget *sys_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sys_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sys_scroll), build_system_info_tab());
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sys_scroll, gtk_label_new("System Info"));

    /* PM Table tab */
    GtkWidget *pm_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pm_sw), build_pm_table_tab());
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pm_sw, gtk_label_new("PM Table"));

    /* PBO tab */
    GtkWidget *pbo_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pbo_sw), build_pbo_tab());
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pbo_sw, gtk_label_new("PBO / Tuning"));

    /* SMU Command tab */
    GtkWidget *smu_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(smu_sw), build_smu_cmd_tab(window));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), smu_sw, gtk_label_new("SMU Command"));

    /* SMN tab */
    GtkWidget *smn_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(smn_sw), build_smn_tab(window));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), smn_sw, gtk_label_new("SMN"));

    /* Log tab */
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_log_tab(), gtk_label_new("Log"));

    gtk_window_present(GTK_WINDOW(window));
}

int gui_main(int argc, char **argv)
{
    /* Strip --gui / -g from argv before passing to GtkApplication (it rejects unknown options). */
    int new_argc = 0;
    char **new_argv = g_new(char *, argc + 1);
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--gui") != 0 && strcmp(argv[i], "-g") != 0)
            new_argv[new_argc++] = argv[i];
    }
    new_argv[new_argc] = NULL;

    GtkApplication *app = gtk_application_new("com.ryzen.smudebug", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), new_argc, new_argv);
    g_object_unref(app);
    g_free(new_argv);
    return status;
}

/**
 * @file gtk_probe.c
 * @brief GTK 3 example 1: probe for LRU sensors and test the link.
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * Shows how to use the "probe" half of openlru-lib:
 *
 *   - olru_probe()      fills a list with every supported sensor plugged in.
 *   - olru_open()       claims the selected sensor and runs the handshake.
 *   - olru_close()      releases it again.
 *
 * Build:  make examples        
 * Run:  ./build/gtk_probe
 */

#include <gtk/gtk.h>

#include "openlru.h"

/** Columns of the list store. */
enum { COL_MODEL, COL_USB_ID, COL_BUS_ADDR, COL_LINK, COL_COUNT };

/** Everything the callbacks need. */
struct app {
    GtkListStore    *store;                        /* rows shown in the tree */
    GtkWidget       *tree;                         /* the GtkTreeView        */
    GtkWidget       *status;                       /* status label           */
    olru_device_info sensors[OLRU_MAX_DEVICES];    /* result of olru_probe() */
    int              count;                        /* entries in sensors[]   */
};

/** Show a message in the status line. */
static void set_status(struct app *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status), text);
}

/** Append one sensor to the list store. */
static void add_row(struct app *app, const olru_device_info *s)
{
    GtkTreeIter iter;
    char usb_id[16], bus_addr[16];

    g_snprintf(usb_id, sizeof usb_id, "%04x:%04x", s->vid, s->pid);
    g_snprintf(bus_addr, sizeof bus_addr, "%03u:%03u", s->bus, s->address);
    gtk_list_store_append(app->store, &iter);
    gtk_list_store_set(app->store, &iter,
                       COL_MODEL,    s->name,
                       COL_USB_ID,   usb_id,
                       COL_BUS_ADDR, bus_addr,
                       COL_LINK,     s->link == OLRU_LINK_BULK ? "bulk" : "mass-storage",
                       -1);
}

/** "Scan" button: call olru_probe() and refill the list. */
static void on_scan(GtkButton *button, gpointer data)
{
    struct app *app = data;
    char *text;

    (void)button;
    gtk_list_store_clear(app->store);
    app->count = olru_probe(app->sensors, OLRU_MAX_DEVICES);
    if (app->count < 0) {
        set_status(app, olru_strerror(app->count));
        app->count = 0;
        return;
    }
    for (int i = 0; i < app->count; i++)
        add_row(app, &app->sensors[i]);
    text = g_strdup_printf("%d sensor(s) found", app->count);
    set_status(app, text);
    g_free(text);
}

/** @return index of the selected row, or -1 if nothing is selected. */
static int selected_index(struct app *app)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree));
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTreePath *path;
    int index;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return -1;
    path = gtk_tree_model_get_path(model, &iter);
    index = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return index;
}

/** "Open" button: open the selected sensor (includes the link handshake). */
static void on_open(GtkButton *button, gpointer data)
{
    struct app *app = data;
    olru_device *dev = NULL;
    int index = selected_index(app);
    int rc;
    char *text;

    (void)button;
    if (index < 0) {
        set_status(app, "Select a sensor first");
        return;
    }
    rc = olru_open(&app->sensors[index], &dev);
    if (rc == OLRU_OK) {
        set_status(app, "Sensor opened and answered the link check");
        olru_close(dev);
        return;
    }
    text = g_strdup_printf("Open failed: %s", olru_strerror(rc));
    set_status(app, text);
    g_free(text);
}

/** Create the tree view with its four text columns. */
static GtkWidget *build_tree(struct app *app)
{
    static const char *titles[COL_COUNT] = { "Model", "USB id", "Bus:Addr", "Link" };
    GtkWidget *tree;

    app->store = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING,
                                    G_TYPE_STRING, G_TYPE_STRING);
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->store));
    for (int i = 0; i < COL_COUNT; i++) {
        GtkCellRenderer *cell = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            titles[i], cell, "text", i, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
    }
    return tree;
}

/** Create the button row. */
static GtkWidget *build_buttons(struct app *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scan = gtk_button_new_with_label("Scan USB bus");
    GtkWidget *open = gtk_button_new_with_label("Open selected sensor");

    g_signal_connect(scan, "clicked", G_CALLBACK(on_scan), app);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open), app);
    gtk_box_pack_start(GTK_BOX(box), scan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), open, FALSE, FALSE, 0);
    return box;
}

/** Build the whole window. */
static GtkWidget *build_window(struct app *app)
{
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_window_set_title(GTK_WINDOW(win), "openlru-lib: probe sensors");
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 300);
    gtk_container_set_border_width(GTK_CONTAINER(win), 10);
    app->tree = build_tree(app);
    app->status = gtk_label_new("Press \"Scan USB bus\"");
    gtk_widget_set_halign(app->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), build_buttons(app), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), app->tree, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), app->status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    return win;
}

int main(int argc, char **argv)
{
    static struct app app;
    GtkWidget *win;

    gtk_init(&argc, &argv);
    win = build_window(&app);
    gtk_widget_show_all(win);
    on_scan(NULL, &app);                 /* fill the list at start-up */
    gtk_main();

    return 0;
}

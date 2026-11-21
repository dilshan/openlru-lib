/**
 * @file gtk_capture.c
 * @brief GTK 3 example 2: capture a fingerprint image and template.
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * Shows the "read fingerprint" half of openlru-lib:
 *
 *   olru_probe() + olru_open()   find and open the first sensor
 *   olru_capture_image()         polled every 100 ms until a finger is seen
 *   olru_read_image()            download the 256x288 raw image
 *   olru_image_to_bmp()          turn it into a BMP for display
 *   olru_read_template()         have the sensor extract a 256 byte template
 *   olru_image_save_bmp()        write the image to a file
 *
 * The finger polling uses g_timeout_add() instead of a thread: each poll is a
 * single quick USB exchange, so the window stays responsive.
 *
 * Flow:   [Open sensor] -> [Capture] -> place finger -> image appears
 *                              \-> [Read template] / [Save image...]
 *
 * Build:  make examples        
 * Run:  ./build/gtk_capture
 */

#include <gtk/gtk.h>

#include "openlru.h"

#define POLL_INTERVAL_MS  100        /* how often to ask for a finger   */
#define POLL_TIMEOUT_MS   15000      /* give up waiting after 15 s      */

/** Everything the callbacks need. */
struct app {
    GtkWidget   *window;
    GtkWidget   *image;              /* shows the fingerprint           */
    GtkWidget   *status;             /* one-line status                 */
    GtkWidget   *hex;                /* GtkTextView with the template   */
    GtkWidget   *btn_open, *btn_capture, *btn_template, *btn_save;
    olru_device *dev;                /* NULL while no sensor is open    */
    uint8_t      raw[OLRU_IMAGE_SIZE];
    gboolean     have_image;         /* raw[] holds a captured image    */
    guint        poll_id;            /* GSource id of the finger poll   */
    unsigned     waited_ms;          /* time spent waiting for a finger */
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void set_status(struct app *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status), text);
}

/** Show "<what>: <library error text>" in the status line. */
static void set_error(struct app *app, const char *what, int rc)
{
    char *text = g_strdup_printf("%s: %s", what, olru_strerror(rc));

    set_status(app, text);
    g_free(text);
}

/** Enable or disable buttons to match the current state. */
static void update_buttons(struct app *app)
{
    gboolean open = app->dev != NULL;
    gboolean polling = app->poll_id != 0;

    gtk_button_set_label(GTK_BUTTON(app->btn_open),
                         open ? "Close sensor" : "Open sensor");
    gtk_widget_set_sensitive(app->btn_capture, open && !polling);
    gtk_widget_set_sensitive(app->btn_template, open && app->have_image);
    gtk_widget_set_sensitive(app->btn_save, app->have_image);
}

/** Stop the finger poll if it is running. */
static void stop_polling(struct app *app)
{
    if (app->poll_id)
        g_source_remove(app->poll_id);
    app->poll_id = 0;
}

/* ------------------------------------------------------------------ */
/* Showing results                                                    */
/* ------------------------------------------------------------------ */

/** Decode the BMP built by the library into a GdkPixbuf (or NULL). */
static GdkPixbuf *pixbuf_from_raw(const uint8_t *raw)
{
    static uint8_t bmp[OLRU_BMP_SIZE];
    GdkPixbufLoader *loader;
    GdkPixbuf *pixbuf = NULL;

    if (olru_image_to_bmp(raw, bmp, sizeof bmp) < 0)
        return NULL;
    loader = gdk_pixbuf_loader_new();
    if (gdk_pixbuf_loader_write(loader, bmp, sizeof bmp, NULL) &&
        gdk_pixbuf_loader_close(loader, NULL)) {
        pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        g_object_ref(pixbuf);
    }
    g_object_unref(loader);
    return pixbuf;
}

static void show_image(struct app *app)
{
    GdkPixbuf *pixbuf = pixbuf_from_raw(app->raw);

    if (!pixbuf)
        return;
    gtk_image_set_from_pixbuf(GTK_IMAGE(app->image), pixbuf);
    g_object_unref(pixbuf);
}

/** Format @p data as lines of 16 hex bytes. */
static char *hex_dump(const uint8_t *data, size_t len)
{
    GString *text = g_string_new(NULL);

    for (size_t i = 0; i < len; i++)
        g_string_append_printf(text, "%02x%s", data[i],
                               (i % 16 == 15) ? "\n" : " ");
    return g_string_free(text, FALSE);
}

static void show_template(struct app *app, const uint8_t *tpl, size_t len)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->hex));
    char *text = hex_dump(tpl, len);

    gtk_text_buffer_set_text(buf, text, -1);
    g_free(text);
}

/* ------------------------------------------------------------------ */
/* Capture                                                            */
/* ------------------------------------------------------------------ */

/** A finger was detected: download and show the image. */
static void finish_capture(struct app *app)
{
    int rc = olru_read_image(app->dev, app->raw, sizeof app->raw);

    if (rc != OLRU_OK) {
        set_error(app, "Reading image failed", rc);
        return;
    }
    app->have_image = TRUE;
    show_image(app);
    set_status(app, "Image captured");
}

/** Timer callback: ask the sensor whether a finger is present. */
static gboolean poll_finger(gpointer data)
{
    struct app *app = data;
    int rc = olru_capture_image(app->dev);

    app->waited_ms += POLL_INTERVAL_MS;
    if (rc == OLRU_ERR_NO_FINGER && app->waited_ms < POLL_TIMEOUT_MS)
        return G_SOURCE_CONTINUE;              /* keep polling */
    app->poll_id = 0;
    if (rc == OLRU_OK)
        finish_capture(app);
    else if (rc == OLRU_ERR_NO_FINGER)
        set_status(app, "Timed out waiting for a finger");
    else
        set_error(app, "Capture failed", rc);
    update_buttons(app);
    return G_SOURCE_REMOVE;
}

static void on_capture(GtkButton *button, gpointer data)
{
    struct app *app = data;

    (void)button;
    app->waited_ms = 0;
    app->have_image = FALSE;
    app->poll_id = g_timeout_add(POLL_INTERVAL_MS, poll_finger, app);
    set_status(app, "Place a finger on the sensor...");
    update_buttons(app);
}

static void on_template(GtkButton *button, gpointer data)
{
    struct app *app = data;
    uint8_t tpl[OLRU_TEMPLATE_SIZE];
    int rc = olru_read_template(app->dev, tpl);

    (void)button;
    if (rc == OLRU_OK) {
        show_template(app, tpl, sizeof tpl);
        set_status(app, "Template read");
        return;
    }
    set_error(app, olru_device_code_text(olru_device_code(app->dev)), rc);
}

/* ------------------------------------------------------------------ */
/* Saving                                                             */
/* ------------------------------------------------------------------ */

/** Ask the user for a file name; returns a newly allocated path or NULL. */
static char *choose_save_path(struct app *app)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save fingerprint image", GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT, NULL);
    char *path = NULL;

    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "finger.bmp");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);
    return path;
}

static void on_save(GtkButton *button, gpointer data)
{
    struct app *app = data;
    char *path = choose_save_path(app);
    int rc;

    (void)button;
    if (!path)
        return;
    rc = olru_image_save_bmp(app->raw, path);
    if (rc == OLRU_OK)
        set_status(app, "Image saved");
    else
        set_error(app, "Saving failed", rc);
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* Open / close the sensor                                            */
/* ------------------------------------------------------------------ */

static void open_sensor(struct app *app)
{
    olru_device_info list[OLRU_MAX_DEVICES];
    int count = olru_probe(list, OLRU_MAX_DEVICES);
    int rc;

    if (count <= 0) {
        set_error(app, "Probe", count < 0 ? count : OLRU_ERR_NO_DEVICE);
        return;
    }
    rc = olru_open(&list[0], &app->dev);        /* use the first sensor */
    if (rc != OLRU_OK)
        set_error(app, "Open failed", rc);
    else
        set_status(app, "Sensor ready - press Capture");
}

static void close_sensor(struct app *app)
{
    stop_polling(app);
    olru_close(app->dev);
    app->dev = NULL;
    set_status(app, "Sensor closed");
}

static void on_open_close(GtkButton *button, gpointer data)
{
    struct app *app = data;

    (void)button;
    if (app->dev)
        close_sensor(app);
    else
        open_sensor(app);
    update_buttons(app);
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    struct app *app = data;

    (void)widget;
    stop_polling(app);
    olru_close(app->dev);
    gtk_main_quit();
}

/* ------------------------------------------------------------------ */
/* Window construction                                                */
/* ------------------------------------------------------------------ */

/** Create a button, wire it to @p handler and add it to @p box. */
static GtkWidget *add_button(GtkWidget *box, const char *label,
                             GCallback handler, struct app *app)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    g_signal_connect(button, "clicked", handler, app);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    return button;
}

static GtkWidget *build_buttons(struct app *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    app->btn_open     = add_button(box, "Open sensor", G_CALLBACK(on_open_close), app);
    app->btn_capture  = add_button(box, "Capture", G_CALLBACK(on_capture), app);
    app->btn_template = add_button(box, "Read template", G_CALLBACK(on_template), app);
    app->btn_save     = add_button(box, "Save image...", G_CALLBACK(on_save), app);
    return box;
}

/** Image on the left, template hex dump on the right. */
static GtkWidget *build_content(struct app *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);

    app->image = gtk_image_new();
    gtk_widget_set_size_request(app->image, OLRU_IMAGE_WIDTH, OLRU_IMAGE_HEIGHT);
    app->hex = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->hex), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->hex), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), app->hex);
    gtk_widget_set_size_request(scroll, 420, -1);
    gtk_box_pack_start(GTK_BOX(box), app->image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    return box;
}

static void build_window(struct app *app)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "openlru-lib: capture fingerprint");
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 10);
    app->status = gtk_label_new("Press \"Open sensor\"");
    gtk_widget_set_halign(app->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), build_buttons(app), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_content(app), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), app->status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_destroy), app);
}

int main(int argc, char **argv)
{
    static struct app app;

    gtk_init(&argc, &argv);
    build_window(&app);
    update_buttons(&app);
    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}

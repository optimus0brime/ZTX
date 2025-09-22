#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkTextBuffer *buffer;
    char *device_name;
} DeviceTaskData;

// Helper: Run shell command and capture output (simplified)
static gchar* run_command(const char *cmd) {
    GError *error = NULL;
    gchar *output = NULL;

    if (!g_spawn_command_line_sync(cmd, &output, NULL, NULL, &error)) {
        g_warning("Failed to run command '%s': %s", cmd, error->message);
        g_clear_error(&error);
        return NULL;
    }
    return output;
}

// Background task to fetch device details asynchronously
static void device_detail_task(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    DeviceTaskData *data = (DeviceTaskData *)task_data;
    gchar *cmd = g_strdup_printf("lsblk -o NAME,MOUNTPOINT,UUID,SIZE,ROTA,TYPE,MODEL,VENDOR /dev/%s && smartctl -i /dev/%s", data->device_name, data->device_name);

    gchar *details = run_command(cmd);
    g_free(cmd);

    g_task_return_pointer(task, details, g_free);
}

static void device_detail_task_done(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);

    gchar *details = g_task_propagate_pointer(task, NULL);
    if (details)
        gtk_text_buffer_set_text(buffer, details, -1);
    else
        gtk_text_buffer_set_text(buffer, "Failed to get device details", -1);
}

// Called on device selection, starts async fetch
static void on_device_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    GtkTextView *text_view = GTK_TEXT_VIEW(user_data);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(text_view);
    gtk_text_buffer_set_text(buffer, "Loading details...", -1);

    if (!row) {
        gtk_text_buffer_set_text(buffer, "", -1);
        return;
    }

    GtkWidget *label = gtk_list_box_row_get_child(row);
    const gchar *device_name_txt = gtk_label_get_text(GTK_LABEL(label));

    // Launch async device detail fetch
    DeviceTaskData *task_data = g_new(DeviceTaskData, 1);
    task_data->buffer = buffer;
    task_data->device_name = g_strdup(device_name_txt);

    GTask *task = g_task_new(NULL, NULL, device_detail_task_done, buffer);
    g_task_set_task_data(task, task_data, (GDestroyNotify)g_free);
    g_task_run_in_thread(task, device_detail_task);

    g_object_unref(task);
}

// Load devices into list box
static void load_devices(GtkListBox *listbox) {
    gchar *output = run_command("lsblk -dno NAME");

    if (!output)
        return;

    gchar **lines = g_strsplit(output, "\n", 0);
    for (int i = 0; lines[i] != NULL; i++) {
        if (strlen(lines[i]) == 0) continue;
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(lines[i]);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        gtk_list_box_append(listbox, row);
    }
    gtk_widget_show(GTK_WIDGET(listbox));
    g_strfreev(lines);
    g_free(output);
}

static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Storage Device Scanner");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_window_set_child(GTK_WINDOW(window), hpaned);

    // Left container with title and device list
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_paned_set_start_child(GTK_PANED(hpaned), left_vbox);
    gtk_widget_set_margin_start(left_vbox, 10);
    gtk_widget_set_margin_top(left_vbox, 10);
    gtk_widget_set_margin_bottom(left_vbox, 10);

    GtkWidget *device_list_label = gtk_label_new("Devices");
    gtk_label_set_xalign(GTK_LABEL(device_list_label), 0.0);
    gtk_box_append(GTK_BOX(left_vbox), device_list_label);

    GtkWidget *device_list = gtk_list_box_new();
    gtk_widget_set_vexpand(device_list, TRUE);
    gtk_widget_set_hexpand(device_list, TRUE);
    gtk_box_append(GTK_BOX(left_vbox), device_list);

    // Right container with title and text view
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_paned_set_end_child(GTK_PANED(hpaned), right_vbox);
    gtk_widget_set_margin_end(right_vbox, 10);
    gtk_widget_set_margin_top(right_vbox, 10);
    gtk_widget_set_margin_bottom(right_vbox, 10);

    GtkWidget *details_label = gtk_label_new("Device Details");
    gtk_label_set_xalign(GTK_LABEL(details_label), 0.0);
    gtk_box_append(GTK_BOX(right_vbox), details_label);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_widget_set_hexpand(scrolled_window, TRUE);
    gtk_box_append(GTK_BOX(right_vbox), scrolled_window);

    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), text_view);

    // Apply monospace font to text view using CSS
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        "textview {"
        "   font-family: monospace;"
        "   font-size: 10pt;"
        "}", -1, NULL);

    GtkStyleContext *context = gtk_widget_get_style_context(text_view);
    gtk_style_context_add_provider(context,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_object_unref(provider);

    load_devices(GTK_LIST_BOX(device_list));
    g_signal_connect(device_list, "row-selected", G_CALLBACK(on_device_selected), text_view);

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char *argv[]) {
    GtkApplication *app;
    int status;

    app = gtk_application_new("com.example.StorageScanner", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gio/gio.h>

/* A struct to hold all our application state */
typedef struct {
    GtkWidget *window;
    GtkWidget *start_button;
    GtkWidget *stop_button;

    GstElement *pipeline;
    GDBusProxy *portal_proxy;
    gchar *session_handle;
    GCancellable *cancellable;
    guint request_signal_id; /* ID for the DBus signal subscription */
} AppData;

/* Forward declarations for callbacks */
static void on_start_called (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_sources_selected (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_session_created (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_proxy_created (GObject *source_object, GAsyncResult *res, gpointer user_data);

/* Checks for required GStreamer plugins and shows an error dialog if they are missing */
static gboolean
check_gst_plugins (GtkWindow *parent)
{
    const gchar *missing_plugin = NULL;
    const gchar *missing_pkg = NULL;

    const gchar *plugins[][2] = {
        {"pipewiresrc", "gstreamer1.0-pipewire"},
        {"x264enc", "gstreamer1.0-plugins-ugly"},
        {"avenc_aac", "gstreamer1.0-libav"},
        {"pulsesrc", "gstreamer1.0-plugins-good"},
        {NULL, NULL}
    };

    for (int i = 0; plugins[i][0] != NULL; ++i) {
        GstElementFactory *factory = gst_element_factory_find (plugins[i][0]);
        if (!factory) {
            missing_plugin = plugins[i][0];
            missing_pkg = plugins[i][1];
            break;
        }
        g_object_unref (factory);
    }

    if (missing_plugin) {
        gchar *primary_text = g_strdup_printf ("Required GStreamer plugin not found: %s", missing_plugin);
        gchar *secondary_text = g_strdup_printf ("Please install the package '%s' and try again.", missing_pkg);

        GtkWidget *dialog = gtk_message_dialog_new (parent,
                                                    GTK_DIALOG_MODAL,
                                                    GTK_MESSAGE_ERROR,
                                                    GTK_BUTTONS_OK,
                                                    "%s", primary_text);
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", secondary_text);
        
        /* The g_application_run is not running yet, so we can't use gtk_dialog_run.
         * Instead we make our own main loop to show the dialog.
         */
        gtk_widget_show(dialog);
        while(g_main_context_iteration(NULL, TRUE));
        
        gtk_window_destroy (GTK_WINDOW (dialog));

        g_free (primary_text);
        g_free (secondary_text);
        return FALSE;
    }

    return TRUE;
}

/* Callback for when the window is destroyed */
static void
on_window_destroy (GtkWidget *widget, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    data->window = NULL;
    data->start_button = NULL;
    data->stop_button = NULL;
}

/* Resets the UI and cleans up all recording-related state */
static void
reset_ui_and_state (AppData *data)
{
    g_print ("Resetting UI and state.\n");
    if (data->start_button) gtk_widget_set_sensitive (data->start_button, TRUE);
    if (data->stop_button) gtk_widget_set_sensitive (data->stop_button, FALSE);

    if (data->pipeline) {
        gst_element_set_state (data->pipeline, GST_STATE_NULL);
        g_clear_object (&data->pipeline);
    }
    g_clear_object (&data->portal_proxy);
    g_clear_pointer (&data->session_handle, g_free);
    g_clear_object (&data->cancellable);
    if (data->request_signal_id) {
        /* We can't easily unsubscribe without the connection, but the ID is invalid anyway */
        data->request_signal_id = 0;
    }
}

/* Callback to handle messages from the GStreamer pipeline */
static gboolean
on_gst_message (GstBus *bus, GstMessage *msg, AppData *data)
{
    switch (GST_MESSAGE_TYPE (msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg_info = NULL;
            gst_message_parse_error (msg, &err, &dbg_info);
            g_printerr ("GStreamer Error: %s\n", err->message);
            g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
            g_clear_error (&err);
            g_free (dbg_info);
            reset_ui_and_state (data);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print ("End-of-Stream reached. Finalizing file.\n");
            reset_ui_and_state (data);
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
            if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
                g_print ("Pipeline state changed from %s to %s\n",
                         gst_element_state_get_name (old_state),
                         gst_element_state_get_name (new_state));
            }
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar *dbg_info = NULL;
            gst_message_parse_warning (msg, &err, &dbg_info);
            g_printerr ("GStreamer Warning: %s\n", err->message);
            g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
            g_clear_error (&err);
            g_free (dbg_info);
            break;
        }
        default:
            break;
    }
    return TRUE; /* Keep the message handler attached */
}

/* Constructs and starts the GStreamer pipeline with the given PipeWire node IDs */
static void
start_gstreamer_pipeline (AppData *data, guint32 video_node_id, guint32 audio_node_id)
{
    gchar *pipeline_str = NULL;
    GstBus *bus = NULL;
    GstStateChangeReturn ret;
    GString *p_str;

    p_str = g_string_new ("");

    g_print ("Creating pipeline with audio and video.\n");
    g_string_append (p_str, "matroskamux name=mux ! filesink location=kapture-recording.mkv ");

    /* Video branch */
    g_string_append_printf (p_str, "pipewiresrc do-timestamp=true path=%u ! queue ! videoconvert ! videorate ! video/x-raw,framerate=30/1 ! queue ! x264enc pass=quant quantizer=0 speed-preset=medium ! queue ! mux.video_0 ", video_node_id);

    /* Audio branch */
    if (audio_node_id != 0) {
        g_print ("Using Portal provided audio stream (Node %u).\n", audio_node_id);
        g_string_append_printf (p_str, "pipewiresrc do-timestamp=true path=%u ! queue ! audioconvert ! audiorate ! queue ! avenc_aac ! queue ! mux.audio_0", audio_node_id);
    } else {
        g_print ("No Portal audio stream. Falling back to PulseAudio default source.\n");
        g_string_append (p_str, "pulsesrc ! queue ! audioconvert ! audiorate ! queue ! avenc_aac ! queue ! mux.audio_0");
    }

    pipeline_str = g_string_free (p_str, FALSE);

    g_print ("Starting GStreamer pipeline: %s\n", pipeline_str);
    data->pipeline = gst_parse_launch (pipeline_str, NULL);
    g_free (pipeline_str);

    if (!data->pipeline) {
        g_printerr ("Failed to create GStreamer pipeline.\n");
        reset_ui_and_state (data);
        return;
    }

    bus = gst_element_get_bus (data->pipeline);
    gst_bus_add_watch (bus, (GstBusFunc) on_gst_message, data);
    g_object_unref (bus);

    ret = gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Failed to set pipeline to PLAYING.\n");
        reset_ui_and_state (data);
    }
}

/* Step 3 Response: Handle the signal from the 'Start' request */
static void
on_start_response (GDBusConnection *connection,
                   const gchar *sender_name,
                   const gchar *object_path,
                   const gchar *interface_name,
                   const gchar *signal_name,
                   GVariant *parameters,
                   gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    guint32 response;
    GVariant *results;
    guint32 video_node_id = 0;
    guint32 audio_node_id = 0;

    g_dbus_connection_signal_unsubscribe (connection, data->request_signal_id);
    data->request_signal_id = 0;

    g_variant_get (parameters, "(u@a{sv})", &response, &results);

    if (response != 0) {
        g_printerr ("Start request failed with code %u\n", response);
        reset_ui_and_state (data);
        g_variant_unref (results);
        return;
    }

    GVariant *streams = g_variant_lookup_value (results, "streams", G_VARIANT_TYPE ("a(ua{sv})"));
    if (!streams || g_variant_n_children (streams) == 0) {
        g_printerr ("No streams returned by portal.\n");
        if (streams) g_variant_unref (streams);
        g_variant_unref (results);
        reset_ui_and_state (data);
        return;
    }

    gchar *streams_str = g_variant_print (streams, TRUE);
    g_print ("Portal returned streams: %s\n", streams_str);
    g_free (streams_str);

    GVariantIter iter;
    GVariant *child;
    g_variant_iter_init (&iter, streams);
    while ((child = g_variant_iter_next_value (&iter))) {
        guint32 node_id;
        GVariant *props;
        g_variant_get (child, "(u@a{sv})", &node_id, &props);

        /* Check for video stream by looking for a 'size' property */
        GVariant *size = g_variant_lookup_value (props, "size", G_VARIANT_TYPE ("(ii)"));
        if (size) {
            if (video_node_id == 0) {
                g_print ("Found video stream with node ID: %u\n", node_id);
                video_node_id = node_id;
            } else {
                g_print ("Found another video stream with node ID: %u (ignoring)\n", node_id);
            }
            g_variant_unref (size);
        } else {
            if (audio_node_id == 0) {
                g_print ("Found audio stream with node ID: %u\n", node_id);
                audio_node_id = node_id;
            } else {
                g_print ("Found another audio stream with node ID: %u (ignoring)\n", node_id);
            }
        }
        g_variant_unref (props);
        g_variant_unref (child);
    }

    g_variant_unref (streams);
    g_variant_unref (results);

    if (video_node_id == 0) {
        g_printerr ("Could not find a video stream.\n");
        reset_ui_and_state (data);
        return;
    }

    start_gstreamer_pipeline (data, video_node_id, audio_node_id);
}

/* Step 3 Call: 'Start' method called, waiting for Request handle */
static void
on_start_called (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    GVariant *result;
    GError *error = NULL;

    result = g_dbus_proxy_call_finish (data->portal_proxy, res, &error);

    if (error) {
        g_printerr ("Error calling Start: %s\n", error->message);
        g_clear_error (&error);
        reset_ui_and_state (data);
        return;
    }

    gchar *request_handle = NULL;
    g_variant_get (result, "(o)", &request_handle);
    g_variant_unref (result);

    g_print ("Start method returned request: %s\n", request_handle);

    data->request_signal_id = g_dbus_connection_signal_subscribe (
        g_dbus_proxy_get_connection (data->portal_proxy),
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        request_handle,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        on_start_response,
        data,
        NULL
    );
    g_free (request_handle);
}

/* Step 2 Response: Handle the signal from the 'SelectSources' request */
static void
on_select_sources_response (GDBusConnection *connection,
                            const gchar *sender_name,
                            const gchar *object_path,
                            const gchar *interface_name,
                            const gchar *signal_name,
                            GVariant *parameters,
                            gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    guint32 response;
    GVariant *results;

    g_dbus_connection_signal_unsubscribe (connection, data->request_signal_id);
    data->request_signal_id = 0;

    g_variant_get (parameters, "(u@a{sv})", &response, &results);

    if (response != 0) {
        g_printerr ("SelectSources request failed (cancelled?): %u\n", response);
        reset_ui_and_state (data);
    } else {
        g_print ("Sources selected. Calling Start.\n");
        g_dbus_proxy_call (data->portal_proxy,
                           "Start",
                           g_variant_new ("(os@a{sv})",
                                          data->session_handle,
                                          "",
                                          g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0)),
                           G_DBUS_CALL_FLAGS_NONE, -1, data->cancellable,
                           on_start_called, data);
    }
    g_variant_unref (results);
}

/* Step 2 Call: 'SelectSources' method called, waiting for Request handle */
static void
on_sources_selected (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    GVariant *result;
    GError *error = NULL;

    result = g_dbus_proxy_call_finish (data->portal_proxy, res, &error);

    if (error) {
        g_printerr ("Error calling SelectSources: %s\n", error->message);
        g_clear_error (&error);
        reset_ui_and_state (data);
        return;
    }

    gchar *request_handle = NULL;
    g_variant_get (result, "(o)", &request_handle);
    g_variant_unref (result);

    g_print ("SelectSources returned request: %s\n", request_handle);

    data->request_signal_id = g_dbus_connection_signal_subscribe (
        g_dbus_proxy_get_connection (data->portal_proxy),
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        request_handle,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        on_select_sources_response,
        data,
        NULL
    );
    g_free (request_handle);
}

/* Step 1 Response: Handle the signal from the 'CreateSession' request */
static void
on_create_session_response (GDBusConnection *connection,
                            const gchar *sender_name,
                            const gchar *object_path,
                            const gchar *interface_name,
                            const gchar *signal_name,
                            GVariant *parameters,
                            gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    guint32 response;
    GVariant *results;

    g_dbus_connection_signal_unsubscribe (connection, data->request_signal_id);
    data->request_signal_id = 0;

    g_variant_get (parameters, "(u@a{sv})", &response, &results);

    if (response != 0) {
        g_printerr ("CreateSession request failed: %u\n", response);
        reset_ui_and_state (data);
    } else {
        g_free (data->session_handle);
        g_variant_lookup (results, "session_handle", "s", &data->session_handle);
        g_print ("Session created: %s\n", data->session_handle);

        GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (builder, "{sv}", "multiple", g_variant_new_boolean (FALSE));
        GVariant *options = g_variant_builder_end (builder);

        g_dbus_proxy_call (data->portal_proxy,
                           "SelectSources",
                           g_variant_new ("(o@a{sv})", data->session_handle, options),
                           G_DBUS_CALL_FLAGS_NONE, -1, data->cancellable,
                           on_sources_selected, data);
    }
    g_variant_unref (results);
}

/* Step 1 Call: 'CreateSession' method called, waiting for Request handle */
static void
on_session_created (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    GVariant *result;
    GError *error = NULL;

    result = g_dbus_proxy_call_finish (data->portal_proxy, res, &error);

    if (error) {
        g_printerr ("Error creating portal session: %s\n", error->message);
        g_clear_error (&error);
        reset_ui_and_state (data);
        return;
    }

    gchar *request_handle = NULL;
    g_variant_get (result, "(o)", &request_handle);
    g_variant_unref (result);

    g_print ("CreateSession returned request: %s\n", request_handle);

    data->request_signal_id = g_dbus_connection_signal_subscribe (
        g_dbus_proxy_get_connection (data->portal_proxy),
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        request_handle,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        on_create_session_response,
        data,
        NULL
    );
    g_free (request_handle);
}

/* Initial Setup: Proxy created, calling CreateSession */
static void
on_proxy_created (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    GError *error = NULL;
    data->portal_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);

    if (error) {
        g_printerr ("Error getting portal proxy: %s\n", error->message);
        g_clear_error (&error);
        reset_ui_and_state (data);
        return;
    }

    /* Generate a unique token for the request */
    gchar *token = g_strdup_printf ("u%u", g_random_int ());

    GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (builder, "{sv}", "session_handle_token", g_variant_new_string (token));
    g_variant_builder_add (builder, "{sv}", "handle_token", g_variant_new_string (token));
    GVariant *options = g_variant_builder_end (builder);
    g_free (token);

    g_dbus_proxy_call (data->portal_proxy,
                       "CreateSession",
                       g_variant_new ("(@a{sv})", options),
                       G_DBUS_CALL_FLAGS_NONE, -1, data->cancellable,
                       on_session_created, data);
}

/* "Start Recording" button click handler */
static void
start_recording (GtkButton *button, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    g_print ("Starting recording process...\n");
    gtk_widget_set_sensitive (data->start_button, FALSE);
    gtk_widget_set_sensitive (data->stop_button, TRUE);

    data->cancellable = g_cancellable_new ();

    g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                              "org.freedesktop.portal.Desktop",
                              "/org/freedesktop/portal/desktop",
                              "org.freedesktop.portal.ScreenCast",
                              data->cancellable, (GAsyncReadyCallback) on_proxy_created, data);
}

/* "Stop Recording" button click handler */
static void
stop_recording (GtkButton *button, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    g_print ("Stopping recording...\n");

    if (data->cancellable) {
        g_cancellable_cancel (data->cancellable);
    }

    /* Force stop immediately to prevent UI hangs. */
    reset_ui_and_state (data);
}

/* This function is called when the application is first activated */
static void
activate (GtkApplication *app, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    GtkWidget *box;

    data->window = gtk_application_window_new (app);
    g_signal_connect (data->window, "destroy", G_CALLBACK (on_window_destroy), data);
    gtk_window_set_title (GTK_WINDOW (data->window), "Kapture Screen Recorder");
    gtk_window_set_default_size (GTK_WINDOW (data->window), 300, 100);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start (box, 10);
    gtk_widget_set_margin_end (box, 10);
    gtk_widget_set_margin_top (box, 10);
    gtk_widget_set_margin_bottom (box, 10);
    gtk_window_set_child (GTK_WINDOW (data->window), box);

    data->start_button = gtk_button_new_with_label ("Start Recording");
    g_signal_connect (data->start_button, "clicked", G_CALLBACK (start_recording), data);
    gtk_box_append (GTK_BOX (box), data->start_button);

    data->stop_button = gtk_button_new_with_label ("Stop Recording");
    g_signal_connect (data->stop_button, "clicked", G_CALLBACK (stop_recording), data);
    gtk_widget_set_sensitive (data->stop_button, FALSE);
    gtk_box_append (GTK_BOX (box), data->stop_button);

    gtk_window_present (GTK_WINDOW (data->window));
}

int
main (int argc, char **argv)
{
    GtkApplication *app;
    int status;
    /* Allocate our state struct on the heap and initialize to zero */
    AppData *data = g_new0 (AppData, 1);

    /* Force GStreamer debug level to 3 (INFO) for all categories */
    g_setenv ("GST_DEBUG", "3", TRUE);

    gst_init (&argc, &argv);

    if (!check_gst_plugins (NULL)) {
        g_free (data);
        return -1;
    }

    app = gtk_application_new ("org.kde.kapturescreenrecorder", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "activate", G_CALLBACK (activate), data);
    status = g_application_run (G_APPLICATION (app), argc, argv);

    /* Clean up */
    g_object_unref (app);
    reset_ui_and_state(data); /* Final cleanup of any remaining state */
    g_free (data);

    return status;
}
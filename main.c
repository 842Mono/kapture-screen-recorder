#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gio/gio.h>

/* A struct to hold all our application state */
typedef struct {
    GtkWidget *window;
    GtkWidget *start_button;
    GtkWidget *stop_button;
    GtkWidget *filename_entry;
    GtkWidget *cursor_check;
    GtkWidget *audio_source_combo;
    GtkWidget *quality_combo;
    GtkWidget *framerate_combo;
    GtkWidget *mix_box;
    GtkWidget *manual_box;
    GtkWidget *pipeline_view;
    GtkWidget *pipeline_check;
    GtkWidget *mix_source1_combo;
    GtkWidget *mix_source2_combo;

    GstElement *pipeline;
    GDBusProxy *portal_proxy;
    gchar *session_handle;
    GCancellable *cancellable;
    guint request_signal_id; /* ID for the DBus signal subscription */
    GstDeviceMonitor *device_monitor;
    guint refresh_timeout_id;
    guint monitor_bus_watch_id;

    gchar *default_mic_id;
    gchar *default_monitor_id;
    GHashTable *display_labels;
} AppData;

static void populate_audio_sources(AppData *data);
/* Forward declarations for callbacks */
static void on_start_called (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_sources_selected (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_session_created (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_proxy_created (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_audio_source_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void refresh_audio_devices_ui(AppData *data);
static gboolean monitor_bus_func(GstBus *bus, GstMessage *msg, gpointer user_data);
static gboolean do_audio_refresh(gpointer user_data);

static void on_refresh_audio_clicked(GtkButton *button, gpointer user_data);
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
        {"audiomixer", "gstreamer1.0-plugins-base"},
        {"audioresample", "gstreamer1.0-plugins-base"},
        {"mp4mux", "gstreamer1.0-plugins-good"},
        {"webmmux", "gstreamer1.0-plugins-good"},
        {"vp8enc", "gstreamer1.0-plugins-good"},
        {"vp9enc", "gstreamer1.0-plugins-good"},
        {"vorbisenc", "gstreamer1.0-plugins-base"},
        {"opusenc", "gstreamer1.0-plugins-base"},
        {"avimux", "gstreamer1.0-plugins-good"},
        {"qtmux", "gstreamer1.0-plugins-good"},
        {"jpegenc", "gstreamer1.0-plugins-good"},
        {"avenc_huffyuv", "gstreamer1.0-libav"},
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

        GtkAlertDialog *dialog = gtk_alert_dialog_new ("%s", primary_text);
        gtk_alert_dialog_set_detail (dialog, secondary_text);
        
        /* The g_application_run is not running yet, so we can't use gtk_dialog_run.
         * Instead we make our own main loop to show the dialog.
         */
        gtk_alert_dialog_show(dialog, parent);
        while(g_main_context_iteration(NULL, TRUE));
        
        g_free (primary_text);
        g_free (secondary_text);
        g_object_unref(dialog);
        return FALSE;
    }

    return TRUE;
}

/* Resets the UI and cleans up all recording-related state */
static void
reset_ui_and_state (AppData *data)
{
    g_print ("Resetting UI and state.\n");

    if (data->start_button) gtk_widget_set_sensitive (data->start_button, TRUE);
    if (data->stop_button) gtk_widget_set_sensitive (data->stop_button, FALSE);
    if (data->filename_entry) gtk_widget_set_sensitive (data->filename_entry, TRUE);
    if (data->audio_source_combo) gtk_widget_set_sensitive (data->audio_source_combo, TRUE);
    if (data->mix_source1_combo) gtk_widget_set_sensitive (data->mix_source1_combo, TRUE);
    if (data->mix_source2_combo) gtk_widget_set_sensitive (data->mix_source2_combo, TRUE);
    if (data->cursor_check) gtk_widget_set_sensitive (data->cursor_check, TRUE);
    if (data->quality_combo) gtk_widget_set_sensitive (data->quality_combo, TRUE);
    if (data->framerate_combo) gtk_widget_set_sensitive (data->framerate_combo, TRUE);
    if (data->pipeline_check) gtk_widget_set_sensitive (data->pipeline_check, TRUE);
    if (data->pipeline_view) gtk_text_view_set_editable(GTK_TEXT_VIEW(data->pipeline_view), TRUE);

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

/* Helper function to construct a pipeline string based on UI settings */
static gchar*
build_pipeline_string(AppData *data, const gchar *video_node_str, guint32 portal_audio_node_id, const gchar **ext_out)
{
    GString *p_str;
    const gchar *selected_audio_id = gtk_string_list_get_string(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->audio_source_combo))), gtk_drop_down_get_selected(GTK_DROP_DOWN(data->audio_source_combo)));

    /* Determine Video Quality Settings */
    const gchar *q_id = gtk_string_list_get_string(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->quality_combo))), gtk_drop_down_get_selected(GTK_DROP_DOWN(data->quality_combo)));

    /* Determine Frame Rate */
    const gchar *fps_id = gtk_string_list_get_string(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->framerate_combo))), gtk_drop_down_get_selected(GTK_DROP_DOWN(data->framerate_combo)));
    const gchar *framerate = fps_id ? fps_id : "30";

    /* Use a large queue (2GB) to buffer video in RAM, helping with high-bandwidth formats or slow disks */
    const gchar *v_queue = "queue max-size-bytes=2147483648 max-size-buffers=0 max-size-time=0";

    p_str = g_string_new ("");

    /* Defaults for MKV High */
    const gchar *muxer = "matroskamux";
    const gchar *video_enc = "x264enc pass=qual quantizer=20 speed-preset=medium";
    const gchar *audio_enc = "avenc_aac";
    const gchar *ext = "mkv";

    if (g_strcmp0(q_id, "mkv_lossless") == 0) {
        video_enc = "x264enc pass=quant quantizer=0 speed-preset=medium";
    } else if (g_strcmp0(q_id, "mkv_low") == 0) {
        video_enc = "x264enc pass=qual quantizer=35 speed-preset=medium";
    } else if (g_strcmp0(q_id, "mkv_raw") == 0) {
        video_enc = "videoconvert ! video/x-raw,format=I420"; /* Raw Uncompressed YUV */
        audio_enc = "audioconvert ! audio/x-raw";  /* PCM Audio */
    } else if (g_strcmp0(q_id, "mp4_high") == 0) {
        muxer = "mp4mux";
        ext = "mp4";
    } else if (g_strcmp0(q_id, "mp4_low") == 0) {
        muxer = "mp4mux";
        video_enc = "x264enc pass=qual quantizer=35 speed-preset=medium";
        ext = "mp4";
    } else if (g_strcmp0(q_id, "webm_vp9") == 0) {
        muxer = "webmmux";
        video_enc = "vp9enc deadline=1 cpu-used=8 row-mt=true"; /* Faster encoding */
        audio_enc = "opusenc";
        ext = "webm";
    } else if (g_strcmp0(q_id, "webm_vp8") == 0) {
        muxer = "webmmux";
        video_enc = "vp8enc deadline=1 cpu-used=4"; /* VP8 is generally faster but less efficient */
        audio_enc = "vorbisenc";
        ext = "webm";
    } else if (g_strcmp0(q_id, "mov_high") == 0) {
        muxer = "qtmux";
        ext = "mov";
    } else if (g_strcmp0(q_id, "avi_raw") == 0) {
        muxer = "avimux";
        video_enc = "videoconvert ! video/x-raw,format=BGR"; /* Raw Uncompressed RGB */
        audio_enc = "audioconvert ! audio/x-raw"; /* PCM Audio */
        ext = "avi";
    } else if (g_strcmp0(q_id, "avi_huffyuv") == 0) {
        muxer = "avimux";
        video_enc = "videoconvert ! avenc_huffyuv"; /* Lossless Compression */
        audio_enc = "audioconvert ! audio/x-raw"; /* PCM Audio */
        ext = "avi";
    }

    /* Determine Audio Strategy */
    const gchar *effective_audio_id = selected_audio_id;

    gboolean enable_audio = TRUE;
    if (effective_audio_id == NULL || g_strcmp0(effective_audio_id, "none") == 0) {
        enable_audio = FALSE;
    }

    if (ext_out) *ext_out = ext;

    if (enable_audio) {
        g_print ("Creating pipeline with audio and video.\n");
        g_string_append_printf (p_str, "%s name=mux ! filesink name=filesink location=dummy.%s ", muxer, ext);

        /* Video branch for muxer */
        g_string_append_printf (p_str, "pipewiresrc do-timestamp=true path=%s ! %s ! videoconvert ! videorate ! video/x-raw,framerate=%s/1 ! %s ! %s ! %s ! mux.video_0 ", video_node_str, v_queue, framerate, v_queue, video_enc, v_queue);

        /* Audio branch for muxer */
        if (g_strcmp0(effective_audio_id, "custom_mix") == 0) {
            g_print ("Audio Strategy: Mixing Two Sources.\n");
            
            const gchar *src1_id = gtk_string_list_get_string(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->mix_source1_combo))), gtk_drop_down_get_selected(GTK_DROP_DOWN(data->mix_source1_combo)));
            const gchar *src2_id = gtk_string_list_get_string(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->mix_source2_combo))), gtk_drop_down_get_selected(GTK_DROP_DOWN(data->mix_source2_combo)));
            
            if (!src1_id) src1_id = data->default_mic_id;
            if (!src2_id) src2_id = data->default_monitor_id;

            gchar **mic_parts = src1_id ? g_strsplit(src1_id, ":", 2) : NULL;
            gchar **mon_parts = src2_id ? g_strsplit(src2_id, ":", 2) : NULL;

            if (mic_parts && mic_parts[0] && mic_parts[1] && mon_parts && mon_parts[0] && mon_parts[1]) {
                g_string_append_printf (p_str, "audiomixer name=mix latency=200000000 ! queue max-size-time=3000000000 max-size-bytes=0 max-size-buffers=0 ! audioconvert ! audioresample ! queue ! %s ! queue ! mux.audio_0 ", audio_enc);
                
                g_string_append_printf (p_str, "pulsesrc name=mic_src do-timestamp=true provide-clock=false device=\"%s\" ! queue max-size-time=3000000000 max-size-bytes=0 max-size-buffers=0 ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! audiorate tolerance=100000000 ! queue ! mix. ", mic_parts[1]);

                g_string_append_printf (p_str, "pulsesrc name=monitor_src do-timestamp=true provide-clock=false device=\"%s\" ! queue max-size-time=3000000000 max-size-bytes=0 max-size-buffers=0 ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! audiorate tolerance=100000000 ! queue ! mix. ", mon_parts[1]);
            } else {
                g_print ("Warning: Mix selected but source IDs are missing.\n");
                /* Fallback to silence/dummy audio to keep pipeline valid? Or just fail gracefully. 
                   For now, let's just append a dummy silence source to avoid syntax error in pipeline parsing */
                g_string_append_printf (p_str, "audiotestsrc wave=silence ! audioconvert ! %s ! mux.audio_0 ", audio_enc);
            }
            if (mic_parts) g_strfreev(mic_parts);
            if (mon_parts) g_strfreev(mon_parts);
        } else if (g_strcmp0(effective_audio_id, "portal") == 0) {
            if (portal_audio_node_id != 0) {
                g_print ("Using Portal provided audio stream (Node %u).\n", portal_audio_node_id);
                g_string_append_printf (p_str, "pipewiresrc do-timestamp=true path=%u ! queue max-size-time=3000000000 max-size-bytes=0 max-size-buffers=0 ! audioconvert ! audiorate ! queue ! %s ! queue ! mux.audio_0", portal_audio_node_id, audio_enc);
            } else {
                g_print ("Portal audio selected but not provided. Falling back to default PulseAudio source.\n");
                g_string_append_printf (p_str, "pulsesrc name=audiosrc do-timestamp=true provide-clock=false ! queue max-size-time=3000000000 max-size-bytes=0 max-size-buffers=0 ! audioconvert ! audiorate ! queue ! %s ! queue ! mux.audio_0", audio_enc);
            }
        } else {
            /* A specific device was selected */
            g_print("Using selected device: %s\n", effective_audio_id);
            gchar **parts = g_strsplit(effective_audio_id, ":", 2);
            if (parts[0] && parts[1]) {
                g_string_append_printf (p_str, "pulsesrc name=audiosrc do-timestamp=true provide-clock=false device=\"%s\" ! queue max-size-time=3000000000 max-size-bytes=0 max-size-buffers=0 ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! audiorate tolerance=100000000 ! queue ! %s ! queue ! mux.audio_0", parts[1], audio_enc);
            }
            g_strfreev(parts);
        }
    } else {
        /* Video only pipeline */
        g_print ("Creating pipeline with video only.\n");
        g_string_append_printf (p_str, "pipewiresrc do-timestamp=true path=%s ! "
            "%s ! videoconvert ! videorate ! video/x-raw,framerate=%s/1 ! "
            "%s ! %s ! "
            "%s name=mux ! filesink name=filesink location=dummy.%s",
            video_node_str, v_queue, framerate, v_queue, video_enc, muxer, ext);
    }

    return g_string_free (p_str, FALSE);
}

/* Constructs and starts the GStreamer pipeline with the given PipeWire node IDs */
static void
start_gstreamer_pipeline (AppData *data, guint32 video_node_id, guint32 audio_node_id)
{
    GstBus *bus = NULL;
    GstStateChangeReturn ret;

    const gchar *ext = NULL;
    gchar *video_node_id_str = g_strdup_printf("%u", video_node_id);
    gchar *pipeline_template = build_pipeline_string(data, video_node_id_str, audio_node_id, &ext);
    g_free(video_node_id_str);

    /* Generate a unique filename */
    const gchar *base_name = gtk_editable_get_text(GTK_EDITABLE(data->filename_entry));
    if (!base_name || *base_name == '\0') {
        base_name = "Kapture Recording";
    }
    gchar *videos_dir = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS));
    if (!videos_dir) {
        videos_dir = g_strdup(g_get_home_dir());
    }

    gchar *final_path = g_build_filename(videos_dir, g_strdup_printf("%s.%s", base_name, ext), NULL);
    int counter = 2;
    while (g_file_test(final_path, G_FILE_TEST_EXISTS)) {
        g_free(final_path);
        final_path = g_build_filename(videos_dir, g_strdup_printf("%s (%d).%s", base_name, counter++, ext), NULL);
    }
    g_free(videos_dir);
    
    if (!pipeline_template) {
        g_printerr("Failed to generate a pipeline string.\n");
        g_free(final_path);
        reset_ui_and_state(data);
        return;
    }
    data->pipeline = gst_parse_launch (pipeline_template, NULL);
    g_free (pipeline_template);

    if (!data->pipeline) {
        g_printerr ("Failed to create GStreamer pipeline.\n");
        g_free(final_path);
        reset_ui_and_state (data);
        return;
    }

    GstElement *filesink = gst_bin_get_by_name(GST_BIN(data->pipeline), "filesink");
    if (filesink) {
        g_print("Setting output file to: %s\n", final_path);
        g_object_set(filesink, "location", final_path, NULL);
        g_object_unref(filesink);
    } else {
        g_printerr("Could not find 'filesink' element in the pipeline!\n");
        g_free(final_path);
        reset_ui_and_state(data);
        return;
    }
    g_free(final_path);


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
        gchar *token = g_strdup_printf ("u%u", g_random_int ());
        GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (builder, "{sv}", "handle_token", g_variant_new_string (token));
        GVariant *options = g_variant_builder_end (builder);
        g_free (token);

        g_dbus_proxy_call (data->portal_proxy,
                           "Start",
                           g_variant_new ("(os@a{sv})",
                                          data->session_handle,
                                          "",
                                          options),
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

        gboolean show_cursor = gtk_check_button_get_active(GTK_CHECK_BUTTON(data->cursor_check));
        guint cursor_mode = show_cursor ? 2 : 1; /* 2 = Embedded (Visible), 1 = Hidden */

        gchar *token = g_strdup_printf ("u%u", g_random_int ());
        GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        g_variant_builder_add (builder, "{sv}", "handle_token", g_variant_new_string (token));
        g_variant_builder_add (builder, "{sv}", "multiple", g_variant_new_boolean (FALSE));
        g_variant_builder_add (builder, "{sv}", "cursor_mode", g_variant_new_uint32 (cursor_mode));
        GVariant *options = g_variant_builder_end (builder);
        g_free (token);

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
    gtk_widget_set_sensitive (data->filename_entry, FALSE);
    gtk_widget_set_sensitive (data->cursor_check, FALSE);
    gtk_widget_set_sensitive (data->audio_source_combo, FALSE);
    gtk_widget_set_sensitive (data->mix_source1_combo, FALSE);
    gtk_widget_set_sensitive (data->mix_source2_combo, FALSE);
    gtk_widget_set_sensitive (data->quality_combo, FALSE);
    gtk_widget_set_sensitive (data->framerate_combo, FALSE);
    gtk_widget_set_sensitive (data->pipeline_check, FALSE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(data->pipeline_view), FALSE);

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
        /* Cancel any pending portal operations, like waiting for a dialog */
        g_cancellable_cancel (data->cancellable);
    }

    if (data->pipeline) {
        g_print("Sending End-of-Stream to pipeline...\n");
        /* Disable button to prevent multiple clicks while we wait for the EOS message */
        gtk_widget_set_sensitive(data->stop_button, FALSE);
        gst_element_send_event(data->pipeline, gst_event_new_eos());
    } else {
        /* If pipeline never started but we clicked stop (e.g. cancelled dialog) */
        reset_ui_and_state (data);
    }
}

/* Factory setup callback to create a label for each dropdown item */
static void
on_item_setup (GtkSignalListItemFactory *factory, GtkListItem *list_item)
{
  GtkWidget *label = gtk_label_new ("");
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_list_item_set_child (list_item, label);
}

/* Factory bind callback to set the text of the label */
static void
on_item_bind (GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    AppData *data = user_data;
    GtkLabel *label = GTK_LABEL (gtk_list_item_get_child (list_item));
    GtkStringObject *string_object = GTK_STRING_OBJECT(gtk_list_item_get_item(list_item));
    const char *id = gtk_string_object_get_string(string_object);

    const char *display_text = g_hash_table_lookup (data->display_labels, id);
    gtk_label_set_text (label, display_text ? display_text : id);
}

/* Helper to find the position of a string in a GtkStringList model */
static gboolean
find_string_in_model(GtkStringList *model, const gchar *str, guint *pos)
{
    if (!str || !model) {
        return FALSE;
    }

    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(model));
    for (guint i = 0; i < n_items; i++) {
        const gchar *item_str = gtk_string_list_get_string(model, i);
        if (g_strcmp0(item_str, str) == 0) {
            if (pos) {
                *pos = i;
            }
            return TRUE;
        }
    }
    return FALSE;
}

/* The core logic to refresh the audio device lists in the UI */
static void
refresh_audio_devices_ui(AppData *data)
{
    g_print("Refreshing audio devices UI...\n");

    /* Get models */
    GtkStringList *audio_model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->audio_source_combo)));
    GtkStringList *mix1_model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->mix_source1_combo)));
    GtkStringList *mix2_model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->mix_source2_combo)));

    /* Store current selections to try and restore them */
    const gchar *selected_audio = gtk_string_list_get_string(audio_model, gtk_drop_down_get_selected(GTK_DROP_DOWN(data->audio_source_combo)));
    gchar *stored_audio = g_strdup(selected_audio);
    const gchar *selected_mix1 = gtk_string_list_get_string(mix1_model, gtk_drop_down_get_selected(GTK_DROP_DOWN(data->mix_source1_combo)));
    gchar *stored_mix1 = g_strdup(selected_mix1);
    const gchar *selected_mix2 = gtk_string_list_get_string(mix2_model, gtk_drop_down_get_selected(GTK_DROP_DOWN(data->mix_source2_combo)));
    gchar *stored_mix2 = g_strdup(selected_mix2);

    /* Clear models */
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(audio_model));
    if (n_items > 0) gtk_string_list_splice(audio_model, 0, n_items, NULL);

    n_items = g_list_model_get_n_items(G_LIST_MODEL(mix1_model));
    if (n_items > 0) gtk_string_list_splice(mix1_model, 0, n_items, NULL);

    n_items = g_list_model_get_n_items(G_LIST_MODEL(mix2_model));
    if (n_items > 0) gtk_string_list_splice(mix2_model, 0, n_items, NULL);

    /* Clear state that will be repopulated */
    g_hash_table_remove_all(data->display_labels);
    g_clear_pointer(&data->default_mic_id, g_free);
    g_clear_pointer(&data->default_monitor_id, g_free);

    /* Repopulate data from the device monitor */
    populate_audio_sources(data);

    /* Try to restore previous selections */
    guint pos;
    if (find_string_in_model(audio_model, stored_audio, &pos)) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(data->audio_source_combo), pos);
    }
    if (find_string_in_model(mix1_model, stored_mix1, &pos)) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(data->mix_source1_combo), pos);
    }
    if (find_string_in_model(mix2_model, stored_mix2, &pos)) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(data->mix_source2_combo), pos);
    }

    g_free(stored_audio);
    g_free(stored_mix1);
    g_free(stored_mix2);

    /* Re-trigger UI updates based on new state */
    on_audio_source_changed(G_OBJECT(data->audio_source_combo), NULL, data);
}

/* "Refresh Audio" button click handler */
static void
on_refresh_audio_clicked(GtkButton *button, gpointer user_data)
{
    refresh_audio_devices_ui((AppData *)user_data);
}

/* Populates the audio source combo box with devices found by GStreamer */
static void
populate_audio_sources(AppData *data) {
    if (!data->device_monitor) {
        g_printerr("Device monitor not initialized!\n");
        return;
    }

    /* Add special items first */
    g_hash_table_insert(data->display_labels, g_strdup("portal"), g_strdup("Portal Provided Audio"));
    g_hash_table_insert(data->display_labels, g_strdup("none"), g_strdup("No Audio"));

    GtkStringList *audio_model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->audio_source_combo)));
    gtk_string_list_append(audio_model, "portal");
    gtk_string_list_append(audio_model, "none");

    GtkStringList *mix1_model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->mix_source1_combo)));
    GtkStringList *mix2_model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->mix_source2_combo)));

    GList *devices = gst_device_monitor_get_devices(data->device_monitor);
    for (GList *l = devices; l != NULL; l = l->next) {
        GstDevice *device = l->data;
        gchar *name = gst_device_get_display_name(device);
        
        /* Check element factory to filter out raw ALSA devices which confuse pulsesrc */
        GstElement *element = gst_device_create_element(device, NULL);
        gchar *factory_name = NULL;
        gchar *element_device_id = NULL;
        if (element) {
            GstElementFactory *factory = gst_element_get_factory(element);
            if (factory) factory_name = g_strdup(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)));
            
            if (g_strcmp0(factory_name, "pulsesrc") == 0) {
                g_object_get(element, "device", &element_device_id, NULL);
            }
            gst_object_unref(element);
        }

        /* Allow only pulsesrc. pipewiresrc IDs from device monitor are often not usable paths. */
        if (g_strcmp0(factory_name, "pulsesrc") != 0) {
            g_free(factory_name);
            g_free(element_device_id);
            g_free(name);
            continue;
        }

        GstStructure *props = gst_device_get_properties(device);
        if (props) {
            const gchar *device_id = gst_structure_get_string(props, "device.id");
            const gchar *device_class = gst_structure_get_string(props, "device.class");
            g_print ("Detected Device: %s [Class: %s] [Factory: %s]\n", name, device_class ? device_class : "N/A", factory_name ? factory_name : "Unknown");

            const gchar *use_id = element_device_id ? element_device_id : device_id;
            gchar *full_id = g_strdup_printf("%s:%s", factory_name, use_id);
            gchar *label = NULL;
            if (g_strcmp0(device_class, "monitor") == 0 || g_str_has_prefix(name, "Monitor of ")) {
                const gchar *disp_name = name;
                if (g_str_has_prefix(name, "Monitor of ")) {
                    disp_name += 11; /* Length of "Monitor of " */
                }
                label = g_strdup_printf("System Audio (%s)", disp_name);
                
                /* Capture the first monitor found as default for simple mode */
                if (!data->default_monitor_id) { 
                    data->default_monitor_id = g_strdup(full_id);
                }
            } else {
                label = g_strdup(name);
                
                /* Capture the first non-monitor (mic) found as default for simple mode */
                if (!data->default_mic_id) {
                    data->default_mic_id = g_strdup(full_id);
                }
            }
            g_hash_table_insert(data->display_labels, g_strdup(full_id), g_strdup(label));
            gtk_string_list_append(audio_model, full_id);
            gtk_string_list_append(mix1_model, full_id);
            gtk_string_list_append(mix2_model, full_id);
            g_free(label);
            g_free(full_id);
            gst_structure_free(props);
        }
        g_free(factory_name);
        g_free(element_device_id);
        g_free(name);
    }

    /* Always add the Mix option */
    g_hash_table_insert(data->display_labels, g_strdup("custom_mix"), g_strdup("Mix (For Recording Microphone + Speakers)"));
    gtk_string_list_append(audio_model, "custom_mix");

    /* Set defaults for mix combos */
    if (data->default_mic_id) {
        guint pos;
        if (find_string_in_model(mix1_model, data->default_mic_id, &pos)) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(data->mix_source1_combo), pos);
        }
    }
    if (data->default_monitor_id) {
        guint pos;
        if (find_string_in_model(mix2_model, data->default_monitor_id, &pos)) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(data->mix_source2_combo), pos);
        }
    }

    /* Set a default selection */
    gtk_drop_down_set_selected(GTK_DROP_DOWN(data->audio_source_combo), 0); /* "Portal Provided Audio" */

    g_list_free_full(devices, g_object_unref);
}

static void update_pipeline_display(AppData *data) {
    gchar *default_pipeline = build_pipeline_string(data, "VIDEO_NODE_ID", 0, NULL);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(data->pipeline_view));
    gtk_text_buffer_set_text(buffer, default_pipeline, -1);
    g_free(default_pipeline);
}

/* Callback for any dropdown change */
static void on_setting_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    update_pipeline_display((AppData *)user_data);
}

/* Callback for audio source combo change */
static void on_audio_source_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    AppData *data = (AppData *)user_data;
    const gchar *id = gtk_string_list_get_string(GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(dropdown))), gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown)));
    gboolean is_mix = (g_strcmp0(id, "custom_mix") == 0);
    gtk_widget_set_visible(data->mix_box, is_mix);
    update_pipeline_display(data);
}

/* Callback for pipeline editor checkbox */
static void
on_pipeline_check_toggled (GtkCheckButton *button, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    gboolean active = gtk_check_button_get_active (button);
    gtk_widget_set_visible (data->manual_box, active);
    if (!active) {
        int current_width = gtk_widget_get_width(data->window);
        gtk_window_set_default_size(GTK_WINDOW(data->window), current_width, -1);
    }
}

/* Debounced callback to refresh the UI when the device list changes */
static gboolean
do_audio_refresh(gpointer user_data)
{
    AppData *data = (AppData *)user_data;
    refresh_audio_devices_ui(data);
    data->refresh_timeout_id = 0;
    return G_SOURCE_REMOVE; /* Run only once */
}

/* Final cleanup function connected to the application's shutdown signal */
static void
on_app_shutdown(GtkApplication *app, gpointer user_data)
{
    AppData *data = (AppData *)user_data;
    g_print("Application shutting down. Final cleanup.\n");

    if (data->monitor_bus_watch_id > 0) {
        g_source_remove(data->monitor_bus_watch_id);
        data->monitor_bus_watch_id = 0;
    }

    /* Stop the device monitor */
    if (data->device_monitor) {
        gst_device_monitor_stop(data->device_monitor);
        g_clear_object(&data->device_monitor);
    }

    /* Free all remaining allocated memory */
    g_hash_table_destroy(data->display_labels);
    g_free(data->default_mic_id);
    g_free(data->default_monitor_id);
    g_free(data->session_handle);
    g_free(data);
}

/* Bus watch function to detect device changes */
static gboolean
monitor_bus_func(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    AppData *data = (AppData *)user_data;
    
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_DEVICE_ADDED:
        case GST_MESSAGE_DEVICE_REMOVED:
            g_print("Device change detected on bus, scheduling refresh...\n");
            if (data->refresh_timeout_id > 0) {
                g_source_remove(data->refresh_timeout_id);
            }
            data->refresh_timeout_id = g_timeout_add(500, do_audio_refresh, data);
            break;
        default:
            break;
    }
    return TRUE;
}

/* This function is called when the application is first activated */
static void
activate (GtkApplication *app, gpointer user_data)
{
    AppData *data = (AppData *) user_data;
    GtkWidget *box;

    data->window = gtk_application_window_new (app);
    /* The main application shutdown signal is now used for cleanup */
    gtk_window_set_title (GTK_WINDOW (data->window), "Kapture Screen Recorder");

    gtk_window_set_default_size (GTK_WINDOW (data->window), 350, -1); /* -1 for auto height */

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start (box, 10);
    gtk_widget_set_margin_end (box, 10);
    gtk_widget_set_margin_top (box, 10);
    gtk_widget_set_margin_bottom (box, 10);
    gtk_window_set_child (GTK_WINDOW (data->window), box);

    /* Cursor Toggle */
    data->cursor_check = gtk_check_button_new_with_label ("Record Mouse Cursor");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (data->cursor_check), TRUE); /* Default to visible */
    gtk_box_append (GTK_BOX (box), data->cursor_check);

    GtkWidget *filename_label = gtk_label_new("Filename:");
    gtk_widget_set_halign(filename_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), filename_label);

    data->filename_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(data->filename_entry), "Kapture Recording");
    gtk_box_append(GTK_BOX(box), data->filename_entry);

    /* --- Audio Header Box (Label + Refresh Button) --- */
    GtkWidget *audio_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(box), audio_header_box);

    GtkWidget *audio_label = gtk_label_new ("Audio Source:");
    gtk_widget_set_halign (audio_label, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (audio_header_box), audio_label);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(audio_header_box), spacer);

    GtkWidget *refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_button, "Refresh Audio Devices");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_audio_clicked), data);
    gtk_box_append(GTK_BOX(audio_header_box), refresh_button);

    /* Create a factory for our dropdowns to use custom labels */
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (on_item_setup), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (on_item_bind), data);

    data->audio_source_combo = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(data->audio_source_combo), factory);
    g_signal_connect (data->audio_source_combo, "notify::selected", G_CALLBACK (on_audio_source_changed), data);
    gtk_box_append (GTK_BOX (box), data->audio_source_combo);

    /* Mix selection box (hidden by default) */
    data->mix_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start (data->mix_box, 10); /* Indent */
    gtk_box_append (GTK_BOX (box), data->mix_box);
    
    GtkWidget *mix_note = gtk_label_new ("Select two sources to record simultaneously (e.g. Microphone and System Audio).");
    gtk_label_set_wrap (GTK_LABEL (mix_note), TRUE);
    gtk_widget_set_halign (mix_note, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (data->mix_box), mix_note);

    GtkWidget *mix1_label = gtk_label_new ("Source 1:");
    gtk_widget_set_halign (mix1_label, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (data->mix_box), mix1_label);
    data->mix_source1_combo = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(data->mix_source1_combo), factory);
    g_signal_connect(data->mix_source1_combo, "notify::selected", G_CALLBACK(on_setting_changed), data);
    gtk_box_append (GTK_BOX (data->mix_box), data->mix_source1_combo);

    GtkWidget *mix2_label = gtk_label_new ("Source 2:");
    gtk_widget_set_halign (mix2_label, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (data->mix_box), mix2_label);
    data->mix_source2_combo = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(data->mix_source2_combo), factory);
    g_signal_connect(data->mix_source2_combo, "notify::selected", G_CALLBACK(on_setting_changed), data);
    gtk_box_append (GTK_BOX (data->mix_box), data->mix_source2_combo);

    GtkWidget *quality_label = gtk_label_new ("Video Quality:");
    gtk_widget_set_halign (quality_label, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (box), quality_label);
    
    g_hash_table_insert(data->display_labels, g_strdup("mkv_high"), g_strdup("MKV - High Quality (H.264/AAC)"));
    g_hash_table_insert(data->display_labels, g_strdup("mkv_lossless"), g_strdup("MKV - Lossless (Recommended)"));
    g_hash_table_insert(data->display_labels, g_strdup("mkv_low"), g_strdup("MKV - Low Quality (H.264/AAC)"));
    g_hash_table_insert(data->display_labels, g_strdup("mkv_raw"), g_strdup("MKV - Raw Uncompressed (Huge File!)"));
    g_hash_table_insert(data->display_labels, g_strdup("mp4_high"), g_strdup("MP4 - High Quality (H.264/AAC)"));
    g_hash_table_insert(data->display_labels, g_strdup("mp4_low"), g_strdup("MP4 - Low Quality (H.264/AAC)"));
    g_hash_table_insert(data->display_labels, g_strdup("webm_vp9"), g_strdup("WebM - High Quality (VP9/Opus)"));
    g_hash_table_insert(data->display_labels, g_strdup("webm_vp8"), g_strdup("WebM - Standard (VP8/Vorbis)"));
    g_hash_table_insert(data->display_labels, g_strdup("mov_high"), g_strdup("MOV - High Quality (H.264/AAC)"));
    g_hash_table_insert(data->display_labels, g_strdup("avi_raw"), g_strdup("AVI - Raw RGB (Huge File, May Not Work Well at High FPS Selections)"));
    g_hash_table_insert(data->display_labels, g_strdup("avi_huffyuv"), g_strdup("AVI - HuffYUV (Huge File, Lossless, May Not Work Well at High FPS Selections)"));

    const char* quality_strings[] = {
        "mkv_lossless", "mkv_high", "mkv_low", "mkv_raw",
        "mp4_high", "mp4_low",
        "webm_vp9", "webm_vp8",
        "mov_high", "avi_raw", "avi_huffyuv",
        NULL
    };
    data->quality_combo = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(quality_strings)), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(data->quality_combo), factory);
    g_signal_connect(data->quality_combo, "notify::selected", G_CALLBACK(on_setting_changed), data);
    gtk_box_append (GTK_BOX (box), data->quality_combo);

    GtkWidget *fps_label = gtk_label_new ("Frame Rate:");
    gtk_widget_set_halign (fps_label, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (box), fps_label);

    g_hash_table_insert(data->display_labels, g_strdup("30"), g_strdup("30 FPS"));
    g_hash_table_insert(data->display_labels, g_strdup("40"), g_strdup("40 FPS"));
    g_hash_table_insert(data->display_labels, g_strdup("50"), g_strdup("50 FPS"));
    g_hash_table_insert(data->display_labels, g_strdup("60"), g_strdup("60 FPS"));
    const char* fps_strings[] = {"30", "40", "50", "60", NULL};
    data->framerate_combo = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(fps_strings)), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(data->framerate_combo), factory);
    g_signal_connect(data->framerate_combo, "notify::selected", G_CALLBACK(on_setting_changed), data);
    gtk_box_append (GTK_BOX (box), data->framerate_combo);

    data->pipeline_check = gtk_check_button_new_with_label("Show Pipeline Editor");
    g_signal_connect(data->pipeline_check, "toggled", G_CALLBACK(on_pipeline_check_toggled), data);
    gtk_box_append(GTK_BOX(box), data->pipeline_check);

    data->manual_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_visible(data->manual_box, FALSE);
    gtk_box_append(GTK_BOX(box), data->manual_box);

    GtkWidget *manual_label = gtk_label_new("The GStreamer pipeline is generated from your selections. You can edit it directly for advanced control.");
    gtk_label_set_wrap(GTK_LABEL(manual_label), TRUE);
    gtk_widget_set_halign(manual_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(data->manual_box), manual_label);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled_window, -1, 150);
    data->pipeline_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(data->pipeline_view), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), data->pipeline_view);
    gtk_box_append(GTK_BOX(data->manual_box), scrolled_window);

    g_object_unref(factory);

    data->start_button = gtk_button_new_with_label ("Start Recording");
    g_signal_connect (data->start_button, "clicked", G_CALLBACK (start_recording), data);
    gtk_box_append (GTK_BOX (box), data->start_button);

    data->stop_button = gtk_button_new_with_label ("Stop Recording");
    g_signal_connect (data->stop_button, "clicked", G_CALLBACK (stop_recording), data);
    gtk_widget_set_sensitive (data->stop_button, FALSE);
    gtk_box_append (GTK_BOX (box), data->stop_button);

    /* Set initial selections now that all widgets are created */
    gtk_drop_down_set_selected(GTK_DROP_DOWN(data->quality_combo), 0);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(data->framerate_combo), 3); /* Default to 60 FPS */

    /* Create and start a persistent device monitor for automatic refreshes */
    data->device_monitor = gst_device_monitor_new();
    
    GstBus *bus = gst_device_monitor_get_bus(data->device_monitor);
    data->monitor_bus_watch_id = gst_bus_add_watch(bus, monitor_bus_func, data);
    gst_object_unref(bus);

    gst_device_monitor_set_show_all_devices(data->device_monitor, TRUE);
    gst_device_monitor_add_filter(data->device_monitor, NULL, NULL);
    gst_device_monitor_start(data->device_monitor);

    /* Populate audio sources now that all widgets are created */
    populate_audio_sources(data);

    /* Trigger visibility update and initial pipeline string generation */
    on_audio_source_changed(G_OBJECT(data->audio_source_combo), NULL, data);

    gtk_window_present (GTK_WINDOW (data->window));
}

int
main (int argc, char **argv)
{
    GtkApplication *app;
    int status;
    /* Allocate our state struct on the heap and initialize to zero */
    AppData *data = g_new0 (AppData, 1);
    data->display_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    /* Force GStreamer debug level to 3 (INFO) for all categories */
    g_setenv ("GST_DEBUG", "3", TRUE);

    gst_init (&argc, &argv);

    if (!check_gst_plugins (NULL)) {
        g_free (data);
        return -1;
    }

    app = gtk_application_new ("io.github.842mono.kapture", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "shutdown", G_CALLBACK (on_app_shutdown), data);
    g_signal_connect (app, "activate", G_CALLBACK (activate), data);
    status = g_application_run (G_APPLICATION (app), argc, argv);

    g_object_unref (app);

    return status;
}
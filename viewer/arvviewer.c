#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/interfaces/xoverlay.h>
#include <gdk/gdkx.h>
#include <arv.h>
#include <stdlib.h>

typedef struct {
	ArvCamera *camera;
	ArvDevice *device;
	ArvStream *stream;

	GstElement *pipeline;
	GstElement *appsrc;

	guint64 timestamp_offset;
	guint64 last_timestamp;

	GtkWidget *main_window;
	GtkWidget *drawing_area;
	GtkWidget *camera_combo_box;
} ArvViewer;

void
arv_viewer_update_device_list_cb (ArvViewer *viewer)
{
	GtkListStore *list_store;
	GtkTreeIter iter;
	unsigned int n_devices;
	unsigned int i;

	list_store = gtk_list_store_new (1, G_TYPE_STRING);
	gtk_combo_box_set_model (GTK_COMBO_BOX (viewer->camera_combo_box), GTK_TREE_MODEL (list_store));
	arv_update_device_list ();
	n_devices = arv_get_n_devices ();
	for (i = 0; i < n_devices; i++) {
		gtk_list_store_append (list_store, &iter);
		gtk_list_store_set (list_store, &iter, 0, arv_get_device_id (i), -1);
	}
	if (n_devices > 0)
		gtk_combo_box_set_active (GTK_COMBO_BOX (viewer->camera_combo_box), 0);
	if (n_devices <= 1)
		gtk_widget_set_sensitive (viewer->camera_combo_box, FALSE);
}

void
arv_viewer_new_buffer_cb (ArvStream *stream, ArvViewer *viewer)
{
	ArvBuffer *arv_buffer;
	GstBuffer *buffer;

	arv_buffer = arv_stream_pop_buffer (stream);
	if (arv_buffer == NULL)
		return;

	if (arv_buffer->status == ARV_BUFFER_STATUS_SUCCESS) {
		buffer = gst_buffer_new ();

		GST_BUFFER_DATA (buffer) = arv_buffer->data;
		GST_BUFFER_MALLOCDATA (buffer) = NULL;
		GST_BUFFER_SIZE (buffer) = arv_buffer->size;

		if (viewer->timestamp_offset == 0) {
			viewer->timestamp_offset = arv_buffer->timestamp_ns;
			viewer->last_timestamp = arv_buffer->timestamp_ns;
		}

		GST_BUFFER_TIMESTAMP (buffer) = arv_buffer->timestamp_ns - viewer->timestamp_offset;
		GST_BUFFER_DURATION (buffer) = arv_buffer->timestamp_ns - viewer->last_timestamp;

		gst_app_src_push_buffer (GST_APP_SRC (viewer->appsrc), buffer);
	}

	arv_stream_push_buffer (stream, arv_buffer);
}

void
arv_viewer_release_camera (ArvViewer *viewer)
{
	g_return_if_fail (viewer != NULL);

	if (viewer->stream != NULL) {
		g_object_unref (viewer->stream);
		viewer->stream = NULL;
	}

	if (viewer->camera != NULL) {
		g_object_unref (viewer->camera);
		viewer->camera = NULL;
		viewer->device = NULL;
	}

	if (viewer->pipeline != NULL) {
		g_object_unref (viewer->pipeline);
		viewer->pipeline = NULL;
		viewer->appsrc = NULL;
	}
}

void
arv_viewer_select_camera_cb (GtkComboBox *combo_box, ArvViewer *viewer)
{
	GtkTreeIter iter;
	GtkTreeModel *list_store;
	GstCaps *caps;
	GstElement *ffmpegcolorspace;
	GstElement *ximagesink;
	char *camera_id;
	unsigned int payload;
	int width;
	int height;
	unsigned int frame_rate;
	unsigned int i;
	gulong window_xid;

	arv_viewer_release_camera (viewer);

	gtk_combo_box_get_active_iter (GTK_COMBO_BOX (viewer->camera_combo_box), &iter);
	list_store = gtk_combo_box_get_model (GTK_COMBO_BOX (viewer->camera_combo_box));
	gtk_tree_model_get (GTK_TREE_MODEL (list_store), &iter, 0, &camera_id, -1);
	viewer->camera = arv_camera_new (camera_id);
	g_free (camera_id);

	viewer->stream = arv_camera_create_stream (viewer->camera, NULL, NULL);
	arv_stream_set_emit_signals (viewer->stream, TRUE);
	payload = arv_camera_get_payload (viewer->camera);
	for (i = 0; i < 50; i++)
		arv_stream_push_buffer (viewer->stream, arv_buffer_new (payload, NULL));

	arv_camera_get_region (viewer->camera, NULL, NULL, &width, &height);
	frame_rate = (unsigned int) (double) (0.5 + arv_camera_get_frame_rate (viewer->camera));

	arv_camera_start_acquisition (viewer->camera);

	viewer->pipeline = gst_pipeline_new ("pipeline");

	viewer->appsrc = gst_element_factory_make ("appsrc", "appsrc");
	ffmpegcolorspace = gst_element_factory_make ("ffmpegcolorspace", "ffmpegcolorspace");
	ximagesink = gst_element_factory_make ("xvimagesink", "xvimagesink");
	g_object_set (ximagesink, "force-aspect-ratio", TRUE, NULL);
	gst_bin_add_many (GST_BIN (viewer->pipeline), viewer->appsrc, ffmpegcolorspace, ximagesink, NULL);
	gst_element_link_many (viewer->appsrc, ffmpegcolorspace, ximagesink, NULL);
	caps = gst_caps_new_simple ("video/x-raw-gray",
				    "bpp", G_TYPE_INT, 8,
				    "depth", G_TYPE_INT, 8,
				    "endianness", G_TYPE_INT, G_BIG_ENDIAN,
				    "width", G_TYPE_INT, width,
				    "height", G_TYPE_INT, height,
				    "framerate", GST_TYPE_FRACTION, frame_rate, 1,
				    "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
				    NULL);
	gst_app_src_set_caps (GST_APP_SRC (viewer->appsrc), caps);
	gst_caps_unref (caps);
	gst_element_set_state (viewer->pipeline, GST_STATE_PLAYING);

	window_xid = GDK_WINDOW_XID (viewer->drawing_area->window);
	gst_x_overlay_set_xwindow_id (GST_X_OVERLAY (ximagesink), window_xid);

	g_signal_connect (viewer->stream, "new-buffer", G_CALLBACK (arv_viewer_new_buffer_cb), viewer);
}

void
arv_viewer_free (ArvViewer *viewer)
{
	g_return_if_fail (viewer != NULL);

	arv_viewer_release_camera (viewer);
}

void
arv_viewer_quit_cb (GtkWidget *widget, ArvViewer *viewer)
{
	arv_viewer_free (viewer);

	gtk_main_quit ();
}

ArvViewer *
arv_viewer_new (void)
{
	GtkBuilder *builder;
	GtkCellRenderer *cell;
	ArvViewer *viewer;
	char *ui_filename;

	viewer = g_new0 (ArvViewer, 1);

	builder = gtk_builder_new ();

	ui_filename = g_build_filename (ARAVIS_DATA_DIR, "arv-viewer.ui", NULL);
	gtk_builder_add_from_file (builder, ui_filename, NULL);
	g_free (ui_filename);

	viewer->camera_combo_box = GTK_WIDGET (gtk_builder_get_object (builder, "camera_combobox"));
	viewer->main_window = GTK_WIDGET (gtk_builder_get_object (builder, "main_window"));
	viewer->drawing_area = GTK_WIDGET (gtk_builder_get_object (builder, "video_drawingarea"));

	g_object_unref (builder);

	cell = gtk_cell_renderer_text_new ();
	gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (viewer->camera_combo_box), cell, TRUE);
	gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (viewer->camera_combo_box), cell, "text", 0, NULL);

	gtk_widget_show_all (viewer->main_window);

	g_signal_connect (viewer->main_window, "destroy", G_CALLBACK (arv_viewer_quit_cb), viewer);
	g_signal_connect (viewer->camera_combo_box, "changed", G_CALLBACK (arv_viewer_select_camera_cb), viewer);

	return viewer;
}

int
main (int argc,char *argv[])
{
	ArvViewer *viewer;

	gtk_init (&argc, &argv);
	gst_init (&argc, &argv);

	viewer = arv_viewer_new ();

	arv_viewer_update_device_list_cb (viewer);
	arv_viewer_select_camera_cb (NULL, viewer);

	gtk_main ();

	return EXIT_SUCCESS;
}
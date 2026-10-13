/*
 * Build-time control: a GTK3 program with libgtk-nocsd.so.0 preloaded shows a
 * dialog transient for a window and destroys the window; no crash and no
 * critical messages. The dialog's fate is printed for comparison.
 */

#include <gtk/gtk.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int criticals;

static GLogWriterOutput
log_writer(GLogLevelFlags level, const GLogField *fields, gsize n,
	gpointer data)
{
	if (level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR))
		criticals++;
	return g_log_writer_default(level, fields, n, data);
}

static void
spin_until_mapped(GtkWidget *widget, guint ms)
{
	gint64 end = g_get_monotonic_time() + (gint64) ms * 1000;

	while ((widget == NULL || !gtk_widget_get_mapped(widget)) &&
		g_get_monotonic_time() < end) {
		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(5000);
	}
}

static void
spin(guint ms)
{
	spin_until_mapped(NULL, ms);
}

int
main(int argc, char **argv)
{
	const char *preload = getenv("LD_PRELOAD");
	int failures = 0;

	if (getenv("NOCSD_TEST_EXPECT_PRELOAD") != NULL &&
		(preload == NULL || preload[0] == '\0')) {
		printf("  (restarted without the library after a crash; ignored)\n");
		return 0;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);
	alarm(60);
	g_log_set_writer_func(log_writer, NULL, NULL);
	if (dlsym(RTLD_DEFAULT, "GTKNoCSDMagic") == NULL) {
		printf("  FAILED: libgtk-nocsd.so.0 is not preloaded\n");
		return 1;
	}
	gtk_init(&argc, &argv);

	gpointer window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_object_add_weak_pointer(window, &window);
	gtk_window_set_default_size(window, 400, 300);
	gtk_widget_show_all(window);
	spin_until_mapped(window, 5000);
	if (!gtk_widget_get_mapped(window)) {
		printf("  FAILED: window mapped\n");
		failures++;
	}

	gpointer dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL,
			GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "Question");
	g_object_add_weak_pointer(dialog, &dialog);
	gtk_widget_show(dialog);
	spin_until_mapped(dialog, 5000);
	if (!gtk_widget_get_mapped(dialog)) {
		printf("  FAILED: dialog mapped\n");
		failures++;
	}

	gtk_widget_destroy(window);
	spin(500);
	printf("  info: window destroyed: %s; dialog still there: %s\n",
		window == NULL ? "yes" : "no", dialog != NULL ? "yes" : "no");
	if (dialog != NULL)
		gtk_widget_destroy(dialog);
	spin(300);

	if (criticals != 0) {
		printf("  FAILED: no critical messages (%d)\n", criticals);
		failures++;
	} else {
		printf("  ok: no critical messages\n");
	}
	return failures == 0 ? 0 : 1;
}

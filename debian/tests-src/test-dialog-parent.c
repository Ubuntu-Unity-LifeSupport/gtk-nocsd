/*
 * Build-time regression test: an AdwDialog that gtk-nocsd pops out into a
 * window of its own goes away with the window it was presented on, as an
 * embedded AdwDialog does without gtk-nocsd.
 *
 * Run one case per process with the freshly built libgtk-nocsd.so.0 in
 * LD_PRELOAD, under an X server: test-dialog-parent <case>
 */

#include <adwaita.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAIT_MS 5000

static int failures;
static int criticals;

#define CHECK(cond, ...)								\
	do {												\
		if (cond) {										\
			printf("  ok: ");							\
		} else {										\
			printf("  FAILED: ");						\
			failures++;									\
		}												\
		printf(__VA_ARGS__);							\
		printf("\n");									\
		fflush(stdout);									\
	} while (0)

static GLogWriterOutput
log_writer(GLogLevelFlags level, const GLogField *fields, gsize n,
	gpointer data)
{
	if (level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR))
		criticals++;
	return g_log_writer_default(level, fields, n, data);
}

static gboolean
is_null(gpointer data)
{
	return *(gpointer *) data == NULL;
}

static gboolean
spin_until(gboolean (*cond)(gpointer), gpointer data, guint ms)
{
	gint64 end = g_get_monotonic_time() + (gint64) ms * 1000;

	while (!cond(data)) {
		if (g_get_monotonic_time() > end)
			return FALSE;
		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(5000);
	}
	return TRUE;
}

static gboolean
never(G_GNUC_UNUSED gpointer data)
{
	return FALSE;
}

static void
spin(guint ms)
{
	spin_until(never, NULL, ms);
}

static gboolean
is_mapped(gpointer data)
{
	return gtk_widget_get_mapped(GTK_WIDGET(data));
}

static void
watch(gpointer object, gpointer *pointer)
{
	*pointer = object;
	if (object != NULL)
		g_object_add_weak_pointer(G_OBJECT(object), pointer);
}

static GtkWindow *
new_parent(void)
{
	GtkWindow *window = GTK_WINDOW(gtk_window_new());

	gtk_window_set_default_size(window, 400, 300);
	gtk_window_set_child(window, gtk_label_new("parent"));
	gtk_window_present(window);
	CHECK(spin_until(is_mapped, window, WAIT_MS), "parent window mapped");
	return window;
}

static AdwDialog *
new_dialog(void)
{
	AdwDialog *dialog = adw_dialog_new();

	adw_dialog_set_child(dialog, gtk_label_new("dialog"));
	adw_dialog_set_content_width(dialog, 200);
	adw_dialog_set_content_height(dialog, 100);
	return dialog;
}

struct presented {
	AdwDialog *dialog;
	GtkWindow *transient;
};

static gboolean
is_presented(gpointer data)
{
	struct presented *p = data;
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(p->dialog));

	return GTK_IS_WINDOW(root) && (GtkWindow *) root != p->transient &&
		gtk_widget_get_mapped(GTK_WIDGET(root)) &&
		gtk_window_get_transient_for(GTK_WINDOW(root)) == p->transient;
}

// Present the dialog on parent; gtk-nocsd puts it in a window of its own,
// transient for transient. Returns that window.
static GtkWindow *
present(AdwDialog *dialog, GtkWidget *parent, GtkWindow *transient)
{
	struct presented p = { dialog, transient };

	adw_dialog_present(dialog, parent);
	CHECK(spin_until(is_presented, &p, WAIT_MS),
		"dialog %p is in its own window, transient for %p", (void *) dialog,
		(void *) transient);
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(dialog));
	return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

static void
count(G_GNUC_UNUSED gpointer instance, gpointer data)
{
	(*(int *) data)++;
}

// Present a dialog again. libadwaita parents the dialog's breakpoint bin
// each time it presents the dialog as a window, and never unparents it, so
// a second presentation logs a gtk_widget_set_parent critical with or
// without gtk-nocsd (control-represent-after-close). Those are reported and
// left out of the final count.
static GtkWindow *
present_again(AdwDialog *dialog, GtkWidget *parent, GtkWindow *transient)
{
	int before = criticals;
	GtkWindow *window = present(dialog, parent, transient);

	printf("  info: critical messages while presenting again: %d\n",
		criticals - before);
	criticals = before;
	return window;
}

/* T1, T1b: GtkApplication, AdwApplicationWindow, AdwAlertDialog; the main
 * window goes, the application quits. After g_application_run returns the
 * application must be finalized (the dialog holds it as its "app" action
 * group and the main window as its "win" action group). */

static int app_close_mode;
static gpointer app_dialog, app_dialog_window, app_main_window;

static void
app_activate(GApplication *app, G_GNUC_UNUSED gpointer data)
{
	GtkWidget *window = adw_application_window_new(GTK_APPLICATION(app));

	adw_application_window_set_content(ADW_APPLICATION_WINDOW(window),
		gtk_label_new("main window"));
	gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);
	gtk_window_present(GTK_WINDOW(window));
	CHECK(spin_until(is_mapped, window, WAIT_MS), "main window mapped");

	AdwDialog *dialog = adw_alert_dialog_new("Question", "Body");
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "no", "No",
		"yes", "Yes", NULL);
	watch(dialog, &app_dialog);
	watch(window, &app_main_window);
	GtkWindow *dialog_window = present(dialog, window, GTK_WINDOW(window));
	if (dialog_window != NULL)
		watch(dialog_window, &app_dialog_window);

	if (app_close_mode)
		gtk_window_close(GTK_WINDOW(window));
	else
		gtk_window_destroy(GTK_WINDOW(window));
	g_application_quit(app);
}

static gboolean
app_timeout(gpointer app)
{
	CHECK(FALSE, "application quit within %d ms", 2 * WAIT_MS);
	g_application_quit(G_APPLICATION(app));
	return G_SOURCE_REMOVE;
}

static void
case_app(int close_mode)
{
	gpointer app_ptr;
	AdwApplication *app = adw_application_new(NULL, G_APPLICATION_NON_UNIQUE);

	app_close_mode = close_mode;
	watch(app, &app_ptr);
	g_signal_connect(app, "activate", G_CALLBACK(app_activate), NULL);
	guint timeout = g_timeout_add(2 * WAIT_MS, app_timeout, app);
	g_application_run(G_APPLICATION(app), 0, NULL);
	g_source_remove(timeout);
	g_object_unref(app);

	spin_until(is_null, &app_ptr, WAIT_MS);
	CHECK(app_dialog_window == NULL, "dialog window finalized");
	CHECK(app_dialog == NULL, "dialog finalized");
	CHECK(app_main_window == NULL, "main window finalized");
	CHECK(app_ptr == NULL, "application finalized after g_application_run");
}

/* T2: plain GtkWindow and AdwDialog; the parent is destroyed */
static void
case_plain(void)
{
	gpointer parent, dialog, window;

	watch(new_parent(), &parent);
	watch(new_dialog(), &dialog);
	watch(present(dialog, parent, parent), &window);
	gtk_window_destroy(parent);
	spin_until(is_null, &window, WAIT_MS);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(window == NULL, "dialog window finalized");
	CHECK(dialog == NULL, "dialog finalized");
	CHECK(parent == NULL, "parent finalized");
}

/* T3: a dialog presented on a dialog; the main parent is destroyed */
static void
case_nested(void)
{
	gpointer parent, first, first_window, second, second_window;

	watch(new_parent(), &parent);
	watch(new_dialog(), &first);
	watch(present(first, parent, parent), &first_window);
	watch(new_dialog(), &second);
	watch(present(second, first, first_window), &second_window);
	gtk_window_destroy(parent);
	spin_until(is_null, &second, WAIT_MS);
	spin_until(is_null, &first, WAIT_MS);
	CHECK(second_window == NULL, "second dialog window finalized");
	CHECK(second == NULL, "second dialog finalized");
	CHECK(first_window == NULL, "first dialog window finalized");
	CHECK(first == NULL, "first dialog finalized");
}

// Wait until counter reaches value, then a little longer for extra emissions
static void
wait_count(const int *counter, int value)
{
	gint64 end = g_get_monotonic_time() + (gint64) WAIT_MS * 1000;

	while (*counter < value && g_get_monotonic_time() < end) {
		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(5000);
	}
	spin(300);
}

/* T4: the application keeps the dialog and presents it again on another
 * window after the first parent is gone */
static void
case_represent(void)
{
	gpointer first_parent, second_parent, dialog, first_window, second_window;
	int closed = 0;

	watch(new_parent(), &first_parent);
	watch(new_parent(), &second_parent);
	AdwDialog *kept = g_object_ref_sink(new_dialog());
	watch(kept, &dialog);
	g_signal_connect(kept, "closed", G_CALLBACK(count), &closed);

	watch(present(kept, first_parent, first_parent), &first_window);
	gtk_window_destroy(first_parent);
	wait_count(&closed, 1);
	spin_until(is_null, &first_window, WAIT_MS);
	CHECK(closed == 1, "\"closed\" emitted once when the first parent went (%d)",
		closed);
	CHECK(first_window == NULL, "first dialog window finalized");

	watch(present_again(kept, second_parent, second_parent), &second_window);
	spin(300);
	CHECK(closed == 1, "no \"closed\" on presenting again (%d)", closed);
	gtk_window_destroy(second_parent);
	wait_count(&closed, 2);
	spin_until(is_null, &second_window, WAIT_MS);
	CHECK(closed == 2, "\"closed\" emitted once when the second parent went "
		"(%d in all)", closed);
	CHECK(second_window == NULL, "second dialog window finalized");

	g_object_unref(kept);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(dialog == NULL, "dialog finalized after the application let it go");
}

/* T5: can-close FALSE; the dialog still goes with its parent and keeps the
 * application's can-close setting */
static void
case_can_close(void)
{
	gpointer parent, dialog, window;
	int closed = 0, attempts = 0;

	watch(new_parent(), &parent);
	AdwDialog *kept = g_object_ref_sink(new_dialog());
	watch(kept, &dialog);
	adw_dialog_set_can_close(kept, FALSE);
	g_signal_connect(kept, "closed", G_CALLBACK(count), &closed);
	g_signal_connect(kept, "close-attempt", G_CALLBACK(count), &attempts);

	watch(present(kept, parent, parent), &window);
	gtk_window_destroy(parent);
	wait_count(&closed, 1);
	spin_until(is_null, &window, WAIT_MS);
	CHECK(closed == 1, "\"closed\" emitted once (%d)", closed);
	CHECK(attempts == 0, "no \"close-attempt\" (%d)", attempts);
	CHECK(window == NULL, "dialog window finalized");
	CHECK(!adw_dialog_get_can_close(kept), "can-close is FALSE again");

	g_object_unref(kept);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(dialog == NULL, "dialog finalized after the application let it go");
}

/* Control: the dialog is closed by the application before the parent goes */
static void
case_closed_first(void)
{
	gpointer parent, dialog, window;
	int closed = 0;

	watch(new_parent(), &parent);
	watch(new_dialog(), &dialog);
	g_signal_connect(dialog, "closed", G_CALLBACK(count), &closed);
	watch(present(dialog, parent, parent), &window);
	CHECK(adw_dialog_close(dialog), "adw_dialog_close succeeded");
	spin_until(is_null, &window, WAIT_MS);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(window == NULL, "dialog window finalized");
	CHECK(dialog == NULL, "dialog finalized");
	gtk_window_destroy(parent);
	spin_until(is_null, &parent, WAIT_MS);
	spin(300);
	CHECK(parent == NULL, "parent finalized");
	CHECK(closed == 1, "\"closed\" emitted once (%d)", closed);
}

/* Control: a kept dialog closed by the application is presented again on
 * another window */
static void
case_represent_after_close(void)
{
	gpointer first_parent, second_parent, dialog, first_window, second_window;
	int closed = 0;

	watch(new_parent(), &first_parent);
	watch(new_parent(), &second_parent);
	AdwDialog *kept = g_object_ref_sink(new_dialog());
	watch(kept, &dialog);
	g_signal_connect(kept, "closed", G_CALLBACK(count), &closed);

	watch(present(kept, first_parent, first_parent), &first_window);
	CHECK(adw_dialog_close(kept), "adw_dialog_close succeeded");
	spin_until(is_null, &first_window, WAIT_MS);
	CHECK(first_window == NULL, "first dialog window finalized");
	watch(present_again(kept, second_parent, second_parent), &second_window);
	CHECK(adw_dialog_close(kept), "adw_dialog_close succeeded again");
	spin_until(is_null, &second_window, WAIT_MS);
	CHECK(second_window == NULL, "second dialog window finalized");
	CHECK(closed == 2, "\"closed\" emitted once per close (%d)", closed);

	g_object_unref(kept);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(dialog == NULL, "dialog finalized after the application let it go");
	gtk_window_destroy(first_parent);
	gtk_window_destroy(second_parent);
	spin(300);
}

/* Control: a parent that hides on close stays, and so does its dialog */
static void
case_hide_on_close(void)
{
	gpointer parent, dialog, window;
	int closed = 0;

	watch(new_parent(), &parent);
	gtk_window_set_hide_on_close(parent, TRUE);
	watch(new_dialog(), &dialog);
	g_signal_connect(dialog, "closed", G_CALLBACK(count), &closed);
	watch(present(dialog, parent, parent), &window);
	gtk_window_close(parent);
	spin(500);
	CHECK(parent != NULL && !gtk_widget_get_visible(parent),
		"parent hidden, not destroyed");
	CHECK(closed == 0, "no \"closed\" (%d)", closed);
	CHECK(window != NULL && dialog != NULL, "dialog and its window still there");
	if (dialog != NULL)
		CHECK(adw_dialog_close(dialog), "dialog closes normally afterwards");
	spin_until(is_null, &window, WAIT_MS);
	CHECK(closed == 1, "\"closed\" emitted once (%d)", closed);
	if (parent != NULL)
		gtk_window_destroy(parent);
	spin(300);
}

/* T6: two dialogs presented on one parent; both go with it */
static void
case_two_dialogs(void)
{
	gpointer parent, first, first_window, second, second_window;
	int closed = 0;

	watch(new_parent(), &parent);
	watch(new_dialog(), &first);
	watch(new_dialog(), &second);
	g_signal_connect(first, "closed", G_CALLBACK(count), &closed);
	g_signal_connect(second, "closed", G_CALLBACK(count), &closed);
	watch(present(first, parent, parent), &first_window);
	watch(present(second, parent, parent), &second_window);
	gtk_window_destroy(parent);
	wait_count(&closed, 2);
	spin_until(is_null, &first, WAIT_MS);
	spin_until(is_null, &second, WAIT_MS);
	CHECK(closed == 2, "\"closed\" emitted once per dialog (%d)", closed);
	CHECK(first_window == NULL && first == NULL,
		"first dialog and its window finalized");
	CHECK(second_window == NULL && second == NULL,
		"second dialog and its window finalized");
}

/* Control: an AdwAlertDialog presented with adw_alert_dialog_choose stays
 * embedded in its AdwApplicationWindow; force-closing it emits "closed"
 * once and leaves the window, on which a dialog can be presented again */
static gboolean
is_embedded(gpointer data)
{
	struct presented *p = data;

	return gtk_widget_get_root(GTK_WIDGET(p->dialog)) ==
		(GtkRoot *) p->transient &&
		gtk_widget_get_mapped(GTK_WIDGET(p->dialog));
}

static gboolean
in_toplevels(GtkWindow *window)
{
	GListModel *toplevels = gtk_window_get_toplevels();
	gboolean found = FALSE;

	for (guint i = 0; i < g_list_model_get_n_items(toplevels); i++) {
		GObject *toplevel = g_list_model_get_item(toplevels, i);

		found = found || toplevel == (GObject *) window;
		g_object_unref(toplevel);
	}
	return found;
}

static void
embedded_chosen(GObject *dialog, GAsyncResult *result,
	G_GNUC_UNUSED gpointer data)
{
	adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result);
}

static void
embedded_activate(GApplication *app, G_GNUC_UNUSED gpointer data)
{
	gpointer second, second_window;
	int closed = 0;
	GtkWidget *window = adw_application_window_new(GTK_APPLICATION(app));

	adw_application_window_set_content(ADW_APPLICATION_WINDOW(window),
		gtk_label_new("main window"));
	gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);
	gtk_window_present(GTK_WINDOW(window));
	CHECK(spin_until(is_mapped, window, WAIT_MS), "main window mapped");

	AdwDialog *alert = adw_alert_dialog_new("Question", "Body");
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "no", "No",
		"yes", "Yes", NULL);
	g_signal_connect(alert, "closed", G_CALLBACK(count), &closed);
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(alert), window, NULL,
		embedded_chosen, NULL);
	struct presented p = { alert, GTK_WINDOW(window) };
	CHECK(spin_until(is_embedded, &p, WAIT_MS),
		"dialog embedded in the main window");
	spin(300);

	adw_dialog_force_close(alert);
	wait_count(&closed, 1);
	CHECK(closed == 1, "\"closed\" emitted once (%d)", closed);
	CHECK(in_toplevels(GTK_WINDOW(window)), "main window still a toplevel");

	watch(new_dialog(), &second);
	watch(present(second, window, GTK_WINDOW(window)), &second_window);
	if (second != NULL)
		CHECK(adw_dialog_close(second), "new dialog closes");
	spin_until(is_null, &second_window, WAIT_MS);
	CHECK(second_window == NULL, "new dialog window finalized");

	gtk_window_destroy(GTK_WINDOW(window));
	g_application_quit(app);
}

static void
case_force_close_embedded(void)
{
	AdwApplication *app = adw_application_new(NULL, G_APPLICATION_NON_UNIQUE);

	g_signal_connect(app, "activate", G_CALLBACK(embedded_activate), NULL);
	guint timeout = g_timeout_add(4 * WAIT_MS, app_timeout, app);
	g_application_run(G_APPLICATION(app), 0, NULL);
	g_source_remove(timeout);
	g_object_unref(app);
}

/* F1: the application keeps the dialog and force-closes it; the dialog
 * window goes, "closed" is emitted once, and the dialog can be presented
 * again on the same window */
static void
case_force_close_kept(void)
{
	gpointer parent, dialog, first_window, second_window = NULL;
	int closed = 0;

	watch(new_parent(), &parent);
	AdwDialog *kept = g_object_ref_sink(new_dialog());
	watch(kept, &dialog);
	g_signal_connect(kept, "closed", G_CALLBACK(count), &closed);

	watch(present(kept, parent, parent), &first_window);
	adw_dialog_force_close(kept);
	wait_count(&closed, 1);
	spin_until(is_null, &first_window, WAIT_MS);
	CHECK(first_window == NULL, "dialog window finalized");
	CHECK(closed == 1, "\"closed\" emitted once (%d)", closed);

	if (closed == 1) {
		watch(present_again(kept, parent, parent), &second_window);
		CHECK(second_window != NULL && gtk_widget_get_mapped(second_window),
			"presented again in a new mapped window");
		spin(300);
		CHECK(closed == 1, "no \"closed\" on presenting again (%d)", closed);
		adw_dialog_close(kept);
		spin_until(is_null, &second_window, WAIT_MS);
	}

	gtk_window_destroy(parent);
	spin(300);
	g_object_unref(kept);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(dialog == NULL, "dialog finalized after the application let it go");
}

/* F2: a single instance cleared from "closed", as epiphany does with its
 * preferences dialog: force-closed when it is wanted on another window, then
 * created again there */
static AdwDialog *singleton;

static void
singleton_closed(G_GNUC_UNUSED AdwDialog *dialog, G_GNUC_UNUSED gpointer data)
{
	singleton = NULL;
}

static void
case_force_close_singleton(void)
{
	gpointer first_parent, second_parent, first, first_window, second;

	watch(new_parent(), &first_parent);
	watch(new_parent(), &second_parent);
	singleton = new_dialog();
	g_signal_connect(singleton, "closed", G_CALLBACK(singleton_closed), NULL);
	watch(singleton, &first);
	watch(present(singleton, first_parent, first_parent), &first_window);

	adw_dialog_force_close(singleton);
	spin_until(is_null, &first_window, WAIT_MS);
	spin(300);
	CHECK(singleton == NULL, "single instance cleared by \"closed\"");
	CHECK(first_window == NULL && first == NULL,
		"first dialog and its window finalized");

	if (singleton == NULL) {
		singleton = new_dialog();
		g_signal_connect(singleton, "closed", G_CALLBACK(singleton_closed),
			NULL);
		watch(singleton, &second);
		present(singleton, second_parent, second_parent);
		CHECK(adw_dialog_close(singleton), "new dialog closes");
		spin_until(is_null, &second, WAIT_MS);
		CHECK(singleton == NULL && second == NULL, "new dialog finalized");
	}

	gtk_window_destroy(first_parent);
	gtk_window_destroy(second_parent);
	spin(300);
}

/* F3: can-close FALSE; force-closing still closes the dialog and keeps the
 * application's can-close setting */
static void
case_force_close_can_close(void)
{
	gpointer parent, dialog, window;
	int closed = 0, attempts = 0;

	watch(new_parent(), &parent);
	AdwDialog *kept = g_object_ref_sink(new_dialog());
	watch(kept, &dialog);
	adw_dialog_set_can_close(kept, FALSE);
	g_signal_connect(kept, "closed", G_CALLBACK(count), &closed);
	g_signal_connect(kept, "close-attempt", G_CALLBACK(count), &attempts);

	watch(present(kept, parent, parent), &window);
	adw_dialog_force_close(kept);
	wait_count(&closed, 1);
	spin_until(is_null, &window, WAIT_MS);
	CHECK(closed == 1, "\"closed\" emitted once (%d)", closed);
	CHECK(attempts == 0, "no \"close-attempt\" (%d)", attempts);
	CHECK(window == NULL, "dialog window finalized");
	CHECK(!adw_dialog_get_can_close(kept), "can-close is FALSE again");

	gtk_window_destroy(parent);
	spin(300);
	g_object_unref(kept);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(dialog == NULL, "dialog finalized after the application let it go");
}

static void
force_close_handler(AdwDialog *dialog, gpointer data)
{
	(*(int *) data)++;
	adw_dialog_force_close(dialog);
}

/* F4: the application force-closes the dialog from its "close-attempt"
 * handler */
static void
case_force_close_from_close_attempt(void)
{
	gpointer parent, dialog, window;
	int closed = 0, attempts = 0;

	watch(new_parent(), &parent);
	watch(new_dialog(), &dialog);
	adw_dialog_set_can_close(dialog, FALSE);
	g_signal_connect(dialog, "closed", G_CALLBACK(count), &closed);
	g_signal_connect(dialog, "close-attempt", G_CALLBACK(force_close_handler),
		&attempts);
	watch(present(dialog, parent, parent), &window);

	adw_dialog_close(dialog);
	wait_count(&closed, 1);
	spin_until(is_null, &window, WAIT_MS);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(attempts == 1, "one \"close-attempt\" (%d)", attempts);
	CHECK(closed == 1, "\"closed\" emitted once (%d)", closed);
	CHECK(window == NULL && dialog == NULL,
		"dialog and its window finalized");
	gtk_window_destroy(parent);
	spin(300);
}

/* Control: force-closing a dialog that was never presented does nothing */
static void
case_force_close_unpresented(void)
{
	gpointer dialog;
	int closed = 0;

	AdwDialog *kept = g_object_ref_sink(new_dialog());
	watch(kept, &dialog);
	g_signal_connect(kept, "closed", G_CALLBACK(count), &closed);
	adw_dialog_force_close(kept);
	spin(300);
	CHECK(closed == 0, "no \"closed\" (%d)", closed);
	g_object_unref(kept);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(dialog == NULL, "dialog finalized");
}

/* Control: the application force-closes the dialog from its "closed"
 * handler after closing it */
static void
case_force_close_from_closed(void)
{
	gpointer parent, dialog, window;
	int closed = 0;

	watch(new_parent(), &parent);
	watch(new_dialog(), &dialog);
	g_signal_connect(dialog, "closed", G_CALLBACK(force_close_handler),
		&closed);
	watch(present(dialog, parent, parent), &window);

	CHECK(adw_dialog_close(dialog), "adw_dialog_close succeeded");
	wait_count(&closed, 1);
	spin_until(is_null, &window, WAIT_MS);
	spin_until(is_null, &dialog, WAIT_MS);
	CHECK(closed == 1, "\"closed\" emitted once (%d)", closed);
	CHECK(window == NULL && dialog == NULL,
		"dialog and its window finalized");
	gtk_window_destroy(parent);
	spin(300);
}

static const struct {
	const char *name;
	void (*run)(void);
} plain_cases[] = {
	{ "T2-plain-window", case_plain },
	{ "T3-nested-dialog", case_nested },
	{ "T4-represent-kept-dialog", case_represent },
	{ "T5-can-close-false", case_can_close },
	{ "control-closed-first", case_closed_first },
	{ "control-represent-after-close", case_represent_after_close },
	{ "control-hide-on-close-parent", case_hide_on_close },
	{ "T6-two-dialogs", case_two_dialogs },
	{ "F1-force-close-kept-dialog", case_force_close_kept },
	{ "F2-force-close-single-instance", case_force_close_singleton },
	{ "F3-force-close-can-close-false", case_force_close_can_close },
	{ "F4-force-close-from-close-attempt",
		case_force_close_from_close_attempt },
	{ "control-force-close-unpresented", case_force_close_unpresented },
	{ "control-force-close-from-closed", case_force_close_from_closed },
};

int
main(int argc, char **argv)
{
	const char *preload = getenv("LD_PRELOAD");

	// gtk-nocsd's crash handler starts the program again without the library
	// after a crash; the crash itself is what the harness reports
	if (getenv("NOCSD_TEST_EXPECT_PRELOAD") != NULL &&
		(preload == NULL || preload[0] == '\0')) {
		printf("  (restarted without the library after a crash; ignored)\n");
		return 0;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 2) {
		fprintf(stderr, "usage: %s <case>\n", argv[0]);
		return 2;
	}
	// Bound every case even if a wait above never returns
	alarm(60);
	g_log_set_writer_func(log_writer, NULL, NULL);

	if (dlsym(RTLD_DEFAULT, "GTKNoCSDMagic") == NULL) {
		printf("  FAILED: libgtk-nocsd.so.0 is not preloaded\n");
		return 1;
	}

	gboolean found = FALSE;
	if (strcmp(argv[1], "T1-app-destroy") == 0) {
		case_app(0);
		found = TRUE;
	} else if (strcmp(argv[1], "T1b-app-close") == 0) {
		case_app(1);
		found = TRUE;
	} else if (strcmp(argv[1], "control-force-close-embedded") == 0) {
		case_force_close_embedded();
		found = TRUE;
	} else {
		adw_init();
		for (gsize i = 0; i < G_N_ELEMENTS(plain_cases); i++) {
			if (strcmp(argv[1], plain_cases[i].name) == 0) {
				plain_cases[i].run();
				found = TRUE;
			}
		}
	}
	if (!found) {
		fprintf(stderr, "unknown case %s\n", argv[1]);
		return 2;
	}

	CHECK(criticals == 0, "no critical messages (%d)", criticals);
	return failures == 0 ? 0 : 1;
}

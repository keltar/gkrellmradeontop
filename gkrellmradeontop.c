#include <gkrellm2/gkrellm.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include "subprocess.h"

#define PLUGIN_NAME "gkrellmradeontop"
#define PLUGIN_DESC "show AMD GPU load chart"
#define PLUGIN_STYLE PLUGIN_NAME
#define PLUGIN_KEYWORD PLUGIN_NAME

#define SCALE_MARK 100
#define SCALE_MAX 100
#define MIN_GRID_RES 10
#define MAX_GRID_RES 100

#define CMDLINE_MAX_LEN 1024
#define RADEONTOP_DEFAULT_CMDLINE "/usr/bin/radeontop -d - -t 1"

//#define DBGPRINTF(fmt, ...) fprintf(stderr, (fmt), __VA_ARGS__)
#define DBGPRINTF(fmt, ...)

struct gpu_stats {
	time_t stats_timestamp;

	unsigned int gpu_pipe;
	unsigned int shader_clock;
};

static struct {
	gboolean enabled;

	char name[64];
	char panel_label[64];

	gboolean extra_info;

	GtkWidget *vbox;
	GkrellmChart *chart;
	GkrellmChartconfig *chart_config;
	GkrellmKrell *krell;

	pthread_mutex_t mutex;
	struct {
		pthread_t thread;
		struct subprocess_s subprocess;
		bool subprocess_running;
		bool stop_thread;
	} radeontop;

	struct gpu_stats gpu_stats, gpu_stats_copy;

	struct {
		GtkWidget *radeontop_cmdline_entry;
		char radeontop_cmdline[CMDLINE_MAX_LEN];
	} options;
} gpu_mon;

static gint style_id;

static GkrellmMonitor *gpu_plugin_mon_ptr;

static void extract_cmdline(const char **out, size_t max, char *buf, const char *in) {
	// TODO add support for quoted arguments with spaces inside
	strcpy(buf, in);
	size_t len = strlen(buf);
	size_t idx = 0, cur_len = 0;
	for(size_t i = 0; i < len+1; ++i) {
		if(buf[i] == '\0' || isspace(buf[i])) {
			buf[i] = '\0';
			if(cur_len > 0) {
				if(idx < max) {
					out[idx] = &buf[i-cur_len];
				}
				idx++;
			}
			cur_len = 0;
			continue;
		}

		cur_len++;
	}
	if(idx < max) {
		out[idx] = 0;
	} else {
		out[max-1] = 0;
	}
}

static float radeontop_extract_stat(const char *str, char *label) {
	const char *m = strstr(str, label);
	if(!m) {
		fprintf(stderr, "no %s marker in radeontop output, output is \"%s\"\n", label, str);
	} else {
		float v = 0;
		char fmt[64];
		snprintf(fmt, sizeof(fmt), "%s %%f%%%%", label);
		if(sscanf(m, fmt, &v) == 1) {
			DBGPRINTF("fetched %f gpu load\n", v);
			return v;
		} else {
			fprintf(stderr, "can't decode %s from string %s\n", label, m);
		}
	}

	return 0;
}

static void stop_helper_process(void) {
	pthread_mutex_lock(&gpu_mon.mutex);
	if(gpu_mon.radeontop.thread) {
		gpu_mon.radeontop.stop_thread = true;
		if(gpu_mon.radeontop.subprocess_running) {
			subprocess_terminate(&gpu_mon.radeontop.subprocess);
			gpu_mon.radeontop.subprocess_running = false;
		}
	}
	pthread_mutex_unlock(&gpu_mon.mutex);
	pthread_join(gpu_mon.radeontop.thread, NULL);
	gpu_mon.radeontop.thread = 0;
}

static void *radeontop_thread(void *arg) {
	(void)arg;

	memset(&gpu_mon.gpu_stats, 0, sizeof(gpu_mon.gpu_stats));
	gpu_mon.radeontop.stop_thread = false;

	while(1) {
		/* early check for thread exit. Could happen if both radeontop and
		 * gkrellm got int/term signal. This still could potentially trigger a
		 * deadlock, although chances of that happening are very low.
		 * Proper fix should do a read timeout and re-check thread stop flag, or
		 * pthread_timedwait_np() and re-kill subprocess.
		 * FIXME */
		pthread_mutex_lock(&gpu_mon.mutex);
		if(gpu_mon.radeontop.stop_thread) {
			pthread_mutex_unlock(&gpu_mon.mutex);
			break;
		}

		char cmdline_buf[CMDLINE_MAX_LEN];
		const char *cmdline[128];
		extract_cmdline(cmdline, sizeof(cmdline)/sizeof(cmdline[0]),
				cmdline_buf, gpu_mon.options.radeontop_cmdline);

		int result = subprocess_create(cmdline, 0, &gpu_mon.radeontop.subprocess);
		if(result != 0) {
			fprintf(stderr, "can't launch radeontop");
			return NULL;
		}
		gpu_mon.radeontop.subprocess_running = true;
		gpu_mon.radeontop.stop_thread = false;
		pthread_mutex_unlock(&gpu_mon.mutex);

		FILE *p = subprocess_stdout(&gpu_mon.radeontop.subprocess);

		char buffer[512];

		// eat first line
		if(fgets(buffer, sizeof(buffer), p)) {
			while(fgets(buffer, sizeof(buffer), p)) {
				DBGPRINTF("%s\n", buffer);

				struct gpu_stats stats = {
					.gpu_pipe = radeontop_extract_stat(buffer, "gpu "),
					.shader_clock = radeontop_extract_stat(buffer, "sclk "),
				};

				pthread_mutex_lock(&gpu_mon.mutex);
				memcpy(&gpu_mon.gpu_stats, &stats, sizeof(stats));
				gpu_mon.gpu_stats.stats_timestamp = time(NULL);
				pthread_mutex_unlock(&gpu_mon.mutex);
			}
		}

		if(subprocess_join(&gpu_mon.radeontop.subprocess, NULL) != 0) {
			fprintf(stderr, "subprocess_join failed, killing process\n");
			subprocess_terminate(&gpu_mon.radeontop.subprocess);
		}

		pthread_mutex_lock(&gpu_mon.mutex);
		gpu_mon.radeontop.subprocess_running = false;
		bool brk = gpu_mon.radeontop.stop_thread;
		pthread_mutex_unlock(&gpu_mon.mutex);

		if(brk) {
			break;
		}

		fprintf(stderr, "radeontop is finished, restarting in 5 seconds\n");
		sleep(5);	// TODO should this be configurable? define'd?
	}

	return NULL;
}

static void draw_chart(GkrellmChart *cp) {
	gkrellm_draw_chartdata(cp);
	if(gpu_mon.extra_info) {
		gchar buf[64];
		snprintf(buf, sizeof(buf), "\\w88\\a%d\\f %d",
				gpu_mon.gpu_stats_copy.shader_clock,
				gpu_mon.gpu_stats_copy.gpu_pipe);
		gkrellm_draw_chart_text(cp, style_id, buf);
	}
	gkrellm_draw_chart_to_screen(cp);
}

static gint expose_event(GtkWidget *widget, GdkEventExpose *ev) {
	GdkPixmap *pixmap = NULL;
	if(widget == gpu_mon.chart->drawing_area) {
		pixmap = gpu_mon.chart->pixmap;
	} else if(widget == gpu_mon.chart->panel->drawing_area) {
		pixmap = gpu_mon.chart->panel->pixmap;
	}
	if(pixmap) {
		gdk_draw_pixmap(widget->window, gkrellm_draw_GC(1), pixmap,
				ev->area.x, ev->area.y, ev->area.x, ev->area.y,
				ev->area.width, ev->area.height);
	}
	return FALSE;
}

static gint mouseclick_event(GtkWidget *widget, GdkEventButton *ev) {
	if(widget != gpu_mon.chart->drawing_area) {
		return FALSE;
	}

	if(ev->button == 1 && ev->type == GDK_BUTTON_PRESS) {
		gpu_mon.extra_info = !gpu_mon.extra_info;
		draw_chart(gpu_mon.chart);
		gkrellm_config_modified();
	} else if(ev->button == 3 || (ev->button == 1 && ev->type == GDK_2BUTTON_PRESS)) {
		gkrellm_chartconfig_window_create(gpu_mon.chart);
	}
	return FALSE;
}

static void setup_scaling(GkrellmChartconfig *cf, void *cp) {
	(void)cp;
	gkrellm_set_chartconfig_auto_grid_resolution(cf, FALSE);
	gkrellm_set_chartconfig_grid_resolution(cf, SCALE_MAX / FULL_SCALE_GRIDS);
}

static void create_plugin(GtkWidget *vbox, gint first_create) {
	gpu_mon.enabled = TRUE;

	if(first_create) {
		pthread_mutex_init(&gpu_mon.mutex, NULL);
		gkrellm_disable_plugin_connect(gpu_plugin_mon_ptr, &stop_helper_process);
		atexit(&stop_helper_process);

		gpu_mon.vbox = gtk_vbox_new(FALSE, 0);
		gtk_container_add(GTK_CONTAINER(vbox), gpu_mon.vbox);
		gtk_widget_show(gpu_mon.vbox);

		gpu_mon.chart = gkrellm_chart_new0();
		gpu_mon.chart->panel = gkrellm_panel_new0();
	} else {
		gkrellm_destroy_decal_list(gpu_mon.chart->panel);
		gkrellm_destroy_krell_list(gpu_mon.chart->panel);
	}

	if(!gpu_mon.radeontop.thread) {
		pthread_create(&gpu_mon.radeontop.thread, NULL, &radeontop_thread, NULL);
	}

	GkrellmStyle *style = gkrellm_panel_style(style_id);

	gkrellm_chart_create(vbox, gpu_plugin_mon_ptr, gpu_mon.chart, &gpu_mon.chart_config);

	GkrellmChartdata *cd = gkrellm_add_default_chartdata(gpu_mon.chart, "shader clock");
	gkrellm_monotonic_chartdata(cd, FALSE);
	gkrellm_set_chartdata_draw_style_default(cd, CHARTDATA_LINE);
	gkrellm_set_chartdata_flags(cd, CHARTDATA_ALLOW_HIDE);

	cd = gkrellm_add_default_chartdata(gpu_mon.chart, "graphics pipe");
	gkrellm_monotonic_chartdata(cd, FALSE);

	gkrellm_chartconfig_fixed_grids_connect(gpu_mon.chart->config,
				setup_scaling, gpu_mon.chart);

	gkrellm_alloc_chartdata(gpu_mon.chart);
	gkrellm_set_draw_chart_function(gpu_mon.chart, draw_chart, gpu_mon.chart);

	gpu_mon.krell = gkrellm_create_krell(gpu_mon.chart->panel, gkrellm_krell_panel_piximage(style_id), style);

	gkrellm_monotonic_krell_values(gpu_mon.krell, FALSE);
	gkrellm_set_krell_full_scale(gpu_mon.krell, SCALE_MARK, 1);

	gkrellm_panel_configure(gpu_mon.chart->panel, g_strdup("GPU"), style);
	gkrellm_panel_create(vbox, gpu_plugin_mon_ptr, gpu_mon.chart->panel);

	if(first_create) {
		gtk_signal_connect(GTK_OBJECT(gpu_mon.chart->drawing_area), "expose_event",
				GTK_SIGNAL_FUNC(expose_event), NULL);
		gtk_signal_connect(GTK_OBJECT(gpu_mon.chart->panel->drawing_area), "expose_event",
				GTK_SIGNAL_FUNC(expose_event), NULL);
		gtk_signal_connect(GTK_OBJECT(gpu_mon.chart->drawing_area), "button_press_event",
				GTK_SIGNAL_FUNC(mouseclick_event), NULL);
	}
}

static void create_plugin_tab(GtkWidget *tabs_vbox) {
	GtkWidget *tabs = gtk_notebook_new();
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(tabs), GTK_POS_TOP);
	gtk_box_pack_start(GTK_BOX(tabs_vbox), tabs, TRUE, TRUE, 0);

	GtkWidget *vbox = gkrellm_gtk_framed_notebook_page(tabs, _("Setup"));
	GtkWidget *vbox1 = gkrellm_gtk_framed_vbox(vbox, _("Launch Options"), 4, FALSE, 0, 2);

	GtkWidget *hbox = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox1), hbox, FALSE, FALSE, 0);
	GtkWidget *label = gtk_label_new(_("radeontop options"));
	gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
	gpu_mon.options.radeontop_cmdline_entry = gtk_entry_new();
	if(strlen(gpu_mon.options.radeontop_cmdline) > 0) {
		gtk_entry_set_text(GTK_ENTRY(gpu_mon.options.radeontop_cmdline_entry),
				gpu_mon.options.radeontop_cmdline);
	}
	gtk_box_pack_start(GTK_BOX(hbox), gpu_mon.options.radeontop_cmdline_entry, TRUE, TRUE, 8);

	label = gtk_label_new(_("default options are \"" RADEONTOP_DEFAULT_CMDLINE "\""));
	gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);
}

static void apply_config(void) {
	if(gpu_mon.options.radeontop_cmdline_entry) {
		g_strlcpy(gpu_mon.options.radeontop_cmdline,
				gtk_entry_get_text(GTK_ENTRY(gpu_mon.options.radeontop_cmdline_entry)),
				sizeof(gpu_mon.options.radeontop_cmdline));
	}

	// restart process to apply new args
	pthread_mutex_lock(&gpu_mon.mutex);
	if(gpu_mon.radeontop.subprocess_running) {
		subprocess_terminate(&gpu_mon.radeontop.subprocess);
	}
	pthread_mutex_unlock(&gpu_mon.mutex);
}

static void save_config(FILE *f) {
	gkrellm_save_chartconfig(f, gpu_mon.chart_config, PLUGIN_KEYWORD, NULL);
	fprintf(f, "%s extra_info %d\n", PLUGIN_KEYWORD, gpu_mon.extra_info);
	fprintf(f, "%s radeontop_cmdline %s\n", PLUGIN_KEYWORD, gpu_mon.options.radeontop_cmdline);
}

static void load_config(gchar *arg) {
	gchar config_keyword[32], config_data[CFG_BUFSIZE];
	if(sscanf(arg, "%31s %[^\n]", config_keyword, config_data) != 2)
		return;

	if(!strcmp(config_keyword, "extra_info")) {
		sscanf(config_data, "%d\n", &gpu_mon.extra_info);
	} else if(!strcmp(config_keyword, GKRELLM_CHARTCONFIG_KEYWORD)) {
		gkrellm_load_chartconfig(&gpu_mon.chart_config, config_data, 1);
	} else if(!strcmp(config_keyword, "radeontop_cmdline")) {
		g_strlcpy(gpu_mon.options.radeontop_cmdline, config_data,
				sizeof(gpu_mon.options.radeontop_cmdline));
	}
}

static void update_plugin(void) {
	GkrellmKrell *krell;

	pthread_mutex_lock(&gpu_mon.mutex);

	// reset stats if stale
	time_t current_time = time(NULL);
	if(current_time - gpu_mon.gpu_stats.stats_timestamp > 2) {
		memset(&gpu_mon.gpu_stats, 0, sizeof(gpu_mon.gpu_stats));
	}

	memcpy(&gpu_mon.gpu_stats_copy, &gpu_mon.gpu_stats, sizeof(gpu_mon.gpu_stats));

	pthread_mutex_unlock(&gpu_mon.mutex);

	// used for both chart and krell
	const gulong gpu_pipe = gpu_mon.gpu_stats_copy.gpu_pipe;

	if(GK.second_tick) {
		const gulong shader_clock = gpu_mon.gpu_stats_copy.shader_clock;

		gkrellm_store_chartdata(gpu_mon.chart, 0, shader_clock, gpu_pipe, 0);
		draw_chart(gpu_mon.chart);
	}

	krell = KRELL(gpu_mon.chart->panel);
	gkrellm_update_krell(gpu_mon.chart->panel, krell, gpu_pipe);
	gkrellm_draw_panel_layers(gpu_mon.chart->panel);
}


static GkrellmMonitor gpu_plugin_mon = {
	PLUGIN_NAME,
	0,
	create_plugin,
	update_plugin,
	create_plugin_tab,
	apply_config,
	save_config,
	load_config,
	PLUGIN_KEYWORD,
	NULL, NULL, NULL,
	MON_CPU | MON_INSERT_AFTER,
	NULL, NULL,
};

GkrellmMonitor *gkrellm_init_plugin(void) {
	// set default options
	g_strlcpy(gpu_mon.options.radeontop_cmdline,
			RADEONTOP_DEFAULT_CMDLINE,
			sizeof(gpu_mon.options.radeontop_cmdline));

	gpu_plugin_mon_ptr = &gpu_plugin_mon;
	style_id = gkrellm_add_chart_style(gpu_plugin_mon_ptr, PLUGIN_NAME);
	return gpu_plugin_mon_ptr;
}

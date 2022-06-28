#include <gkrellm2/gkrellm.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define PLUGIN_NAME "gkrellmradeontop"
#define PLUGIN_DESC "show AMD GPU load chart"
#define PLUGIN_STYLE PLUGIN_NAME
#define PLUGIN_KEYWORD PLUGIN_NAME

#define SCALE_MARK 100
#define SCALE_MAX 100
#define MIN_GRID_RES 10
#define MAX_GRID_RES 100

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
		FILE *popen_pipe;
	} radeontop;

	struct gpu_stats gpu_stats, gpu_stats_copy;
} gpu_mon;

static gint style_id;

static GkrellmMonitor *gpu_plugin_mon_ptr;

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

static void *radeontop_thread(void *arg) {
	(void)arg;

	memset(&gpu_mon.gpu_stats, 0, sizeof(gpu_mon.gpu_stats));

	while(1) {
		FILE *p = popen("radeontop -d -", "r");
		if(!p) {
			fprintf(stderr, "can't launch radeontop");
			return NULL;
		}

		gpu_mon.radeontop.popen_pipe = p;

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

		fprintf(stderr, "radeontop is finished, restarting in 5 seconds\n");

		pclose(p);

		sleep(5);
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

static void setup_scaling(GkrellmChartconfig *cf, GkrellmChart *cp) {
	(void)cp;
	gkrellm_set_chartconfig_auto_grid_resolution(cf, FALSE);
	gkrellm_set_chartconfig_grid_resolution(cf, SCALE_MAX / FULL_SCALE_GRIDS);
}

static void create_plugin(GtkWidget *vbox, gint first_create) {
	gpu_mon.enabled = TRUE;

	if(first_create) {
		pthread_mutex_init(&gpu_mon.mutex, NULL);
		pthread_create(&gpu_mon.radeontop.thread, NULL, &radeontop_thread, NULL);

		gpu_mon.vbox = gtk_vbox_new(FALSE, 0);
		gtk_container_add(GTK_CONTAINER(vbox), gpu_mon.vbox);
		gtk_widget_show(gpu_mon.vbox);

		gpu_mon.chart = gkrellm_chart_new0();
		gpu_mon.chart->panel = gkrellm_panel_new0();
	} else {
		gkrellm_destroy_decal_list(gpu_mon.chart->panel);
		gkrellm_destroy_krell_list(gpu_mon.chart->panel);
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

static void create_plugin_tab(GtkWidget *vbox) {
	(void)vbox;
}

static void apply_config(void) {
}

static void save_config(FILE *f) {
	gkrellm_save_chartconfig(f, gpu_mon.chart_config, PLUGIN_KEYWORD, NULL);
	fprintf(f, "%s extra_info %d\n", PLUGIN_KEYWORD, gpu_mon.extra_info);
}

static void load_config(gchar *arg) {
	gchar config_keyword[32], config_data[CFG_BUFSIZE];
	if(sscanf(arg, "%31s %[^\n]", config_keyword, config_data) != 2)
		return;

	if(!strcmp(config_keyword, "extra_info")) {
		sscanf(config_data, "%d\n", &gpu_mon.extra_info);
	} else if(!strcmp(config_data, GKRELLM_CHARTCONFIG_KEYWORD)) {
		gkrellm_load_chartconfig(&gpu_mon.chart_config, config_data, 1);
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
	gpu_plugin_mon_ptr = &gpu_plugin_mon;
	style_id = gkrellm_add_chart_style(gpu_plugin_mon_ptr, PLUGIN_NAME);
	return gpu_plugin_mon_ptr;
}

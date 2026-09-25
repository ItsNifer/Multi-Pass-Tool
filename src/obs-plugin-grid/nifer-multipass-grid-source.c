/* ==========================================================================
 *  Nifer Multipass Grid OBS Source Plugin v2.0 by Nifer
 *  Twitter - @NiferEdits
 *  Adds a "Reshade Grid Pass (Nifer)" source that crops one pass out of the
 *  2x2 grid drawn by NiferMultiPassGrid.fx, with built-in per-pass recording
 *  Works with any ReShade build, no addon support required
 *  Windows only, requires OBS 28+ and links against obs-frontend-api
 * ========================================================================== */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/config-file.h>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void)
{
	return "ReShade multi-pass grid capture (Color / Depth / Normals / Raw) "
	       "with built-in per-pass recording, for NiferMultiPassGrid.fx";
}


// STATE
// ==========================================================================

#define NIFER_MAX_SOURCES 16
#define NIFER_ENC_OBS "obs_settings"
#define NIFER_PLUGIN_VERSION "v2.0"
#define NIFER_SOURCE_ID "nifer_multipass_grid_source"

#define NIFER_PASS_COLOR   0
#define NIFER_PASS_DEPTH   1
#define NIFER_PASS_NORMALS 2
#define NIFER_PASS_RAW     3
#define NIFER_PASS_COUNT   4

#define NIFER_LAYOUT_FOUR   0
#define NIFER_LAYOUT_TWO    1
#define NIFER_LAYOUT_SINGLE 2
#define NIFER_LAYOUT_CYCLE  3

#define NIFER_MARKER_W 32
#define NIFER_MARKER_H 16

struct nifer_source {
	obs_source_t *source;
	obs_weak_source_t *target;
	gs_texrender_t *texrender;
	gs_texrender_t *frames[2];
	gs_texrender_t *markers[2];
	gs_stagesurf_t *stages[2];
	bool stage_valid[2];
	int ring;
	gs_texture_t *cache;
	uint32_t cache_w;
	uint32_t cache_h;
	int no_marker_frames;
	bool marker_warned;
	int hits;
	float report_timer;
	int pass;
	int layout;
	bool right_side;

	bool record_enabled;
	bool prepared;
	char *rec_folder;
	char *rec_prefix;
	char *rec_encoder;
	int rec_fps;
	char rec_path[640];

	obs_view_t *view;
	video_t *video;
	obs_encoder_t *venc;
	obs_encoder_t *aenc;
	obs_output_t *output;
	bool recording;
	bool started_with_main;

	obs_hotkey_id hotkey_id;
};

static struct nifer_source *g_sources[NIFER_MAX_SOURCES];
static size_t g_source_count = 0;
static pthread_mutex_t g_sources_mutex;
static obs_hotkey_id g_record_all_hotkey = OBS_INVALID_HOTKEY_ID;

static const char *const g_pass_names[NIFER_PASS_COUNT] = {
	"color", "depth", "normals", "raw"
};

// GRID TARGET
// ==========================================================================

static void clear_target(struct nifer_source *ctx)
{
	obs_source_t *prev = obs_weak_source_get_source(ctx->target);
	if (prev) {
		obs_source_remove_active_child(ctx->source, prev);
		obs_source_release(prev);
	}
	obs_weak_source_release(ctx->target);
	ctx->target = NULL;
}

static void set_target(struct nifer_source *ctx, const char *name)
{
	clear_target(ctx);

	if (!name || !*name)
		return;

	obs_source_t *next = obs_get_source_by_name(name);
	if (!next)
		return;

	if (next != ctx->source && obs_source_add_active_child(ctx->source, next))
		ctx->target = obs_source_get_weak_source(next);

	obs_source_release(next);
}

static void get_cell(struct nifer_source *ctx, uint32_t tw, uint32_t th,
	uint32_t *x, uint32_t *y, uint32_t *cx, uint32_t *cy)
{
	if (ctx->layout == NIFER_LAYOUT_SINGLE ||
	    ctx->layout == NIFER_LAYOUT_CYCLE) {
		*x = 0;
		*y = 0;
		*cx = tw;
		*cy = th;
		return;
	}

	if (ctx->layout == NIFER_LAYOUT_TWO) {
		*cx = tw / 2;
		*cy = th;
		*x = ctx->right_side ? *cx : 0;
		*y = 0;
		return;
	}

	*cx = tw / 2;
	*cy = th / 2;
	*x = (ctx->pass == NIFER_PASS_DEPTH ||
	      ctx->pass == NIFER_PASS_RAW) ? *cx : 0;
	*y = (ctx->pass == NIFER_PASS_NORMALS ||
	      ctx->pass == NIFER_PASS_RAW) ? *cy : 0;
}

static void get_output_size(struct nifer_source *ctx, uint32_t tw, uint32_t th,
	uint32_t *ow, uint32_t *oh)
{
	if (ctx->layout == NIFER_LAYOUT_TWO) {
		*ow = tw;
		*oh = th;
		return;
	}

	uint32_t x, y, cx, cy;
	get_cell(ctx, tw, th, &x, &y, &cx, &cy);
	*ow = cx;
	*oh = cy;
}

static uint32_t nifer_get_width(void *data)
{
	struct nifer_source *ctx = data;
	obs_source_t *target = obs_weak_source_get_source(ctx->target);
	if (!target)
		return 0;
	const uint32_t tw = obs_source_get_width(target);
	const uint32_t th = obs_source_get_height(target);
	obs_source_release(target);

	uint32_t ow, oh;
	get_output_size(ctx, tw, th, &ow, &oh);
	return ow;
}

static uint32_t nifer_get_height(void *data)
{
	struct nifer_source *ctx = data;
	obs_source_t *target = obs_weak_source_get_source(ctx->target);
	if (!target)
		return 0;
	const uint32_t tw = obs_source_get_width(target);
	const uint32_t th = obs_source_get_height(target);
	obs_source_release(target);

	uint32_t ow, oh;
	get_output_size(ctx, tw, th, &ow, &oh);
	return oh;
}

// ENCODERS
// ==========================================================================

static bool encoder_id_usable(const char *id)
{
	if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
		return false;

	const uint32_t caps = obs_get_encoder_caps(id);
	if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
		return false;

	const char *codec = obs_get_encoder_codec(id);
	if (!codec)
		return false;

	return strcmp(codec, "h264") == 0 ||
	       strcmp(codec, "hevc") == 0 ||
	       strcmp(codec, "av1") == 0;
}

static void apply_encoder_defaults(obs_data_t *settings, const char *enc_id)
{
	obs_data_t *defaults = obs_encoder_defaults(enc_id);
	if (!defaults)
		return;

	for (obs_data_item_t *item = obs_data_first(defaults); item != NULL;
	     obs_data_item_next(&item)) {
		const char *name = obs_data_item_get_name(item);
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			obs_data_set_default_string(settings, name,
				obs_data_item_get_string(item));
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_DOUBLE)
				obs_data_set_default_double(settings, name,
					obs_data_item_get_double(item));
			else
				obs_data_set_default_int(settings, name,
					obs_data_item_get_int(item));
			break;
		case OBS_DATA_BOOLEAN:
			obs_data_set_default_bool(settings, name,
				obs_data_item_get_bool(item));
			break;
		default:
			break;
		}
	}

	obs_data_release(defaults);
}

static const char *find_encoder_id(const char *const *candidates)
{
	for (size_t i = 0; candidates[i] != NULL; i++) {
		if (obs_get_encoder_codec(candidates[i]) != NULL)
			return candidates[i];
	}
	return NULL;
}

static const char *simple_alias_to_id(const char *alias)
{
	static const char *const x264[] = { "obs_x264", NULL };
	static const char *const qsv[] = { "obs_qsv11_v2", "obs_qsv11", NULL };
	static const char *const qsv_hevc[] = { "obs_qsv11_hevc", NULL };
	static const char *const qsv_av1[] = { "obs_qsv11_av1", NULL };
	static const char *const nvenc[] = { "obs_nvenc_h264", "jim_nvenc", "ffmpeg_nvenc", NULL };
	static const char *const nvenc_hevc[] = { "obs_nvenc_hevc", "jim_hevc_nvenc", NULL };
	static const char *const nvenc_av1[] = { "obs_nvenc_av1", "jim_av1_nvenc", NULL };
	static const char *const amd[] = { "h264_texture_amf", NULL };
	static const char *const amd_hevc[] = { "h265_texture_amf", NULL };
	static const char *const amd_av1[] = { "av1_texture_amf", NULL };

	if (!alias)
		return NULL;
	if (strcmp(alias, "x264") == 0 || strcmp(alias, "x264_lowcpu") == 0)
		return find_encoder_id(x264);
	if (strcmp(alias, "qsv") == 0)
		return find_encoder_id(qsv);
	if (strcmp(alias, "qsv_hevc") == 0)
		return find_encoder_id(qsv_hevc);
	if (strcmp(alias, "qsv_av1") == 0)
		return find_encoder_id(qsv_av1);
	if (strcmp(alias, "nvenc") == 0)
		return find_encoder_id(nvenc);
	if (strcmp(alias, "nvenc_hevc") == 0)
		return find_encoder_id(nvenc_hevc);
	if (strcmp(alias, "nvenc_av1") == 0)
		return find_encoder_id(nvenc_av1);
	if (strcmp(alias, "amd") == 0)
		return find_encoder_id(amd);
	if (strcmp(alias, "amd_hevc") == 0)
		return find_encoder_id(amd_hevc);
	if (strcmp(alias, "amd_av1") == 0)
		return find_encoder_id(amd_av1);
	return NULL;
}

static obs_encoder_t *encoder_from_live_output(void)
{
	obs_encoder_t *venc = NULL;
	obs_output_t *main_out = obs_frontend_get_recording_output();
	obs_encoder_t *main_enc = main_out ?
		obs_output_get_video_encoder(main_out) : NULL;

	if (main_enc) {
		obs_data_t *vs = obs_encoder_get_settings(main_enc);
		venc = obs_video_encoder_create(obs_encoder_get_id(main_enc),
			"nifer_grid_venc", vs, NULL);
		obs_data_release(vs);
	}
	if (main_out)
		obs_output_release(main_out);

	return venc;
}

static obs_encoder_t *encoder_from_advanced_config(config_t *cfg)
{
	const char *enc_id = config_get_string(cfg, "AdvOut", "RecEncoder");
	const char *json_name = "recordEncoder.json";

	if (!enc_id || !*enc_id || strcmp(enc_id, "none") == 0) {
		enc_id = config_get_string(cfg, "AdvOut", "Encoder");
		json_name = "streamEncoder.json";
	}
	if (!enc_id || !*enc_id)
		return NULL;

	obs_data_t *vs = NULL;
	char *profile_path = obs_frontend_get_current_profile_path();
	if (profile_path) {
		struct dstr path = {0};
		dstr_printf(&path, "%s/%s", profile_path, json_name);
		vs = obs_data_create_from_json_file(path.array);
		dstr_free(&path);
		bfree(profile_path);
	}

	obs_encoder_t *venc = obs_video_encoder_create(enc_id,
		"nifer_grid_venc", vs, NULL);
	if (vs)
		obs_data_release(vs);

	return venc;
}

static obs_encoder_t *encoder_from_simple_config(config_t *cfg)
{
	const char *quality = config_get_string(cfg, "SimpleOutput", "RecQuality");
	const bool stream_quality = quality && strcmp(quality, "Stream") == 0;

	const char *alias = config_get_string(cfg, "SimpleOutput",
		stream_quality ? "StreamEncoder" : "RecEncoder");
	const char *enc_id = simple_alias_to_id(alias);
	if (!enc_id)
		return NULL;

	obs_data_t *vs = obs_data_create();
	if (stream_quality) {
		obs_data_set_string(vs, "rate_control", "CBR");
		obs_data_set_int(vs, "bitrate",
			(int)config_get_int(cfg, "SimpleOutput", "VideoBitrate"));
	} else {
		int q = 23;
		if (quality && strcmp(quality, "HQ") == 0)
			q = 16;
		if (quality && strcmp(quality, "Lossless") == 0)
			q = strcmp(enc_id, "obs_x264") == 0 ? 0 : 10;
		obs_data_set_string(vs, "rate_control",
			strcmp(enc_id, "obs_x264") == 0 ? "CRF" : "CQP");
		obs_data_set_int(vs, "crf", q);
		obs_data_set_int(vs, "cqp", q);
		obs_data_set_int(vs, "qp", q);
		obs_data_set_int(vs, "cq_level", q);
	}

	obs_encoder_t *venc = obs_video_encoder_create(enc_id,
		"nifer_grid_venc", vs, NULL);
	obs_data_release(vs);

	return venc;
}

static obs_encoder_t *create_record_encoder(struct nifer_source *ctx)
{
	obs_encoder_t *venc = NULL;
	const char *choice = (ctx->rec_encoder && *ctx->rec_encoder) ?
		ctx->rec_encoder : NIFER_ENC_OBS;

	if (strcmp(choice, NIFER_ENC_OBS) == 0) {
		venc = encoder_from_live_output();

		if (!venc) {
			config_t *cfg = obs_frontend_get_profile_config();
			const char *mode = cfg ?
				config_get_string(cfg, "Output", "Mode") : NULL;

			if (mode && strcmp(mode, "Advanced") == 0)
				venc = encoder_from_advanced_config(cfg);
			else if (cfg)
				venc = encoder_from_simple_config(cfg);
		}

		if (!venc)
			blog(LOG_WARNING,
			     "[nifer-grid] could not read the OBS recording "
			     "encoder settings, using x264 defaults");
	} else {
		obs_data_t *vs = obs_source_get_settings(ctx->source);
		venc = obs_video_encoder_create(choice, "nifer_grid_venc",
			vs, NULL);
		obs_data_release(vs);

		if (!venc)
			blog(LOG_WARNING,
			     "[nifer-grid] encoder '%s' unavailable, falling "
			     "back to x264", choice);
	}

	if (!venc)
		venc = obs_video_encoder_create("obs_x264", "nifer_grid_venc",
			NULL, NULL);

	return venc;
}

// SOURCE RECORD
// ==========================================================================

static void release_source_record(struct nifer_source *ctx)
{
	if (ctx->output) {
		obs_output_release(ctx->output);
		ctx->output = NULL;
	}
	if (ctx->venc) {
		obs_encoder_release(ctx->venc);
		ctx->venc = NULL;
	}
	if (ctx->aenc) {
		obs_encoder_release(ctx->aenc);
		ctx->aenc = NULL;
	}
	if (ctx->view) {
		obs_view_remove(ctx->view);
		obs_view_set_source(ctx->view, 0, NULL);
		obs_view_destroy(ctx->view);
		ctx->view = NULL;
		ctx->video = NULL;
	}
	ctx->recording = false;
	ctx->started_with_main = false;
	ctx->prepared = false;
	ctx->rec_path[0] = '\0';
}

static void signal_stop_source_record(struct nifer_source *ctx)
{
	if (ctx->output && ctx->recording)
		obs_output_stop(ctx->output);
}

static void stop_source_record(struct nifer_source *ctx)
{
	signal_stop_source_record(ctx);
	release_source_record(ctx);
}

static bool prepare_source_record(struct nifer_source *ctx, bool from_main)
{
	if (ctx->recording || ctx->prepared)
		return false;
	if (from_main && !ctx->record_enabled)
		return false;

	if (!obs_source_active(ctx->source)) {
		blog(LOG_INFO,
		     "[nifer-grid] pass '%s' is hidden, not recording it",
		     g_pass_names[ctx->pass]);
		return false;
	}

	const uint32_t width = nifer_get_width(ctx);
	const uint32_t height = nifer_get_height(ctx);
	if (width == 0 || height == 0) {
		blog(LOG_WARNING,
		     "[nifer-grid] pass '%s' has no grid capture source yet, "
		     "skipping its recording", g_pass_names[ctx->pass]);
		return false;
	}

	if (!ctx->rec_folder || !*ctx->rec_folder) {
		blog(LOG_WARNING,
		     "[nifer-grid] no recording folder set for pass '%s', "
		     "skipping its recording", g_pass_names[ctx->pass]);
		return false;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return false;
	ovi.base_width = width;
	ovi.base_height = height;
	ovi.output_width = width;
	ovi.output_height = height;
	if (ctx->rec_fps > 0) {
		ovi.fps_num = (uint32_t)ctx->rec_fps;
		ovi.fps_den = 1;
	}

	ctx->view = obs_view_create();
	obs_view_set_source(ctx->view, 0, ctx->source);
	ctx->video = obs_view_add2(ctx->view, &ovi);
	if (!ctx->video) {
		blog(LOG_ERROR, "[nifer-grid] failed to create video mix");
		release_source_record(ctx);
		return false;
	}

	ctx->venc = create_record_encoder(ctx);
	if (!ctx->venc) {
		blog(LOG_ERROR, "[nifer-grid] failed to create encoder");
		release_source_record(ctx);
		return false;
	}
	obs_encoder_set_video(ctx->venc, ctx->video);

	ctx->aenc = obs_audio_encoder_create("ffmpeg_aac", "nifer_grid_aenc",
		NULL, 0, NULL);
	if (ctx->aenc)
		obs_encoder_set_audio(ctx->aenc, obs_get_audio());

	const char *prefix = (ctx->rec_prefix && *ctx->rec_prefix) ?
		ctx->rec_prefix : g_pass_names[ctx->pass];

	char timebuf[64];
	time_t now = time(NULL);
	struct tm tm_now;
	localtime_s(&tm_now, &now);
	strftime(timebuf, sizeof(timebuf), "%Y-%m-%d_%H-%M-%S", &tm_now);

	snprintf(ctx->rec_path, sizeof(ctx->rec_path), "%s/%s_%s.mkv",
		ctx->rec_folder, prefix, timebuf);

	obs_data_t *os = obs_data_create();
	obs_data_set_string(os, "path", ctx->rec_path);
	ctx->output = obs_output_create("ffmpeg_muxer", "nifer_grid_out", os,
		NULL);
	obs_data_release(os);

	if (!ctx->output) {
		blog(LOG_ERROR, "[nifer-grid] failed to create output");
		release_source_record(ctx);
		return false;
	}

	obs_output_set_video_encoder(ctx->output, ctx->venc);
	if (ctx->aenc)
		obs_output_set_audio_encoder(ctx->output, ctx->aenc, 0);

	ctx->prepared = true;
	ctx->started_with_main = from_main;
	return true;
}

static void arm_source_record(struct nifer_source *ctx)
{
	if (!ctx->prepared || ctx->recording)
		return;

	if (!obs_output_start(ctx->output)) {
		release_source_record(ctx);
		return;
	}

	ctx->recording = true;
}

static bool start_all_records(bool from_main)
{
	bool any_prepared = false;

	for (size_t i = 0; i < g_source_count; i++) {
		if (prepare_source_record(g_sources[i], from_main))
			any_prepared = true;
	}

	if (!any_prepared)
		return false;

	for (size_t i = 0; i < g_source_count; i++)
		arm_source_record(g_sources[i]);

	for (size_t i = 0; i < g_source_count; i++) {
		struct nifer_source *ctx = g_sources[i];
		if (ctx->recording)
			blog(LOG_INFO, "[nifer-grid] recording pass '%s' -> %s",
			     g_pass_names[ctx->pass], ctx->rec_path);
	}

	return true;
}

static void stop_all_records(bool only_main)
{
	for (size_t i = 0; i < g_source_count; i++) {
		if (only_main && !g_sources[i]->started_with_main)
			continue;
		signal_stop_source_record(g_sources[i]);
	}

	for (size_t i = 0; i < g_source_count; i++) {
		struct nifer_source *ctx = g_sources[i];
		if (only_main && !ctx->started_with_main)
			continue;
		if (!ctx->prepared && !ctx->recording && !ctx->output)
			continue;
		release_source_record(ctx);
	}
}

static void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);

	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		pthread_mutex_lock(&g_sources_mutex);
		start_all_records(true);
		pthread_mutex_unlock(&g_sources_mutex);
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPING ||
	           event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		pthread_mutex_lock(&g_sources_mutex);
		stop_all_records(true);
		pthread_mutex_unlock(&g_sources_mutex);
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		pthread_mutex_lock(&g_sources_mutex);
		stop_all_records(false);
		pthread_mutex_unlock(&g_sources_mutex);
	}
}

// HOTKEYS
// ==========================================================================

#define NIFER_BEEP_START_FREQ 880
#define NIFER_BEEP_STOP_FREQ  440
#define NIFER_BEEP_MS         150

static DWORD WINAPI beep_thread(LPVOID param)
{
	Beep((DWORD)(uintptr_t)param, NIFER_BEEP_MS);
	return 0;
}

static void play_beep(bool start)
{
	HANDLE thread = CreateThread(NULL, 0, beep_thread,
		(LPVOID)(uintptr_t)(start ? NIFER_BEEP_START_FREQ :
		                            NIFER_BEEP_STOP_FREQ), 0, NULL);
	if (thread)
		CloseHandle(thread);
}

static void record_all_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	pthread_mutex_lock(&g_sources_mutex);

	bool any_recording = false;
	for (size_t i = 0; i < g_source_count; i++) {
		if (g_sources[i]->recording)
			any_recording = true;
	}

	if (any_recording) {
		stop_all_records(false);
		play_beep(false);
	} else if (start_all_records(false)) {
		play_beep(true);
	}

	pthread_mutex_unlock(&g_sources_mutex);
}

static void record_source_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct nifer_source *ctx = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	pthread_mutex_lock(&g_sources_mutex);

	if (ctx->recording) {
		stop_source_record(ctx);
		play_beep(false);
	} else if (prepare_source_record(ctx, false)) {
		arm_source_record(ctx);
		if (ctx->recording) {
			play_beep(true);
			blog(LOG_INFO, "[nifer-grid] recording pass '%s' -> %s",
			     g_pass_names[ctx->pass], ctx->rec_path);
		}
	}

	pthread_mutex_unlock(&g_sources_mutex);
}

// SOURCE CALLBACKS
// ==========================================================================

static const char *nifer_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Reshade Grid Pass (Nifer)";
}

static void nifer_update(void *data, obs_data_t *settings)
{
	struct nifer_source *ctx = data;

	int pass = (int)obs_data_get_int(settings, "pass");
	if (pass < 0 || pass >= NIFER_PASS_COUNT)
		pass = NIFER_PASS_COLOR;
	ctx->pass = pass;

	int layout = (int)obs_data_get_int(settings, "layout");
	if (layout < 0 || layout > NIFER_LAYOUT_CYCLE)
		layout = NIFER_LAYOUT_FOUR;
	ctx->layout = layout;
	ctx->right_side = obs_data_get_bool(settings, "right_side");

	set_target(ctx, obs_data_get_string(settings, "grid_source"));

	ctx->record_enabled = obs_data_get_bool(settings, "record_enabled");
	ctx->rec_fps = (int)obs_data_get_int(settings, "rec_fps");

	bfree(ctx->rec_folder);
	ctx->rec_folder = bstrdup(obs_data_get_string(settings, "rec_folder"));
	bfree(ctx->rec_prefix);
	ctx->rec_prefix = bstrdup(obs_data_get_string(settings, "rec_prefix"));
	bfree(ctx->rec_encoder);
	ctx->rec_encoder = bstrdup(obs_data_get_string(settings, "rec_encoder"));
}

static void *nifer_create(obs_data_t *settings, obs_source_t *source)
{
	struct nifer_source *ctx = bzalloc(sizeof(struct nifer_source));
	ctx->source = source;

	obs_enter_graphics();
	ctx->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (int i = 0; i < 2; i++) {
		ctx->frames[i] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		ctx->markers[i] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		ctx->stages[i] = gs_stagesurface_create(NIFER_MARKER_W,
			NIFER_MARKER_H, GS_RGBA);
	}
	obs_leave_graphics();

	ctx->hotkey_id = obs_hotkey_register_source(source,
		"nifer_grid.record_toggle",
		"Record this Grid Pass source (toggle)",
		record_source_hotkey, ctx);

	nifer_update(ctx, settings);

	pthread_mutex_lock(&g_sources_mutex);
	if (g_source_count < NIFER_MAX_SOURCES)
		g_sources[g_source_count++] = ctx;
	pthread_mutex_unlock(&g_sources_mutex);

	return ctx;
}

static void nifer_destroy(void *data)
{
	struct nifer_source *ctx = data;

	pthread_mutex_lock(&g_sources_mutex);
	for (size_t i = 0; i < g_source_count; i++) {
		if (g_sources[i] == ctx) {
			g_sources[i] = g_sources[--g_source_count];
			break;
		}
	}
	pthread_mutex_unlock(&g_sources_mutex);

	if (ctx->hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(ctx->hotkey_id);

	stop_source_record(ctx);
	clear_target(ctx);

	obs_enter_graphics();
	if (ctx->texrender)
		gs_texrender_destroy(ctx->texrender);
	for (int i = 0; i < 2; i++) {
		if (ctx->frames[i])
			gs_texrender_destroy(ctx->frames[i]);
		if (ctx->markers[i])
			gs_texrender_destroy(ctx->markers[i]);
		if (ctx->stages[i])
			gs_stagesurface_destroy(ctx->stages[i]);
	}
	if (ctx->cache)
		gs_texture_destroy(ctx->cache);
	obs_leave_graphics();

	bfree(ctx->rec_folder);
	bfree(ctx->rec_prefix);
	bfree(ctx->rec_encoder);
	bfree(ctx);
}

static void render_target_to(gs_texrender_t *dst, obs_source_t *target,
	uint32_t tw, uint32_t th)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, tw, th))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)tw, 0.0f, (float)th, -100.0f, 100.0f);
	obs_source_video_render(target);
	gs_texrender_end(dst);
}

static void draw_texture_region(gs_texture_t *tex, uint32_t x, uint32_t y,
	uint32_t cx, uint32_t cy)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, tex);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite_subregion(tex, 0, x, y, cx, cy);
}

static int classify_marker(const uint8_t *pixels, uint32_t linesize)
{
	const uint8_t *sig = pixels + (NIFER_MARKER_H / 2) * linesize + 8 * 4;
	const uint8_t *tag = pixels + (NIFER_MARKER_H / 2) * linesize + 24 * 4;

	if (sig[0] < 200 || sig[1] > 60 || sig[2] < 200)
		return -1;

	if (tag[0] > 200 && tag[1] > 200 && tag[2] < 60)
		return NIFER_PASS_RAW;
	if (tag[0] > 200 && tag[1] < 60 && tag[2] < 60)
		return NIFER_PASS_COLOR;
	if (tag[0] < 60 && tag[1] > 200 && tag[2] < 60)
		return NIFER_PASS_DEPTH;
	if (tag[0] < 60 && tag[1] < 60 && tag[2] > 200)
		return NIFER_PASS_NORMALS;

	return -1;
}

static void ensure_cache(struct nifer_source *ctx, uint32_t tw, uint32_t th)
{
	if (ctx->cache && ctx->cache_w == tw && ctx->cache_h == th)
		return;

	if (ctx->cache)
		gs_texture_destroy(ctx->cache);

	ctx->cache = gs_texture_create(tw, th, GS_RGBA, 1, NULL,
		GS_RENDER_TARGET);
	ctx->cache_w = tw;
	ctx->cache_h = th;
}

static void render_cycle(struct nifer_source *ctx, obs_source_t *target,
	uint32_t tw, uint32_t th)
{
	const int cur = ctx->ring;
	const int prev = 1 - cur;

	render_target_to(ctx->frames[cur], target, tw, th);

	gs_texture_t *frame = gs_texrender_get_texture(ctx->frames[cur]);
	if (frame) {
		gs_texrender_reset(ctx->markers[cur]);
		if (gs_texrender_begin(ctx->markers[cur], NIFER_MARKER_W,
				NIFER_MARKER_H)) {
			gs_ortho(0.0f, (float)NIFER_MARKER_W, 0.0f,
				(float)NIFER_MARKER_H, -100.0f, 100.0f);
			draw_texture_region(frame, 0, 0, NIFER_MARKER_W,
				NIFER_MARKER_H);
			gs_texrender_end(ctx->markers[cur]);
		}

		gs_texture_t *marker = gs_texrender_get_texture(ctx->markers[cur]);
		if (marker) {
			gs_stage_texture(ctx->stages[cur], marker);
			ctx->stage_valid[cur] = true;
		}
	}

	if (ctx->stage_valid[prev]) {
		uint8_t *pixels = NULL;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(ctx->stages[prev], &pixels, &linesize)) {
			const int detected = classify_marker(pixels, linesize);
			gs_stagesurface_unmap(ctx->stages[prev]);

			if (detected < 0) {
				if (++ctx->no_marker_frames == 240 &&
				    !ctx->marker_warned) {
					ctx->marker_warned = true;
					blog(LOG_WARNING,
					     "[nifer-grid] no frame cycle marker found. "
					     "Set the shader Layout to 'Frame cycle' "
					     "and check that the capture source shows "
					     "the game unscaled and uncropped");
				}
			} else {
				ctx->no_marker_frames = 0;
			}

			if (detected == ctx->pass) {
				gs_texture_t *hit =
					gs_texrender_get_texture(ctx->frames[prev]);
				if (hit) {
					ensure_cache(ctx, tw, th);
					if (ctx->cache) {
						gs_copy_texture(ctx->cache, hit);
						ctx->hits++;
					}
				}
			}
		}
	}

	ctx->ring = prev;

	if (ctx->cache)
		draw_texture_region(ctx->cache, 0, 0, ctx->cache_w, ctx->cache_h);
}

static void nifer_tick(void *data, float seconds)
{
	struct nifer_source *ctx = data;

	if (ctx->layout != NIFER_LAYOUT_CYCLE)
		return;

	ctx->report_timer += seconds;
	if (ctx->report_timer < 5.0f)
		return;

	blog(LOG_INFO, "[nifer-grid] pass '%s' is updating at %.1f fps",
	     g_pass_names[ctx->pass], (float)ctx->hits / ctx->report_timer);

	ctx->hits = 0;
	ctx->report_timer = 0.0f;
}

static void nifer_render(void *data, gs_effect_t *unused_effect)
{
	struct nifer_source *ctx = data;
	UNUSED_PARAMETER(unused_effect);

	obs_source_t *target = obs_weak_source_get_source(ctx->target);
	if (!target)
		return;

	const uint32_t tw = obs_source_get_width(target);
	const uint32_t th = obs_source_get_height(target);
	if (tw < 2 || th < 2) {
		obs_source_release(target);
		return;
	}

	if (ctx->layout == NIFER_LAYOUT_CYCLE) {
		render_cycle(ctx, target, tw, th);
		obs_source_release(target);
		return;
	}

	render_target_to(ctx->texrender, target, tw, th);
	obs_source_release(target);

	gs_texture_t *tex = gs_texrender_get_texture(ctx->texrender);
	if (!tex)
		return;

	uint32_t x, y, cx, cy, ow, oh;
	get_cell(ctx, tw, th, &x, &y, &cx, &cy);
	get_output_size(ctx, tw, th, &ow, &oh);

	if (cx == 0 || cy == 0)
		return;

	gs_matrix_push();
	gs_matrix_scale3f((float)ow / (float)cx, (float)oh / (float)cy, 1.0f);
	draw_texture_region(tex, x, y, cx, cy);
	gs_matrix_pop();
}

// PROPERTIES UI
// ==========================================================================

static bool add_source_to_list(void *param, obs_source_t *src)
{
	obs_property_t *prop = param;

	if (strcmp(obs_source_get_id(src), NIFER_SOURCE_ID) == 0)
		return true;
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) == 0)
		return true;

	const char *name = obs_source_get_name(src);
	obs_property_list_add_string(prop, name, name);
	return true;
}

static bool encoder_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	const char *enc_id = obs_data_get_string(settings, "rec_encoder");

	obs_properties_remove_by_name(props, "encoder_group");

	if (*enc_id && strcmp(enc_id, NIFER_ENC_OBS) != 0) {
		obs_properties_t *enc_props = obs_get_encoder_properties(enc_id);
		if (enc_props) {
			apply_encoder_defaults(settings, enc_id);
			obs_properties_add_group(props, "encoder_group",
				"Encoder Settings", OBS_GROUP_NORMAL, enc_props);
		}
	}

	return true;
}

static bool layout_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	const int layout = (int)obs_data_get_int(settings, "layout");

	obs_property_t *side = obs_properties_get(props, "right_side");
	if (side)
		obs_property_set_visible(side, layout == NIFER_LAYOUT_TWO);

	return true;
}

static void nifer_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "layout", NIFER_LAYOUT_FOUR);
	obs_data_set_default_bool(settings, "right_side", false);
	obs_data_set_default_int(settings, "pass", NIFER_PASS_COLOR);
	obs_data_set_default_bool(settings, "record_enabled", false);
	obs_data_set_default_string(settings, "rec_encoder", NIFER_ENC_OBS);
	obs_data_set_default_int(settings, "rec_fps", 0);
}

static obs_properties_t *nifer_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "title_text",
		"Nifer Multipass Grid " NIFER_PLUGIN_VERSION " by Nifer\n"
		"Crops one pass out of the layout drawn by "
		"NiferMultiPassGrid.fx and records it to its own file.\n"
		"No addon build of ReShade required. Point 'Grid capture "
		"source' at a Game Capture of the game, and set Layout to "
		"match the shader.", OBS_TEXT_INFO);

	obs_property_t *target = obs_properties_add_list(props, "grid_source",
		"Grid capture source", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(target, "", "");
	obs_enum_sources(add_source_to_list, target);

	obs_property_t *layout = obs_properties_add_list(props, "layout",
		"Layout (match the shader)", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(layout, "Four passes (2x2)", NIFER_LAYOUT_FOUR);
	obs_property_list_add_int(layout, "Two passes (side by side)", NIFER_LAYOUT_TWO);
	obs_property_list_add_int(layout, "One pass (fullscreen)", NIFER_LAYOUT_SINGLE);
	obs_property_list_add_int(layout, "Frame cycle (full resolution)", NIFER_LAYOUT_CYCLE);
	obs_property_set_modified_callback(layout, layout_changed);

	obs_property_t *side = obs_properties_add_list(props, "right_side",
		"Side", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_BOOL);
	obs_property_list_add_bool(side, "Left", false);
	obs_property_list_add_bool(side, "Right", true);

	obs_property_t *list = obs_properties_add_list(props, "pass", "Pass",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "Color (without shaders)", NIFER_PASS_COLOR);
	obs_property_list_add_int(list, "Depth", NIFER_PASS_DEPTH);
	obs_property_list_add_int(list, "Normals", NIFER_PASS_NORMALS);
	obs_property_list_add_int(list, "Color (with shaders)", NIFER_PASS_RAW);

	obs_properties_add_bool(props, "record_enabled", "Record this pass");

	obs_properties_add_path(props, "rec_folder", "Recording folder",
		OBS_PATH_DIRECTORY, NULL, NULL);

	obs_properties_add_text(props, "rec_prefix",
		"Filename prefix (empty = pass name)", OBS_TEXT_DEFAULT);

	obs_property_t *fps = obs_properties_add_list(props, "rec_fps",
		"Frame rate", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "Use OBS recording settings", 0);
	obs_property_list_add_int(fps, "24", 24);
	obs_property_list_add_int(fps, "30", 30);
	obs_property_list_add_int(fps, "48", 48);
	obs_property_list_add_int(fps, "60", 60);
	obs_property_list_add_int(fps, "120", 120);
	obs_property_list_add_int(fps, "144", 144);
	obs_property_list_add_int(fps, "240", 240);

	obs_property_t *enc = obs_properties_add_list(props, "rec_encoder",
		"Encoder", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(enc, "Use OBS recording settings",
		NIFER_ENC_OBS);

	const char *id;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (!encoder_id_usable(id))
			continue;
		struct dstr label = {0};
		dstr_printf(&label, "%s [%s]",
			obs_encoder_get_display_name(id),
			obs_get_encoder_codec(id));
		obs_property_list_add_string(enc, label.array, id);
		dstr_free(&label);
	}

	obs_property_set_modified_callback(enc, encoder_changed);

	return props;
}

// MODULE
// ==========================================================================

static struct obs_source_info nifer_source_info = {
	.id = NIFER_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
	                OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = nifer_get_name,
	.create = nifer_create,
	.destroy = nifer_destroy,
	.update = nifer_update,
	.get_defaults = nifer_get_defaults,
	.get_properties = nifer_get_properties,
	.video_tick = nifer_tick,
	.video_render = nifer_render,
	.get_width = nifer_get_width,
	.get_height = nifer_get_height,
};

bool obs_module_load(void)
{
	pthread_mutex_init(&g_sources_mutex, NULL);
	obs_register_source(&nifer_source_info);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	g_record_all_hotkey = obs_hotkey_register_frontend(
		"nifer_grid.record_all",
		"Record all Grid Pass sources (toggle)",
		record_all_hotkey, NULL);
	return true;
}

void obs_module_unload(void)
{
	if (g_record_all_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_record_all_hotkey);
	obs_frontend_remove_event_callback(on_frontend_event, NULL);

	pthread_mutex_lock(&g_sources_mutex);
	for (size_t i = 0; i < g_source_count; i++)
		stop_source_record(g_sources[i]);
	pthread_mutex_unlock(&g_sources_mutex);

	pthread_mutex_destroy(&g_sources_mutex);
}

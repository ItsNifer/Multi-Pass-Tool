/* ==========================================================================
 *  Nifer Multipass OBS Source Plugin v2.0 by Nifer
 *  Twitter - @NiferEdits
 *  Adds a "Reshade Pass Capture (Nifer)" source showing one pass shared by
 *  the Nifer Multipass ReShade addon, with built-in per pass recording
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
#include "../shared/nifer_multipass_shared.h"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void)
{
	return "ReShade multi-pass capture source (Color / Depth / Normals) "
	       "with built-in per-pass recording, for the Nifer Multipass "
	       "ReShade addon";
}

// STATE
// ==========================================================================

#define NIFER_MAX_SOURCES 16
#define NIFER_ENC_OBS "obs_settings"
#define NIFER_PLUGIN_VERSION "v2.0"

struct nifer_source {
	obs_source_t *source;
	int pass;

	HANDLE shmem;
	volatile nifer_shared_info *info;

	gs_texture_t *texture;
	uint32_t opened_handle;
	uint32_t opened_generation;

	float retry_timer;

	bool record_enabled;
	bool exr_enabled;
	bool exr_recording;
	bool exr_pending;
	bool prepared;
	char rec_path[640];
	char *rec_folder;
	char *rec_prefix;
	char *rec_encoder;
	int rec_fps;

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

// SHARED MEMORY AND TEXTURE
// ==========================================================================

static void close_shmem(struct nifer_source *ctx)
{
	if (ctx->info) {
		UnmapViewOfFile((void *)ctx->info);
		ctx->info = NULL;
	}
	if (ctx->shmem) {
		CloseHandle(ctx->shmem);
		ctx->shmem = NULL;
	}
}

static void free_texture(struct nifer_source *ctx)
{
	if (ctx->texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->texture);
		obs_leave_graphics();
		ctx->texture = NULL;
	}
	ctx->opened_handle = 0;
}

static bool try_open_shmem(struct nifer_source *ctx)
{
	ctx->shmem = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE,
		NIFER_SHARED_NAME);
	if (!ctx->shmem)
		return false;

	ctx->info = (volatile nifer_shared_info *)MapViewOfFile(
		ctx->shmem, FILE_MAP_ALL_ACCESS, 0, 0,
		sizeof(nifer_shared_info));
	if (!ctx->info) {
		CloseHandle(ctx->shmem);
		ctx->shmem = NULL;
		return false;
	}

	if (ctx->info->magic != NIFER_MAGIC ||
	    ctx->info->version != NIFER_VERSION) {
		close_shmem(ctx);
		return false;
	}

	ctx->info->obs_pid = GetCurrentProcessId();
	return true;
}

static uint32_t nifer_get_width(void *data)
{
	struct nifer_source *ctx = data;
	if (!ctx->info || !ctx->info->active)
		return 0;
	return ctx->info->passes[ctx->pass].width;
}

static uint32_t nifer_get_height(void *data)
{
	struct nifer_source *ctx = data;
	if (!ctx->info || !ctx->info->active)
		return 0;
	return ctx->info->passes[ctx->pass].height;
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
			"nifer_rec_venc", vs, NULL);
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
		"nifer_rec_venc", vs, NULL);
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
		"nifer_rec_venc", vs, NULL);
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
			     "[nifer-multipass] could not read the OBS "
			     "recording encoder settings, using x264 defaults");
	} else {
		obs_data_t *vs = obs_source_get_settings(ctx->source);
		venc = obs_video_encoder_create(choice, "nifer_rec_venc",
			vs, NULL);
		obs_data_release(vs);

		if (!venc)
			blog(LOG_WARNING,
			     "[nifer-multipass] encoder '%s' unavailable, "
			     "falling back to x264", choice);
	}

	if (!venc)
		venc = obs_video_encoder_create("obs_x264", "nifer_rec_venc",
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
	ctx->exr_pending = false;
	ctx->rec_path[0] = '\0';
}

static void signal_stop_source_record(struct nifer_source *ctx)
{
	if (ctx->exr_recording) {
		if (ctx->info)
			((nifer_shared_info *)ctx->info)->exr.active = 0;
		ctx->exr_recording = false;
	}
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
		     "[nifer-multipass] pass '%s' is hidden, not recording it",
		     g_pass_names[ctx->pass]);
		return false;
	}

	const uint32_t width = nifer_get_width(ctx);
	const uint32_t height = nifer_get_height(ctx);
	if (width == 0 || height == 0) {
		blog(LOG_WARNING,
		     "[nifer-multipass] pass '%s' has no frames yet (game not "
		     "running?), skipping its recording",
		     g_pass_names[ctx->pass]);
		return false;
	}

	if (!ctx->rec_folder || !*ctx->rec_folder) {
		blog(LOG_WARNING,
		     "[nifer-multipass] no recording folder set for pass '%s', "
		     "skipping its recording",
		     g_pass_names[ctx->pass]);
		return false;
	}

	if (ctx->pass == NIFER_PASS_DEPTH && ctx->exr_enabled) {
		ctx->exr_pending = true;
		ctx->prepared = true;
		ctx->started_with_main = from_main;
		return true;
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
		blog(LOG_ERROR, "[nifer-multipass] failed to create video mix");
		release_source_record(ctx);
		return false;
	}

	ctx->venc = create_record_encoder(ctx);
	if (!ctx->venc) {
		blog(LOG_ERROR, "[nifer-multipass] failed to create encoder");
		release_source_record(ctx);
		return false;
	}
	obs_encoder_set_video(ctx->venc, ctx->video);

	ctx->aenc = obs_audio_encoder_create("ffmpeg_aac", "nifer_rec_aenc",
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
	ctx->output = obs_output_create("ffmpeg_muxer", "nifer_rec_out", os,
		NULL);
	obs_data_release(os);

	if (!ctx->output) {
		blog(LOG_ERROR, "[nifer-multipass] failed to create output");
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

	if (ctx->exr_pending) {
		nifer_shared_info *info = (nifer_shared_info *)ctx->info;
		strncpy_s(info->exr.folder, sizeof(info->exr.folder),
			ctx->rec_folder, _TRUNCATE);
		info->exr.active = 1;
		ctx->exr_recording = true;
		ctx->exr_pending = false;
		ctx->recording = true;
		return;
	}

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
		if (!ctx->recording)
			continue;
		if (ctx->exr_recording)
			blog(LOG_INFO,
			     "[nifer-multipass] EXR depth sequence started -> %s",
			     ctx->rec_folder);
		else
			blog(LOG_INFO,
			     "[nifer-multipass] recording pass '%s' -> %s",
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
			if (ctx->exr_recording)
				blog(LOG_INFO,
				     "[nifer-multipass] EXR depth sequence started -> %s",
				     ctx->rec_folder);
			else
				blog(LOG_INFO,
				     "[nifer-multipass] recording pass '%s' -> %s",
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
	return "Reshade Pass Capture (Nifer)";
}

static void nifer_update(void *data, obs_data_t *settings)
{
	struct nifer_source *ctx = data;

	int pass = (int)obs_data_get_int(settings, "pass");
	if (pass < 0 || pass >= NIFER_PASS_COUNT)
		pass = NIFER_PASS_COLOR;

	if (pass != ctx->pass) {
		ctx->pass = pass;
		free_texture(ctx);
	}

	ctx->record_enabled = obs_data_get_bool(settings, "record_enabled");
	ctx->exr_enabled = obs_data_get_bool(settings, "exr_enabled");
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
	ctx->hotkey_id = obs_hotkey_register_source(source,
		"nifer_multipass.record_toggle",
		"Record this Multi Pass source (toggle)",
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
	free_texture(ctx);
	close_shmem(ctx);
	bfree(ctx->rec_folder);
	bfree(ctx->rec_prefix);
	bfree(ctx->rec_encoder);
	bfree(ctx);
}

static void nifer_tick(void *data, float seconds)
{
	struct nifer_source *ctx = data;

	if (!ctx->info) {
		ctx->retry_timer += seconds;
		if (ctx->retry_timer >= 0.5f) {
			ctx->retry_timer = 0.0f;
			try_open_shmem(ctx);
		}
		return;
	}

	if (!ctx->info->active) {
		free_texture(ctx);
		close_shmem(ctx);
	}
}

static void nifer_render(void *data, gs_effect_t *unused_effect)
{
	struct nifer_source *ctx = data;
	UNUSED_PARAMETER(unused_effect);

	if (!ctx->info || !ctx->info->active)
		return;

	volatile nifer_pass_info *pi = &ctx->info->passes[ctx->pass];
	if (!pi->valid || pi->handle == 0)
		return;

	const uint32_t handle = pi->handle;
	const uint32_t generation = pi->generation;
	if (ctx->opened_handle != handle ||
	    ctx->opened_generation != generation) {
		if (ctx->texture) {
			gs_texture_destroy(ctx->texture);
			ctx->texture = NULL;
		}
		if (pi->handle_type == NIFER_HANDLE_NT)
			ctx->texture = gs_texture_open_nt_shared(handle);
		else
			ctx->texture = gs_texture_open_shared(handle);
		ctx->opened_handle = handle;
		ctx->opened_generation = generation;
		if (!ctx->texture)
			blog(LOG_WARNING,
			     "[nifer-multipass] failed to open shared texture %u (pass %d, type %u)",
			     handle, ctx->pass, pi->handle_type);
	}

	if (!ctx->texture)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

	const bool linear_srgb = gs_get_linear_srgb();
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear_srgb);

	if (linear_srgb)
		gs_effect_set_texture_srgb(image, ctx->texture);
	else
		gs_effect_set_texture(image, ctx->texture);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(ctx->texture, 0, 0, 0);

	gs_enable_framebuffer_srgb(previous);
}

// PROPERTIES UI
// ==========================================================================

static bool pass_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	obs_property_t *exr = obs_properties_get(props, "exr_enabled");
	if (exr)
		obs_property_set_visible(exr,
			obs_data_get_int(settings, "pass") == NIFER_PASS_DEPTH);

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

static void nifer_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "pass", NIFER_PASS_COLOR);
	obs_data_set_default_bool(settings, "record_enabled", false);
	obs_data_set_default_bool(settings, "exr_enabled", false);
	obs_data_set_default_string(settings, "rec_encoder", NIFER_ENC_OBS);
	obs_data_set_default_int(settings, "rec_fps", 0);
}

static obs_properties_t *nifer_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "title_text",
		"Nifer Multipass " NIFER_PLUGIN_VERSION " by Nifer\n"
		"Records ReShade passes (Color, Depth, Normals) as "
		"separate full resolution files.\n"
		"Requires the Nifer Multipass ReShade addon in a DirectX "
		"10/11/12 game with NiferMultiPass enabled. "
		"Start the game before recording.", OBS_TEXT_INFO);

	obs_property_t *list = obs_properties_add_list(props, "pass", "Pass",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "Color (without shaders)", NIFER_PASS_COLOR);
	obs_property_list_add_int(list, "Depth", NIFER_PASS_DEPTH);
	obs_property_list_add_int(list, "Normals", NIFER_PASS_NORMALS);
	obs_property_list_add_int(list, "Color (with shaders)", NIFER_PASS_RAW);
	obs_property_set_modified_callback(list, pass_changed);

	obs_properties_add_bool(props, "record_enabled", "Record this pass");

	obs_properties_add_bool(props, "exr_enabled",
		"Export as EXR image sequence instead of video "
		"(32-bit float depth, one .exr per game frame - large files, "
		"fast SSD recommended)");

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

	obs_property_list_add_string(enc,
		"Use OBS recording settings", NIFER_ENC_OBS);

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
	.id = "nifer_multipass_source",
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
		"nifer_multipass.record_all",
		"Record all Multi Pass sources (toggle)",
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

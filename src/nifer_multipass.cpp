/* ==========================================================================
 *  Nifer Multipass ReShade Addon v2.0 by Nifer
 *  Twitter - @NiferEdits
 *  Shares the Color, Depth, Normals and Raw passes with OBS as GPU textures
 *  Needs NiferMultiPass.fx enabled at the top of the effect list
 *  Based on the obs_capture addon example by Hugh Bailey and Patrick Mours
 * ========================================================================== */

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <d3d12.h>
#include <reshade.hpp>
#include "../shared/nifer_multipass_shared.h"

using namespace reshade::api;

// STATE
// ==========================================================================

struct pass_capture
{
	resource texture = {};
	HANDLE handle = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	format fmt = format::unknown;
	bool d3d12 = false;
	ID3D12Resource* tex12 = nullptr;
	uint32_t dup_for_pid = 0;
};

static HANDLE s_shmem = nullptr;
static nifer_shared_info* s_info = nullptr;
static pass_capture s_passes[NIFER_PASS_COUNT];

static effect_texture_variable s_color_var = {};
static effect_texture_variable s_shaded_var = {};
static effect_texture_variable s_depth_var = {};
static effect_texture_variable s_normals_var = {};
static effect_texture_variable s_depth_raw_var = {};
static effect_uniform_variable s_fps_limit_var = {};
static effect_runtime* s_active_runtime = nullptr;
static bool s_vars_cached = false;

static bool s_capture_this_frame = true;
static LONGLONG s_next_capture_time = 0;

static const char* const s_effect_file = "NiferMultiPass.fx";

// CAPTURE RATE LIMIT
// ==========================================================================

static bool should_capture(effect_runtime* runtime)
{
	int32_t limit = 0;
	if (s_fps_limit_var != 0)
		runtime->get_uniform_value_int(s_fps_limit_var, &limit, 1);
	if (limit <= 0)
		return true;

	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	const LONGLONG period = freq.QuadPart / limit;

	if (s_next_capture_time == 0)
		s_next_capture_time = now.QuadPart;
	if (now.QuadPart < s_next_capture_time)
		return false;

	s_next_capture_time += period;
	if (now.QuadPart > s_next_capture_time + period)
		s_next_capture_time = now.QuadPart;

	return true;
}

// EXR DEPTH SEQUENCE EXPORT
// ==========================================================================

#define EXR_RING  3
#define EXR_QUEUE 8

struct exr_job
{
	float* data;
	uint32_t width;
	uint32_t height;
	char path[640];
};

struct exr_state
{
	resource staging[EXR_RING];
	bool ready[EXR_RING];
	int cur;
	uint32_t width;
	uint32_t height;
	uint64_t frame_index;
	bool session_active;
	char session_dir[576];
};

static exr_state s_exr;
static exr_job s_exr_queue[EXR_QUEUE];
static int s_exr_q_head = 0;
static int s_exr_q_count = 0;
static bool s_exr_thread_exit = false;
static HANDLE s_exr_thread = nullptr;
static CRITICAL_SECTION s_exr_lock;
static CONDITION_VARIABLE s_exr_cv;

static void exr_write_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }

static bool exr_write_file(const char* path, const float* data, uint32_t w, uint32_t h)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || f == nullptr)
		return false;

	const uint8_t magic[8] = { 0x76, 0x2F, 0x31, 0x01, 2, 0, 0, 0 };
	fwrite(magic, 1, 8, f);

	fwrite("channels", 1, 9, f);
	fwrite("chlist", 1, 7, f);
	exr_write_u32(f, 19);
	fwrite("Z", 1, 2, f);
	exr_write_u32(f, 2);
	const uint8_t plinear[4] = { 0, 0, 0, 0 };
	fwrite(plinear, 1, 4, f);
	exr_write_u32(f, 1);
	exr_write_u32(f, 1);
	const uint8_t zero = 0;
	fwrite(&zero, 1, 1, f);

	fwrite("compression", 1, 12, f);
	fwrite("compression", 1, 12, f);
	exr_write_u32(f, 1);
	fwrite(&zero, 1, 1, f);

	const int32_t box[4] = { 0, 0, (int32_t)w - 1, (int32_t)h - 1 };
	fwrite("dataWindow", 1, 11, f);
	fwrite("box2i", 1, 6, f);
	exr_write_u32(f, 16);
	fwrite(box, 4, 4, f);
	fwrite("displayWindow", 1, 14, f);
	fwrite("box2i", 1, 6, f);
	exr_write_u32(f, 16);
	fwrite(box, 4, 4, f);

	fwrite("lineOrder", 1, 10, f);
	fwrite("lineOrder", 1, 10, f);
	exr_write_u32(f, 1);
	fwrite(&zero, 1, 1, f);

	const float one = 1.0f;
	fwrite("pixelAspectRatio", 1, 17, f);
	fwrite("float", 1, 6, f);
	exr_write_u32(f, 4);
	fwrite(&one, 4, 1, f);

	const float center[2] = { 0.0f, 0.0f };
	fwrite("screenWindowCenter", 1, 19, f);
	fwrite("v2f", 1, 4, f);
	exr_write_u32(f, 8);
	fwrite(center, 4, 2, f);

	fwrite("screenWindowWidth", 1, 18, f);
	fwrite("float", 1, 6, f);
	exr_write_u32(f, 4);
	fwrite(&one, 4, 1, f);

	fwrite(&zero, 1, 1, f);

	const uint64_t data_start = (uint64_t)_ftelli64(f) + (uint64_t)h * 8;
	for (uint32_t y = 0; y < h; ++y)
	{
		const uint64_t off = data_start + (uint64_t)y * (8 + (uint64_t)w * 4);
		fwrite(&off, 8, 1, f);
	}

	for (uint32_t y = 0; y < h; ++y)
	{
		const int32_t yy = (int32_t)y;
		const uint32_t size = w * 4;
		fwrite(&yy, 4, 1, f);
		fwrite(&size, 4, 1, f);
		fwrite(data + (size_t)y * w, 4, w, f);
	}

	fclose(f);
	return true;
}

static DWORD WINAPI exr_thread_proc(LPVOID)
{
	for (;;)
	{
		EnterCriticalSection(&s_exr_lock);
		while (s_exr_q_count == 0 && !s_exr_thread_exit)
			SleepConditionVariableCS(&s_exr_cv, &s_exr_lock, INFINITE);
		if (s_exr_q_count == 0 && s_exr_thread_exit)
		{
			LeaveCriticalSection(&s_exr_lock);
			break;
		}
		exr_job job = s_exr_queue[s_exr_q_head];
		s_exr_q_head = (s_exr_q_head + 1) % EXR_QUEUE;
		s_exr_q_count--;
		LeaveCriticalSection(&s_exr_lock);

		if (!exr_write_file(job.path, job.data, job.width, job.height))
			reshade::log::message(reshade::log::level::error,
				"nifer_multipass: failed to write EXR file");
		free(job.data);
	}
	return 0;
}

static void exr_enqueue(const uint8_t* mapped, uint32_t row_pitch, uint32_t w, uint32_t h, const char* path)
{
	float* copy = static_cast<float*>(malloc((size_t)w * h * 4));
	if (copy == nullptr)
		return;
	for (uint32_t y = 0; y < h; ++y)
		memcpy(copy + (size_t)y * w, mapped + (size_t)y * row_pitch, (size_t)w * 4);

	EnterCriticalSection(&s_exr_lock);
	if (s_exr_q_count == EXR_QUEUE)
	{
		LeaveCriticalSection(&s_exr_lock);
		free(copy);
		reshade::log::message(reshade::log::level::warning,
			"nifer_multipass: EXR writer cannot keep up, dropping a frame (disk too slow?)");
		return;
	}
	exr_job& job = s_exr_queue[(s_exr_q_head + s_exr_q_count) % EXR_QUEUE];
	job.data = copy;
	job.width = w;
	job.height = h;
	strncpy_s(job.path, path, _TRUNCATE);
	s_exr_q_count++;
	LeaveCriticalSection(&s_exr_lock);
	WakeConditionVariable(&s_exr_cv);
}

static void exr_release_staging(device* dev)
{
	for (int i = 0; i < EXR_RING; ++i)
	{
		if (s_exr.staging[i] != 0)
		{
			dev->destroy_resource(s_exr.staging[i]);
			s_exr.staging[i] = {};
		}
		s_exr.ready[i] = false;
	}
	s_exr.cur = 0;
	s_exr.width = 0;
	s_exr.height = 0;
}

static bool exr_ensure_staging(device* dev, const resource_desc& src_desc)
{
	if (s_exr.staging[0] != 0 &&
		s_exr.width == src_desc.texture.width &&
		s_exr.height == src_desc.texture.height)
		return true;

	exr_release_staging(dev);

	for (int i = 0; i < EXR_RING; ++i)
	{
		if (!dev->create_resource(
			resource_desc(
				src_desc.texture.width, src_desc.texture.height, 1, 1,
				format::r32_float, 1,
				memory_heap::gpu_to_cpu,
				resource_usage::copy_dest),
			nullptr,
			resource_usage::copy_dest,
			&s_exr.staging[i]))
		{
			reshade::log::message(reshade::log::level::error,
				"nifer_multipass: failed to create EXR readback texture");
			exr_release_staging(dev);
			return false;
		}
	}

	s_exr.width = src_desc.texture.width;
	s_exr.height = src_desc.texture.height;
	return true;
}

static void exr_begin_session()
{
	char folder[512];
	strncpy_s(folder, s_info->exr.folder, _TRUNCATE);
	if (folder[0] == '\0')
		return;

	char timebuf[64];
	const time_t now = time(nullptr);
	struct tm tm_now;
	localtime_s(&tm_now, &now);
	strftime(timebuf, sizeof(timebuf), "%Y-%m-%d_%H-%M-%S", &tm_now);

	snprintf(s_exr.session_dir, sizeof(s_exr.session_dir),
		"%s/depth_exr_%s", folder, timebuf);
	CreateDirectoryA(s_exr.session_dir, nullptr);

	s_exr.frame_index = 0;
	s_exr.session_active = true;

	reshade::log::message(reshade::log::level::info,
		"nifer_multipass: EXR depth sequence export started");
}

static void exr_end_session(device* dev)
{
	exr_release_staging(dev);
	s_exr.session_active = false;

	reshade::log::message(reshade::log::level::info,
		"nifer_multipass: EXR depth sequence export stopped");
}

static void handle_exr_export(effect_runtime* runtime, command_list* cmd_list)
{
	device* const dev = runtime->get_device();

	if (s_info->exr.active == 0)
	{
		if (s_exr.session_active)
			exr_end_session(dev);
		return;
	}

	if (s_depth_raw_var == 0)
		return;

	if (!s_exr.session_active)
	{
		exr_begin_session();
		if (!s_exr.session_active)
			return;
	}

	resource_view srv = {}, srv_srgb = {};
	runtime->get_texture_binding(s_depth_raw_var, &srv, &srv_srgb);
	if (srv == 0)
		return;
	const resource res = dev->get_resource_from_view(srv);
	if (res == 0)
		return;

	const resource_desc desc = dev->get_resource_desc(res);
	if (!exr_ensure_staging(dev, desc))
		return;

	const int next = (s_exr.cur + 1) % EXR_RING;
	if (s_exr.ready[next])
	{
		subresource_data mapped;
		if (dev->map_texture_region(s_exr.staging[next], 0, nullptr,
			map_access::read_only, &mapped))
		{
			char path[640];
			snprintf(path, sizeof(path), "%s/depth_%06llu.exr",
				s_exr.session_dir,
				(unsigned long long)s_exr.frame_index++);
			exr_enqueue(static_cast<const uint8_t*>(mapped.data),
				mapped.row_pitch, s_exr.width, s_exr.height, path);
			dev->unmap_texture_region(s_exr.staging[next], 0);
		}
		s_exr.ready[next] = false;
	}

	cmd_list->barrier(res, resource_usage::shader_resource, resource_usage::copy_source);
	cmd_list->copy_resource(res, s_exr.staging[s_exr.cur]);
	cmd_list->barrier(res, resource_usage::copy_source, resource_usage::shader_resource);
	s_exr.ready[s_exr.cur] = true;
	s_exr.cur = next;
}

// SHARED TEXTURES
// ==========================================================================

static bool is_supported_api(device* dev)
{
	const device_api api = dev->get_api();
	return api == device_api::d3d10 || api == device_api::d3d11 ||
		api == device_api::d3d12;
}

static void sync_shared_handle(int pass)
{
	pass_capture& p = s_passes[pass];
	nifer_pass_info& ip = s_info->passes[pass];

	if (!p.d3d12 || p.handle == nullptr)
		return;

	const uint32_t obs_pid = s_info->obs_pid;
	if (obs_pid == 0)
	{
		p.dup_for_pid = 0;
		return;
	}
	if (p.dup_for_pid == obs_pid && ip.handle != 0)
		return;

	HANDLE obs_process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, obs_pid);
	if (obs_process == nullptr)
		return;

	HANDLE dup = nullptr;
	const BOOL ok = DuplicateHandle(GetCurrentProcess(), p.handle,
		obs_process, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
	CloseHandle(obs_process);
	if (!ok)
	{
		reshade::log::message(reshade::log::level::error,
			"nifer_multipass: failed to duplicate NT handle into OBS process");
		return;
	}

	ip.handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dup));
	ip.generation++;
	p.dup_for_pid = obs_pid;
}

static void release_pass_texture(device* dev, int pass)
{
	pass_capture& p = s_passes[pass];

	if (s_info != nullptr)
	{
		s_info->passes[pass].valid = 0;
		s_info->passes[pass].handle = 0;
	}

	if (p.d3d12)
	{
		if (p.handle != nullptr)
			CloseHandle(p.handle);
		if (p.tex12 != nullptr)
			p.tex12->Release();
	}
	else if (p.texture != 0)
	{
		dev->destroy_resource(p.texture);
	}

	p = pass_capture();
}

static bool create_d3d12_shared_texture(device* dev, pass_capture& p, uint32_t width, uint32_t height, format fmt)
{
	ID3D12Device* const dev12 =
		reinterpret_cast<ID3D12Device*>(dev->get_native());

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = static_cast<DXGI_FORMAT>(fmt);
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
		D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

	if (FAILED(dev12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED,
		&desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(&p.tex12))))
	{
		reshade::log::message(reshade::log::level::error,
			"nifer_multipass: failed to create D3D12 shared texture");
		return false;
	}

	if (FAILED(dev12->CreateSharedHandle(p.tex12, nullptr, GENERIC_ALL,
		nullptr, &p.handle)))
	{
		reshade::log::message(reshade::log::level::error,
			"nifer_multipass: failed to create D3D12 shared handle");
		p.tex12->Release();
		p.tex12 = nullptr;
		return false;
	}

	p.texture = { reinterpret_cast<uint64_t>(p.tex12) };
	return true;
}

static bool ensure_pass_texture(device* dev, int pass, const resource_desc& src_desc)
{
	pass_capture& p = s_passes[pass];

	const format fmt = format_to_default_typed(src_desc.texture.format, 0);

	if (p.texture != 0 &&
		p.width == src_desc.texture.width &&
		p.height == src_desc.texture.height &&
		format_to_default_typed(p.fmt, 0) == fmt)
		return true;

	if (p.texture != 0)
		release_pass_texture(dev, pass);

	const bool d3d12 = dev->get_api() == device_api::d3d12;
	const resource_usage copy_state = src_desc.texture.samples > 1 ?
		resource_usage::resolve_dest : resource_usage::copy_dest;

	if (d3d12)
	{
		if (!create_d3d12_shared_texture(dev, p,
			src_desc.texture.width, src_desc.texture.height, fmt))
			return false;
		p.fmt = fmt;
	}
	else
	{
		if (!dev->create_resource(
			resource_desc(
				src_desc.texture.width, src_desc.texture.height, 1, 1, fmt, 1,
#if RESHADE_API_VERSION >= 20
				memory_heap::default_,
#else
				memory_heap::gpu_only,
#endif
				resource_usage::shader_resource | copy_state,
				resource_flags::shared),
			nullptr,
			copy_state,
			&p.texture,
			&p.handle))
		{
			reshade::log::message(reshade::log::level::error,
				"nifer_multipass: failed to create shared texture");
			return false;
		}
		p.fmt = dev->get_resource_desc(p.texture).texture.format;
	}

	p.width = src_desc.texture.width;
	p.height = src_desc.texture.height;
	p.d3d12 = d3d12;
	p.dup_for_pid = 0;

	nifer_pass_info& ip = s_info->passes[pass];
	ip.valid = 0;
	ip.width = p.width;
	ip.height = p.height;
	ip.format = static_cast<uint32_t>(p.fmt);
	ip.frame = 0;
	ip.handle_type = d3d12 ? NIFER_HANDLE_NT : NIFER_HANDLE_LEGACY;

	if (d3d12)
	{
		ip.handle = 0;
	}
	else
	{
		ip.handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p.handle));
		ip.generation++;
	}

	return true;
}

static void copy_pass(command_list* cmd_list, int pass, resource src, resource_usage src_state, bool multisampled)
{
	pass_capture& p = s_passes[pass];

	if (multisampled)
	{
		cmd_list->barrier(src, src_state, resource_usage::resolve_source);
		cmd_list->resolve_texture_region(src, 0, nullptr, p.texture, 0, 0, 0, 0, format_to_default_typed(p.fmt, 0));
		cmd_list->barrier(src, resource_usage::resolve_source, src_state);
	}
	else
	{
		cmd_list->barrier(src, src_state, resource_usage::copy_source);
		cmd_list->copy_resource(src, p.texture);
		cmd_list->barrier(src, resource_usage::copy_source, src_state);
	}

	sync_shared_handle(pass);

	nifer_pass_info& ip = s_info->passes[pass];
	ip.frame++;
	ip.valid = 1;
}

// COLOR PASS
// ==========================================================================

static void on_begin_effects(effect_runtime* runtime, command_list* cmd_list, resource_view, resource_view)
{
	if (s_info == nullptr || cmd_list == nullptr)
		return;

	device* const dev = runtime->get_device();
	if (!is_supported_api(dev))
		return;

	if (s_active_runtime != nullptr && runtime != s_active_runtime)
		return;

	s_capture_this_frame = should_capture(runtime);
	if (!s_capture_this_frame)
		return;

	if (s_vars_cached && s_color_var != 0)
		return;

	const resource back_buffer = runtime->get_current_back_buffer();
	const resource_desc desc = dev->get_resource_desc(back_buffer);

	if (!ensure_pass_texture(dev, NIFER_PASS_COLOR, desc))
		return;

	copy_pass(cmd_list, NIFER_PASS_COLOR, back_buffer,
		resource_usage::render_target, desc.texture.samples > 1);
}

// DEPTH + NORMALS PASSES
// ==========================================================================

static void cache_texture_variables(effect_runtime* runtime)
{
	s_color_var = runtime->find_texture_variable(s_effect_file, "NiferColorTex");
	s_shaded_var = runtime->find_texture_variable(s_effect_file, "NiferShadedTex");
	s_depth_var = runtime->find_texture_variable(s_effect_file, "NiferDepthTex");
	s_normals_var = runtime->find_texture_variable(s_effect_file, "NiferNormalsTex");
	s_depth_raw_var = runtime->find_texture_variable(s_effect_file, "NiferDepthRawTex");

	if (s_color_var == 0)
		s_color_var = runtime->find_texture_variable(nullptr, "NiferColorTex");
	if (s_shaded_var == 0)
		s_shaded_var = runtime->find_texture_variable(nullptr, "NiferShadedTex");
	if (s_depth_var == 0)
		s_depth_var = runtime->find_texture_variable(nullptr, "NiferDepthTex");
	if (s_normals_var == 0)
		s_normals_var = runtime->find_texture_variable(nullptr, "NiferNormalsTex");
	if (s_depth_raw_var == 0)
		s_depth_raw_var = runtime->find_texture_variable(nullptr, "NiferDepthRawTex");

	s_fps_limit_var = runtime->find_uniform_variable(s_effect_file, "CaptureFpsLimit");
	if (s_fps_limit_var == 0)
		s_fps_limit_var = runtime->find_uniform_variable(nullptr, "CaptureFpsLimit");

	s_vars_cached = true;

	if (s_color_var != 0 || s_depth_var != 0 || s_normals_var != 0)
		s_active_runtime = runtime;

	if (s_color_var == 0 || s_depth_var == 0 || s_normals_var == 0 || s_depth_raw_var == 0)
		reshade::log::message(reshade::log::level::warning,
			"nifer_multipass: one or more NiferMultiPass.fx textures not found "
			"(is the latest shader version installed and loaded?)");
}

static void capture_fx_texture(effect_runtime* runtime, command_list* cmd_list, effect_texture_variable variable, int pass)
{
	if (variable == 0)
		return;

	device* const dev = runtime->get_device();

	resource_view srv = {}, srv_srgb = {};
	runtime->get_texture_binding(variable, &srv, &srv_srgb);
	if (srv == 0)
		return;

	const resource res = dev->get_resource_from_view(srv);
	if (res == 0)
		return;

	const resource_desc desc = dev->get_resource_desc(res);

	if (!ensure_pass_texture(dev, pass, desc))
		return;

	copy_pass(cmd_list, pass, res,
		resource_usage::shader_resource, false);
}

static void capture_final_output(effect_runtime* runtime, command_list* cmd_list, resource_view rtv)
{
	device* const dev = runtime->get_device();

	resource res = (rtv != 0) ? dev->get_resource_from_view(rtv) : resource{};
	if (res == 0)
		res = runtime->get_current_back_buffer();
	if (res == 0)
		return;

	const resource_desc desc = dev->get_resource_desc(res);

	if (!ensure_pass_texture(dev, NIFER_PASS_RAW, desc))
		return;

	copy_pass(cmd_list, NIFER_PASS_RAW, res,
		resource_usage::render_target, desc.texture.samples > 1);
}

static void on_finish_effects(effect_runtime* runtime, command_list* cmd_list, resource_view rtv, resource_view)
{
	if (s_info == nullptr || cmd_list == nullptr)
		return;
	if (!is_supported_api(runtime->get_device()))
		return;

	if (!s_vars_cached)
		cache_texture_variables(runtime);

	if (s_active_runtime != nullptr && runtime != s_active_runtime)
		return;

	if (!s_capture_this_frame)
		return;

	capture_fx_texture(runtime, cmd_list, s_color_var, NIFER_PASS_COLOR);
	capture_fx_texture(runtime, cmd_list, s_depth_var, NIFER_PASS_DEPTH);
	capture_fx_texture(runtime, cmd_list, s_normals_var, NIFER_PASS_NORMALS);
	if (s_shaded_var != 0)
		capture_fx_texture(runtime, cmd_list, s_shaded_var, NIFER_PASS_RAW);
	else
		capture_final_output(runtime, cmd_list, rtv);

	handle_exr_export(runtime, cmd_list);
}

static void on_reloaded_effects(effect_runtime*)
{
	s_color_var = {};
	s_shaded_var = {};
	s_depth_var = {};
	s_normals_var = {};
	s_depth_raw_var = {};
	s_fps_limit_var = {};
	s_active_runtime = nullptr;
	s_vars_cached = false;
	s_next_capture_time = 0;
}

// CLEANUP
// ==========================================================================

static void on_destroy_runtime(effect_runtime* runtime)
{
	if (runtime == s_active_runtime)
	{
		s_active_runtime = nullptr;
		s_vars_cached = false;
	}

	runtime->get_command_queue()->wait_idle();

	if (s_exr.session_active)
		exr_end_session(runtime->get_device());
	else
		exr_release_staging(runtime->get_device());

	for (int i = 0; i < NIFER_PASS_COUNT; ++i)
		release_pass_texture(runtime->get_device(), i);
}

// ADDON ENTRY POINTS
// ==========================================================================

extern "C" __declspec(dllexport) const char* NAME = "Nifer Multipass";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
"Shares clean color, depth and normals passes with OBS as GPU textures. "
"Use together with NiferMultiPass.fx and the 'Reshade Pass Capture' OBS source plugin.";

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
	if (!reshade::register_addon(addon_module, reshade_module))
		return false;

	s_shmem = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		0, sizeof(nifer_shared_info), NIFER_SHARED_NAME);
	if (s_shmem == nullptr)
	{
		reshade::unregister_addon(addon_module, reshade_module);
		return false;
	}

	s_info = static_cast<nifer_shared_info*>(
		MapViewOfFile(s_shmem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(nifer_shared_info)));
	if (s_info == nullptr)
	{
		CloseHandle(s_shmem);
		s_shmem = nullptr;
		reshade::unregister_addon(addon_module, reshade_module);
		return false;
	}

	std::memset(s_info, 0, sizeof(nifer_shared_info));
	s_info->magic = NIFER_MAGIC;
	s_info->version = NIFER_VERSION;
	s_info->pid = GetCurrentProcessId();
	s_info->active = 1;

	InitializeCriticalSection(&s_exr_lock);
	InitializeConditionVariable(&s_exr_cv);
	s_exr_thread_exit = false;
	s_exr_thread = CreateThread(nullptr, 0, exr_thread_proc, nullptr, 0, nullptr);

	reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
	reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
	reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_runtime);

	return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
	if (s_exr_thread != nullptr)
	{
		EnterCriticalSection(&s_exr_lock);
		s_exr_thread_exit = true;
		LeaveCriticalSection(&s_exr_lock);
		WakeConditionVariable(&s_exr_cv);
		WaitForSingleObject(s_exr_thread, 5000);
		CloseHandle(s_exr_thread);
		s_exr_thread = nullptr;
		DeleteCriticalSection(&s_exr_lock);
	}

	if (s_info != nullptr)
	{
		s_info->active = 0;
		UnmapViewOfFile(s_info);
		s_info = nullptr;
	}
	if (s_shmem != nullptr)
	{
		CloseHandle(s_shmem);
		s_shmem = nullptr;
	}

	reshade::unregister_addon(addon_module, reshade_module);
}
/* ==========================================================================
 *  Nifer Multipass Shared Memory Layout v2.0 by Nifer
 *  Twitter - @NiferEdits
 *  Used by BOTH the ReShade addon and the OBS source plugin
 *  Keep this file identical on both sides and rebuild both when it changes
 * ========================================================================== */

#pragma once

#include <stdint.h>

#define NIFER_SHARED_NAME "NiferMultipassSharedInfo_v4"

#define NIFER_MAGIC   0x5246494Eu
#define NIFER_VERSION 4u

#define NIFER_PASS_COLOR   0
#define NIFER_PASS_DEPTH   1
#define NIFER_PASS_NORMALS 2
#define NIFER_PASS_RAW     3
#define NIFER_PASS_COUNT   4

#define NIFER_HANDLE_LEGACY 0
#define NIFER_HANDLE_NT     1

#pragma pack(push, 4)

typedef struct nifer_pass_info
{
	uint32_t valid;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t handle;
	uint32_t handle_type;
	uint32_t generation;
	uint32_t reserved;
	uint64_t frame;
} nifer_pass_info;

typedef struct nifer_exr_control
{
	uint32_t active;
	uint32_t reserved;
	char folder[512];
} nifer_exr_control;

typedef struct nifer_shared_info
{
	uint32_t magic;
	uint32_t version;
	uint32_t pid;
	uint32_t active;
	uint32_t obs_pid;
	uint32_t reserved[3];
	nifer_pass_info passes[NIFER_PASS_COUNT];
	nifer_exr_control exr;
} nifer_shared_info;

#pragma pack(pop)

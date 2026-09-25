// ==========================================================================
//  NiferMultiPassGrid v2.0 by Nifer
//  Twitter - @NiferEdits
//  Draws the Color, Depth and Normals passes on screen so they can be
//  captured without an addon build of ReShade
//  Layouts: four passes (2x2), two passes (side by side), one pass fullscreen
//  Enable NiferGridCapture at the TOP and NiferGridColor at the BOTTOM
//  Credits: Crosire (ReShade), DisplayDepth.fx (normals reconstruction)
// ==========================================================================

#include "ReShade.fxh"

#define NIFER_PASS_COLOR   0
#define NIFER_PASS_DEPTH   1
#define NIFER_PASS_NORMALS 2
#define NIFER_PASS_RAW     3

// OPTIONS
// ==========================================================================

uniform int GridLayout <
	ui_type = "combo";
	ui_label = "Layout";
	ui_tooltip = "How many passes to show on screen at once.\n"
	             "Fewer passes means more pixels for each one.\n"
	             "Four: needs double width AND height for full resolution.\n"
	             "Two: needs double width only.\n"
	             "One: already full resolution, no doubling needed.\n"
	             "Frame cycle: full resolution, one pass per frame in turn.\n"
	             "Your pass frame rate becomes game fps / cycle length.";
	ui_items = "Four passes (2x2)\0Two passes (side by side)\0One pass (fullscreen)\0Frame cycle (full resolution)\0";
	ui_category = "Layout";
> = 0;

uniform int LeftPass <
	ui_type = "combo";
	ui_label = "Left Pass (two pass layout)";
	ui_items = "Color (without shaders)\0Depth\0Normals\0Color (with shaders)\0";
	ui_category = "Layout";
> = 0;

uniform int RightPass <
	ui_type = "combo";
	ui_label = "Right Pass (two pass layout)";
	ui_items = "Color (without shaders)\0Depth\0Normals\0Color (with shaders)\0";
	ui_category = "Layout";
> = 1;

uniform int SinglePass <
	ui_type = "combo";
	ui_label = "Pass (one pass layout)";
	ui_tooltip = "Which pass fills the screen in the one pass layout.\n"
	             "Bind a key to this in the ReShade settings to switch\n"
	             "passes between takes when re-recording a replay.";
	ui_items = "Color (without shaders)\0Depth\0Normals\0Color (with shaders)\0";
	ui_category = "Layout";
> = 0;

uniform int CycleCount <
	ui_type = "slider";
	ui_label = "Cycle Length";
	ui_tooltip = "How many passes to cycle through, one per frame.\n"
	             "Each pass records at game fps / this number, so pick the\n"
	             "smallest number of passes you actually need.\n"
	             "Example: 240 fps game with 4 passes gives 60 fps per pass,\n"
	             "with 2 passes it gives 120 fps per pass.";
	ui_min = 1; ui_max = 4; ui_step = 1;
	ui_category = "Frame Cycle";
> = 4;

uniform int FramesPerPass <
	ui_type = "slider";
	ui_label = "Frames Per Pass";
	ui_tooltip = "How many game frames each pass stays on screen.\n"
	             "Raise this if OBS cannot sample every game frame, for\n"
	             "example when your game runs much faster than the OBS\n"
	             "canvas frame rate. Higher values are more reliable but\n"
	             "lower the pass frame rate further.";
	ui_min = 1; ui_max = 8; ui_step = 1;
	ui_category = "Frame Cycle";
> = 1;

uniform bool PhaseDither <
	ui_label = "Anti Alias Phase";
	ui_tooltip = "Slowly shifts the cycle so that OBS cannot lock onto the\n"
	             "same passes every time and starve the others.\n"
	             "Leave this on unless you are debugging.";
	ui_category = "Frame Cycle";
> = true;

uniform int CycleSlot0 <
	ui_type = "combo";
	ui_label = "Cycle Slot 1";
	ui_items = "Color (without shaders)\0Depth\0Normals\0Color (with shaders)\0";
	ui_category = "Frame Cycle";
> = 0;

uniform int CycleSlot1 <
	ui_type = "combo";
	ui_label = "Cycle Slot 2";
	ui_items = "Color (without shaders)\0Depth\0Normals\0Color (with shaders)\0";
	ui_category = "Frame Cycle";
> = 1;

uniform int CycleSlot2 <
	ui_type = "combo";
	ui_label = "Cycle Slot 3";
	ui_items = "Color (without shaders)\0Depth\0Normals\0Color (with shaders)\0";
	ui_category = "Frame Cycle";
> = 2;

uniform int CycleSlot3 <
	ui_type = "combo";
	ui_label = "Cycle Slot 4";
	ui_items = "Color (without shaders)\0Depth\0Normals\0Color (with shaders)\0";
	ui_category = "Frame Cycle";
> = 3;

uniform int framecount < source = "framecount"; >;

uniform float DepthMultiplier <
	ui_type = "slider";
	ui_label = "Depth Brightness";
	ui_tooltip = "Multiplies the linearized depth. Raise this if the depth pass looks too dark.";
	ui_min = 0.1; ui_max = 20.0; ui_step = 0.1;
	ui_category = "Depth Options";
> = 1.0;

uniform float DepthGamma <
	ui_type = "slider";
	ui_label = "Depth Gamma";
	ui_tooltip = "Gamma curve for the depth pass. Below 1.0 brightens distant detail.";
	ui_min = 0.1; ui_max = 4.0; ui_step = 0.05;
	ui_category = "Depth Options";
> = 1.0;

uniform bool InvertDepth <
	ui_label = "Invert Depth";
	ui_tooltip = "White = near, black = far (instead of the reverse).";
	ui_category = "Depth Options";
> = false;

// CAPTURE TEXTURES
// ==========================================================================

texture NiferGridColorTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture NiferHoldColorTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture NiferHoldDepthTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture NiferHoldNormalsTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture NiferHoldRawTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };

sampler NiferGridColorSampler { Texture = NiferGridColorTex; };
sampler NiferHoldColorSampler { Texture = NiferHoldColorTex; };
sampler NiferHoldDepthSampler { Texture = NiferHoldDepthTex; };
sampler NiferHoldNormalsSampler { Texture = NiferHoldNormalsTex; };
sampler NiferHoldRawSampler { Texture = NiferHoldRawTex; };

// PASS GENERATION
// ==========================================================================

float GetDepthValue(float2 uv)
{
	float depth = ReShade::GetLinearizedDepth(uv);
	depth = saturate(depth * DepthMultiplier);
	depth = pow(abs(depth), DepthGamma);
	if (InvertDepth)
		depth = 1.0 - depth;
	return depth;
}

float3 GetNormalsValue(float2 uv)
{
	float3 offset = float3(BUFFER_PIXEL_SIZE, 0.0);
	float2 posCenter = uv;
	float2 posNorth  = posCenter - offset.zy;
	float2 posEast   = posCenter + offset.xz;

	float3 vertCenter = float3(posCenter - 0.5, 1.0) * ReShade::GetLinearizedDepth(posCenter);
	float3 vertNorth  = float3(posNorth  - 0.5, 1.0) * ReShade::GetLinearizedDepth(posNorth);
	float3 vertEast   = float3(posEast   - 0.5, 1.0) * ReShade::GetLinearizedDepth(posEast);

	return normalize(cross(vertCenter - vertNorth, vertCenter - vertEast)) * 0.5 + 0.5;
}

float3 SamplePass(int pass_id, float2 uv)
{
	if (pass_id == NIFER_PASS_COLOR)
		return tex2D(NiferGridColorSampler, uv).rgb;
	if (pass_id == NIFER_PASS_DEPTH)
		return GetDepthValue(uv).xxx;
	if (pass_id == NIFER_PASS_NORMALS)
		return GetNormalsValue(uv);

	return tex2D(ReShade::BackBuffer, uv).rgb;
}

// SHADERS
// ==========================================================================

int CycleLength()
{
	return clamp(CycleCount, 1, 4);
}

int CycleHold()
{
	return clamp(FramesPerPass, 1, 8);
}

int CycleStep()
{
	int len = CycleLength();
	int hold = CycleHold();
	int step = framecount / hold;

	if (PhaseDither)
		step += framecount / (hold * len * 3);

	return step;
}

int CyclePhase()
{
	int len = CycleLength();
	int phase = CycleStep() % len;
	return (phase < 0) ? phase + len : phase;
}

int CyclePassId(int phase)
{
	if (phase == 0)
		return CycleSlot0;
	if (phase == 1)
		return CycleSlot1;
	if (phase == 2)
		return CycleSlot2;
	return CycleSlot3;
}

bool CycleHoldFrame()
{
	return GridLayout == 3 && CyclePhase() == 0 &&
	       (framecount % CycleHold()) == 0;
}

float4 CapturePS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(tex2D(ReShade::BackBuffer, uv).rgb, 1.0);
}

float4 HoldColorPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!CycleHoldFrame())
		discard;
	return float4(tex2D(NiferGridColorSampler, uv).rgb, 1.0);
}

float4 HoldDepthPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!CycleHoldFrame())
		discard;
	return float4(GetDepthValue(uv).xxx, 1.0);
}

float4 HoldNormalsPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!CycleHoldFrame())
		discard;
	return float4(GetNormalsValue(uv), 1.0);
}

float4 HoldRawPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!CycleHoldFrame())
		discard;
	return float4(tex2D(ReShade::BackBuffer, uv).rgb, 1.0);
}

float3 SampleHeldPass(int pass_id, float2 uv)
{
	if (pass_id == NIFER_PASS_COLOR)
		return tex2D(NiferHoldColorSampler, uv).rgb;
	if (pass_id == NIFER_PASS_DEPTH)
		return tex2D(NiferHoldDepthSampler, uv).rgb;
	if (pass_id == NIFER_PASS_NORMALS)
		return tex2D(NiferHoldNormalsSampler, uv).rgb;

	return tex2D(NiferHoldRawSampler, uv).rgb;
}

float3 CycleMarkerColor(int pass_id)
{
	if (pass_id == NIFER_PASS_COLOR)
		return float3(1.0, 0.0, 0.0);
	if (pass_id == NIFER_PASS_DEPTH)
		return float3(0.0, 1.0, 0.0);
	if (pass_id == NIFER_PASS_NORMALS)
		return float3(0.0, 0.0, 1.0);

	return float3(1.0, 1.0, 0.0);
}

float4 GridPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (GridLayout == 3) {
		int pass_id = CyclePassId(CyclePhase());
		float2 px = uv * float2(BUFFER_WIDTH, BUFFER_HEIGHT);

		if (px.y < 16.0 && px.x < 32.0) {
			if (px.x < 16.0)
				return float4(1.0, 0.0, 1.0, 1.0);
			return float4(CycleMarkerColor(pass_id), 1.0);
		}

		return float4(SampleHeldPass(pass_id, uv), 1.0);
	}

	if (GridLayout == 2)
		return float4(SamplePass(SinglePass, uv), 1.0);

	if (GridLayout == 1) {
		float2 local = float2(frac(uv.x * 2.0), uv.y);
		int pass_id = (uv.x < 0.5) ? LeftPass : RightPass;
		return float4(SamplePass(pass_id, local), 1.0);
	}

	float2 local = frac(uv * 2.0);
	bool right = uv.x >= 0.5;
	bool bottom = uv.y >= 0.5;

	int pass_id = NIFER_PASS_COLOR;
	if (right && !bottom)
		pass_id = NIFER_PASS_DEPTH;
	else if (!right && bottom)
		pass_id = NIFER_PASS_NORMALS;
	else if (right && bottom)
		pass_id = NIFER_PASS_RAW;

	return float4(SamplePass(pass_id, local), 1.0);
}

// TECHNIQUES
// ==========================================================================

technique NiferGridCapture <
	ui_tooltip = "Captures the clean Color pass before your other shaders.\n"
	             "Place this at the TOP of the effect list and keep it ENABLED.\n"
	             "Draws nothing on screen, that is normal.";
>
{
	pass CapturePass
	{
		VertexShader = PostProcessVS;
		PixelShader = CapturePS;
		RenderTarget = NiferGridColorTex;
	}
}

technique NiferGridColor <
	ui_tooltip = "Draws the passes on screen for capture.\n"
	             "Place this at the BOTTOM of the effect list.\n"
	             "Four pass layout: Color no shaders top left, Depth top right,\n"
	             "Normals bottom left, Color with shaders bottom right.";
>
{
	pass HoldColorPass
	{
		VertexShader = PostProcessVS;
		PixelShader = HoldColorPS;
		RenderTarget = NiferHoldColorTex;
		ClearRenderTargets = false;
	}
	pass HoldDepthPass
	{
		VertexShader = PostProcessVS;
		PixelShader = HoldDepthPS;
		RenderTarget = NiferHoldDepthTex;
		ClearRenderTargets = false;
	}
	pass HoldNormalsPass
	{
		VertexShader = PostProcessVS;
		PixelShader = HoldNormalsPS;
		RenderTarget = NiferHoldNormalsTex;
		ClearRenderTargets = false;
	}
	pass HoldRawPass
	{
		VertexShader = PostProcessVS;
		PixelShader = HoldRawPS;
		RenderTarget = NiferHoldRawTex;
		ClearRenderTargets = false;
	}
	pass GridPass
	{
		VertexShader = PostProcessVS;
		PixelShader = GridPS;
	}
}
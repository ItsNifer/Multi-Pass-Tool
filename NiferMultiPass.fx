// ==========================================================================
//  NiferMultiPass v2.0 by Nifer
//  Twitter - @NiferEdits
//  Captures Color, Depth and Normals into offscreen textures for the Nifer
//  Multipass ReShade addon, which looks them up by name
//  Enable NiferMultiPass at the TOP of the effect list, and (only if
//  you want the Color with shaders pass) NiferMultiPassColor at the BOTTOM
//  Credits: Crosire (ReShade), DisplayDepth.fx (normals reconstruction)
// ==========================================================================

#include "ReShade.fxh"

// OPTIONS
// ==========================================================================

uniform int CaptureFpsLimit <
	ui_type = "slider";
	ui_label = "Capture FPS Limit";
	ui_tooltip = "Limits how often frames are sent to OBS and how often EXR\n"
	             "depth frames are written. 0 = every game frame (default).\n"
	             "Lower this to save system resources, especially for the\n"
	             "EXR depth sequence export.";
	ui_min = 0; ui_max = 240; ui_step = 1;
	ui_category = "Capture";
> = 0;

uniform float DepthMultiplier <
	ui_type = "slider";
	ui_label = "Depth Brightness";
	ui_tooltip = "Multiplies the linearized depth. Raise this if the depth pass looks too dark.\n"
	             "Only affects the video depth pass, never the 32-bit EXR export.";
	ui_min = 0.1; ui_max = 20.0; ui_step = 0.1;
	ui_category = "Depth Options";
> = 1.0;

uniform float DepthGamma <
	ui_type = "slider";
	ui_label = "Depth Gamma";
	ui_tooltip = "Gamma curve for the depth pass. Below 1.0 brightens distant detail.\n"
	             "Only affects the video depth pass, never the 32-bit EXR export.";
	ui_min = 0.1; ui_max = 4.0; ui_step = 0.05;
	ui_category = "Depth Options";
> = 1.0;

uniform bool InvertDepth <
	ui_label = "Invert Depth";
	ui_tooltip = "White = near, black = far (instead of the reverse).\n"
	             "Only affects the video depth pass, never the 32-bit EXR export.";
	ui_category = "Depth Options";
> = false;

uniform bool PreviewEnabled <
	ui_label = "Preview Pass On Screen";
	ui_tooltip = "Shows the selected pass fullscreen so you can verify it works.\n"
	             "TURN THIS OFF BEFORE RECORDING.";
	ui_category = "Preview";
> = false;

uniform int PreviewPass <
	ui_type = "combo";
	ui_label = "Preview Pass";
	ui_tooltip = "Which pass to show while Preview is on.\n"
	             "Color (with shaders) needs NiferMultiPassColor enabled at\n"
	             "the bottom of the effect list, otherwise it stays black.";
	ui_items = "Depth\0Normals\0Color (without shaders)\0Color (with shaders)\0";
	ui_category = "Preview";
> = 0;

// CAPTURE TEXTURES
// ==========================================================================

texture NiferColorTex    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture NiferShadedTex   { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture NiferDepthTex    { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture NiferNormalsTex  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture NiferDepthRawTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F; };

sampler NiferColorSampler    { Texture = NiferColorTex; };
sampler NiferShadedSampler   { Texture = NiferShadedTex; };
sampler NiferDepthSampler    { Texture = NiferDepthTex; };
sampler NiferNormalsSampler  { Texture = NiferNormalsTex; };
sampler NiferDepthRawSampler { Texture = NiferDepthRawTex; };

// PASS GENERATION
// ==========================================================================

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

// SHADERS
// ==========================================================================

float4 ColorPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(tex2D(ReShade::BackBuffer, uv).rgb, 1.0);
}

float4 DepthRawPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(ReShade::GetLinearizedDepth(uv), 0.0, 0.0, 1.0);
}

float4 DepthPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float depth = tex2D(NiferDepthRawSampler, uv).r;
	depth = saturate(depth * DepthMultiplier);
	depth = pow(abs(depth), DepthGamma);
	if (InvertDepth)
		depth = 1.0 - depth;
	return float4(depth.xxx, 1.0);
}

float4 NormalsPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(GetNormalsValue(uv), 1.0);
}

float4 ShadedPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(tex2D(ReShade::BackBuffer, uv).rgb, 1.0);
}

float4 OutputPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!PreviewEnabled)
		return tex2D(ReShade::BackBuffer, uv);

	if (PreviewPass == 0)
		return tex2D(NiferDepthSampler, uv);
	if (PreviewPass == 1)
		return tex2D(NiferNormalsSampler, uv);
	if (PreviewPass == 2)
		return tex2D(NiferColorSampler, uv);

	return tex2D(NiferShadedSampler, uv);
}

// TECHNIQUE
// ==========================================================================

technique NiferMultiPass <
	ui_tooltip = "Captures Color, Depth and Normals for the Nifer Multipass addon.\n"
	             "Place this at the TOP of the effect list and keep it ENABLED\n"
	             "while recording. Draws nothing on screen unless Preview is on.";
>
{
	pass ColorPass
	{
		VertexShader = PostProcessVS;
		PixelShader = ColorPS;
		RenderTarget = NiferColorTex;
	}
	pass DepthRawPass
	{
		VertexShader = PostProcessVS;
		PixelShader = DepthRawPS;
		RenderTarget = NiferDepthRawTex;
	}
	pass DepthPass
	{
		VertexShader = PostProcessVS;
		PixelShader = DepthPS;
		RenderTarget = NiferDepthTex;
	}
	pass NormalsPass
	{
		VertexShader = PostProcessVS;
		PixelShader = NormalsPS;
		RenderTarget = NiferNormalsTex;
	}
	pass OutputPass
	{
		VertexShader = PostProcessVS;
		PixelShader = OutputPS;
	}
}

technique NiferMultiPassColor <
	ui_tooltip = "Captures the Color (with shaders) pass.\n"
	             "Only needed if you want that pass. Place it at the very\n"
	             "BOTTOM of the effect list, below your other shaders.\n"
	             "Draws nothing on screen, that is normal.";
>
{
	pass ShadedPass
	{
		VertexShader = PostProcessVS;
		PixelShader = ShadedPS;
		RenderTarget = NiferShadedTex;
	}
}
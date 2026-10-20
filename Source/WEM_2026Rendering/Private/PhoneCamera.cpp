// Copyright Epic Games, Inc. All Rights Reserved.

#include "PhoneCamera.h"

#include "Camera/CameraTypes.h"
#include "Engine/Scene.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"

namespace PhoneCameraPrivate
{
	TAutoConsoleVariable<float> CVarEffects(
		TEXT("wem.Portrait.Effects"),
		1.0f,
		TEXT("How strongly the portrait frame looks like a phone camera: flat HDR with crunchy detail, punchy colour,\n")
		TEXT("over-sharpening, sensor noise, colour fringes, green lens flares, a vignette, a rolling shutter.\n")
		TEXT("0 leaves the picture alone, 1 is full, 2 goes twice as far."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarShake(
		TEXT("wem.Portrait.Shake"),
		1.0f,
		TEXT("How much the hand holding the phone moves in the portrait frame. 0 holds it still."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarRollingShutter(
		TEXT("wem.Portrait.RollingShutter"),
		0.08f,
		TEXT("Seconds the phone takes to read its sensor from the top row down. Turning while it reads leans uprights;\n")
		TEXT("tilting stretches or squashes the picture. Real phones take 0.01 to 0.03. 0 turns it off."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarSharpen(
		TEXT("wem.Portrait.Sharpen"),
		1.0f,
		TEXT("How hard the phone look sharpens, on top of wem.Portrait.Effects: the bright and dark halos along edges grow\n")
		TEXT("with it. 1 is the default, 0 none, 2 twice as much."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarFringe(
		TEXT("wem.Portrait.Fringe"),
		1.0f,
		TEXT("How far the red and blue colour fringes spread towards the frame's edges, on top of wem.Portrait.Effects.\n")
		TEXT("1 is the default, 0 none, 2 twice as far."),
		ECVF_RenderThreadSafe);

	/** A view that stops drawing (a play session that ended) is forgotten after this many frames. */
	constexpr uint64 UnusedViewFrames = 300;

	/** What one frame's pass needs, worked out when the pass is added. */
	struct FPassSettings
	{
		FVector2f ShutterSkew = FVector2f::ZeroVector;
		float FringeAmount = 0.0f;
		float SharpenAmount = 0.0f;
		float VignetteAmount = 0.0f;
		float NoiseAmount = 0.0f;
		uint32 FrameIndex = 0;
	};
}

/** PhoneCamera.usf: the stabiliser's crop, the rolling shutter, colour fringes, over-sharpening, a vignette and sensor noise. */
class FPhoneCameraPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPhoneCameraPS);
	SHADER_USE_PARAMETER_STRUCT(FPhoneCameraPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
		SHADER_PARAMETER(FVector2f, SceneColorInvExtent)
		SHADER_PARAMETER(FVector2f, SceneColorMin)
		SHADER_PARAMETER(FVector2f, SceneColorSize)
		SHADER_PARAMETER(FIntPoint, OutputMin)
		SHADER_PARAMETER(FVector2f, OutputSize)
		SHADER_PARAMETER(FVector2f, FrameShape)
		SHADER_PARAMETER(float, CropScale)
		SHADER_PARAMETER(FVector2f, ShutterSkew)
		SHADER_PARAMETER(float, FringeAmount)
		SHADER_PARAMETER(float, SharpenAmount)
		SHADER_PARAMETER(float, VignetteAmount)
		SHADER_PARAMETER(float, NoiseAmount)
		SHADER_PARAMETER(uint32, FrameIndex)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FPhoneCameraPS, "/WEM_2026/Private/PhoneCamera.usf", "MainPS", SF_Pixel);

float PhoneCamera::GetStrength()
{
	return FMath::Clamp(PhoneCameraPrivate::CVarEffects.GetValueOnAnyThread(), 0.0f, 2.0f);
}

float PhoneCamera::GetShake()
{
	return FMath::Max(PhoneCameraPrivate::CVarShake.GetValueOnGameThread(), 0.0f);
}

void PhoneCamera::AddStabilizerMargin(FMinimalViewInfo& ViewInfo)
{
	const double TanHalf = FMath::Tan(FMath::DegreesToRadians((double)ViewInfo.FOV) / 2.0) / CropScale;
	ViewInfo.FOV = FMath::RadiansToDegrees(2.0 * FMath::Atan(TanHalf));
}

FRotator PhoneCamera::HandShake(const double Seconds, const float Strength)
{
	// Each axis mixes a slow sway (arms, breathing) with a quick, small tremor that the stabiliser lets through.
	// The offsets keep the axes from moving together.
	const auto Wobble = [Seconds](float SlowHz, float SlowDegrees, float QuickHz, float QuickDegrees, float Offset)
	{
		return SlowDegrees * FMath::PerlinNoise1D((float)(Seconds * SlowHz + Offset))
			+ QuickDegrees * FMath::PerlinNoise1D((float)(Seconds * QuickHz + Offset * 3.1f));
	};

	const float Pitch = Wobble(0.31f, 0.7f, 4.7f, 0.08f, 11.3f);
	const float Yaw = Wobble(0.23f, 0.8f, 3.9f, 0.08f, 47.9f);
	const float Roll = Wobble(0.19f, 1.0f, 3.3f, 0.06f, 83.1f);
	return FRotator(Pitch, Yaw, Roll) * Strength;
}

void PhoneCamera::ApplyLook(FPostProcessSettings& Settings, const float Strength)
{
	if (Strength <= 0.0f)
	{
		return;
	}

	// Each setting moves from where the camera had it towards the phone's value, all the way at Strength 1.
	const auto Toward = [Strength](auto From, auto To) { return FMath::Lerp(From, To, Strength); };

	// The "HDR" look: highlights and shadows pressed together into a flat, evenly bright picture, with the fine
	// detail pushed up until it crunches.
	Settings.bOverride_LocalExposureHighlightContrastScale = true;
	Settings.LocalExposureHighlightContrastScale = FMath::Max(0.05f, Toward(Settings.LocalExposureHighlightContrastScale, 0.45f));
	Settings.bOverride_LocalExposureShadowContrastScale = true;
	Settings.LocalExposureShadowContrastScale = FMath::Max(0.05f, Toward(Settings.LocalExposureShadowContrastScale, 0.5f));
	Settings.bOverride_LocalExposureDetailStrength = true;
	Settings.LocalExposureDetailStrength = Toward(Settings.LocalExposureDetailStrength, 1.8f);

	// Punchy colour and a touch more contrast. All the sharpening is in the pass, under wem.Portrait.Sharpen.
	Settings.bOverride_ColorSaturation = true;
	Settings.ColorSaturation = Toward(Settings.ColorSaturation, FVector4(1.3, 1.3, 1.3, 1.0));
	Settings.bOverride_ColorContrast = true;
	Settings.ColorContrast = Toward(Settings.ColorContrast, FVector4(1.06, 1.06, 1.06, 1.0));

	// The small lens: glowing lights, but no lens flare ghosts at all, not even the engine's own: their blobs in the
	// middle of the frame were distracting. Its colour fringes and dark rim are in the pass, after the sky glitch,
	// which would otherwise paint them into its trails.
	Settings.bOverride_BloomIntensity = true;
	Settings.BloomIntensity = Toward(Settings.BloomIntensity, 1.5f);
	Settings.bOverride_LensFlareIntensity = true;
	Settings.LensFlareIntensity = 0.0f;

	// Quick auto exposure that pumps as the view moves from dark to bright, a short shutter with little motion blur,
	// and a tiny sensor that keeps everything in focus (a focal distance of 0 turns depth of field off).
	Settings.bOverride_AutoExposureSpeedUp = true;
	Settings.AutoExposureSpeedUp = Toward(Settings.AutoExposureSpeedUp, 7.0f);
	Settings.bOverride_AutoExposureSpeedDown = true;
	Settings.AutoExposureSpeedDown = Toward(Settings.AutoExposureSpeedDown, 5.0f);
	Settings.bOverride_MotionBlurAmount = true;
	Settings.MotionBlurAmount = FMath::Max(0.0f, Toward(Settings.MotionBlurAmount, 0.15f));
	Settings.bOverride_DepthOfFieldFocalDistance = true;
	Settings.DepthOfFieldFocalDistance = 0.0f;
}

FVector2f PhoneCamera::TurnRate(const FQuat& From, const FQuat& To, const double Seconds)
{
	if (Seconds <= 0.0)
	{
		return FVector2f::ZeroVector;
	}

	// The turn in the camera's own frame: yaw is to the right, pitch is up.
	const FRotator Turn = (From.Inverse() * To).Rotator();
	return FVector2f(FMath::DegreesToRadians(Turn.Yaw) / Seconds, FMath::DegreesToRadians(Turn.Pitch) / Seconds);
}

FVector2f PhoneCamera::ShutterSkew(const FVector2f& TurnRadiansPerSecond, const float ReadoutSeconds, const FMatrix& Projection)
{
	// Near the middle of the frame, a radian is half the projection's scale in frame widths (or heights).
	const float X = TurnRadiansPerSecond.X * ReadoutSeconds * 0.5f * (float)Projection.M[0][0];
	const float Y = TurnRadiansPerSecond.Y * ReadoutSeconds * 0.5f * (float)Projection.M[1][1];

	// The crop leaves (1 / CropScale - 1) to spare before a leaning or squashed picture would reach past its edges.
	// Stretching only moves inward, so it gets more room.
	const float Margin = 1.0f / CropScale - 1.0f;
	return FVector2f(FMath::Clamp(X, -Margin, Margin), FMath::Clamp(Y, -Margin, 0.5f));
}

namespace PhoneCameraPrivate
{
	FScreenPassTexture Draw_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs,
		const FPassSettings& Settings)
	{
		const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
		if (!SceneColor.IsValid())
		{
			return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		}

		FScreenPassRenderTarget Output = Inputs.OverrideOutput;
		if (!Output.IsValid())
		{
			Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("PhoneCamera.Output"));
		}

		const FIntPoint Extent = SceneColor.Texture->Desc.Extent;

		FPhoneCameraPS::FParameters* Parameters = GraphBuilder.AllocParameters<FPhoneCameraPS::FParameters>();
		Parameters->SceneColorTexture = SceneColor.Texture;
		Parameters->SceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Parameters->SceneColorInvExtent = FVector2f(1.0f / Extent.X, 1.0f / Extent.Y);
		Parameters->SceneColorMin = FVector2f(SceneColor.ViewRect.Min.X, SceneColor.ViewRect.Min.Y);
		Parameters->SceneColorSize = FVector2f(SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height());
		Parameters->OutputMin = Output.ViewRect.Min;
		Parameters->OutputSize = FVector2f(Output.ViewRect.Width(), Output.ViewRect.Height());
		Parameters->FrameShape = Parameters->OutputSize / FMath::Max3(Parameters->OutputSize.X, Parameters->OutputSize.Y, 1.0f);
		Parameters->CropScale = PhoneCamera::CropScale;
		Parameters->ShutterSkew = Settings.ShutterSkew;
		Parameters->FringeAmount = Settings.FringeAmount;
		Parameters->SharpenAmount = Settings.SharpenAmount;
		Parameters->VignetteAmount = Settings.VignetteAmount;
		Parameters->NoiseAmount = Settings.NoiseAmount;
		Parameters->FrameIndex = Settings.FrameIndex;
		Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
		TShaderMapRef<FPhoneCameraPS> PixelShader(ShaderMap);
		FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("PhoneCamera"), PixelShader, Parameters, Output.ViewRect);

		return MoveTemp(Output);
	}
}

void PhoneCamera::FPass::Subscribe_RenderThread(const FSceneView& View, FPostProcessingPassDelegateArray& InOutPassCallbacks)
{
	using namespace PhoneCameraPrivate;

	const float Strength = GetStrength();
	if (Strength <= 0.0f || !View.Family)
	{
		return;
	}

	FPassSettings Settings;
	Settings.FringeAmount = 0.0035f * Strength * FMath::Max(CVarFringe.GetValueOnRenderThread(), 0.0f);
	Settings.SharpenAmount = 0.8f * Strength * FMath::Max(CVarSharpen.GetValueOnRenderThread(), 0.0f);
	Settings.VignetteAmount = FMath::Min(0.45f * Strength, 0.95f);
	Settings.NoiseAmount = Strength;
	Settings.FrameIndex = View.Family->FrameNumber;

	{
		const uint64 Frame = GFrameCounterRenderThread;

		FScopeLock Lock(&ViewsLock);
		for (auto It = Views.CreateIterator(); It; ++It)
		{
			if (It.Value().LastUsedFrame + UnusedViewFrames < Frame)
			{
				It.RemoveCurrent();
			}
		}

		// How fast the view turns, from where it looked last frame, smoothed over a few frames so the lean does not
		// flicker. Game time, so a paused game stands still; a long gap (a hitch, a new play session) starts over.
		FViewState& State = Views.FindOrAdd(View.GetViewKey());
		const double Now = View.Family->Time.GetWorldTimeSeconds();
		const FQuat Rotation = View.ViewRotation.Quaternion();
		const double Gap = Now - State.LastSeconds;
		if (State.LastSeconds >= 0.0 && Gap > 0.0 && Gap < 0.25)
		{
			const float Blend = 1.0f - FMath::Exp(-(float)Gap / 0.04f);
			State.TurnRate = FMath::Lerp(State.TurnRate, TurnRate(State.LastRotation, Rotation, Gap), Blend);
		}
		else
		{
			State.TurnRate = FVector2f::ZeroVector;
		}
		State.LastRotation = Rotation;
		State.LastSeconds = Now;
		State.LastUsedFrame = Frame;

		const float Readout = FMath::Max(CVarRollingShutter.GetValueOnRenderThread(), 0.0f);
		Settings.ShutterSkew = ShutterSkew(State.TurnRate, Readout, View.ViewMatrices.GetViewToClipNoAA());
	}

	InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateLambda(
		[Settings](FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
		{
			return PhoneCameraPrivate::Draw_RenderThread(GraphBuilder, View, Inputs, Settings);
		}));
}

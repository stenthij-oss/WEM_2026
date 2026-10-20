// Copyright Epic Games, Inc. All Rights Reserved.

#include "SkyGlitch.h"

#include "Engine/Texture.h"
#include "Engine/World.h"
#include "FXRenderingUtils.h"
#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "RHIStaticStates.h"
#include "SceneTexturesConfig.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "SkyGlitchTitle.h"
#include "SystemTextures.h"
#include "TextureResource.h"

#include <atomic>

namespace SkyGlitchPrivate
{
	/** Bumped to wipe every trail; a trail painted under an older count starts over. */
	std::atomic<uint32> ClearCount{0};

	void Clear()
	{
		ClearCount.fetch_add(1);
	}

	/** A title as the render thread draws it (SkyGlitchTitle.h). */
	struct FTitle
	{
		FTextureRHIRef Texture;
		FLinearColor Colour = FLinearColor::White;
		float Opacity = 0.0f;
		float Width = 0.0f;
		float MaxHeight = 0.0f;

		bool IsDrawn() const
		{
			return Texture.IsValid() && Opacity > 0.0f;
		}
	};

	/**
	 * Render thread only, set by the commands SkyGlitchTitle sends: the title shown, the one to burn in, and how many
	 * burns have been asked for. A trail burns the title in once for each, on the first frame it draws after.
	 */
	FTitle ShownTitle;
	FTitle BurningTitle;
	uint32 Burns = 0;

	TAutoConsoleVariable<int32> CVarSkyGlitch(
		TEXT("wem.SkyGlitch"),
		1,
		TEXT("1 lets the platforms leave trails across the sky as the camera moves, the way a hung window smeared across\n")
		TEXT("the Windows XP desktop. Game and play-in-editor views only. Turning it off and on again wipes the trails."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { Clear(); }),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarSkyGlitchInterval(
		TEXT("wem.SkyGlitch.Interval"),
		0.0f,
		TEXT("Seconds between the copies the platforms leave in the sky. 0 leaves one every frame, so a moving platform\n")
		TEXT("smears; larger values space the copies out into separate outlines."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarSkyGlitchDistance(
		TEXT("wem.SkyGlitch.Distance"),
		100000.0f,
		TEXT("Anything farther from the camera than this, in cm, counts as sky and takes trails. Everything nearer draws\n")
		TEXT("live, always on top of the trails."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarSkyGlitchBlack(
		TEXT("wem.SkyGlitch.Black"),
		0,
		TEXT("1 draws the sky pure black while the glitch is on, so the trails pile up on black instead of on the live sky.\n")
		TEXT("Only the picture of the sky goes: the sun and sky still light the platforms as before."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarSkyGlitchResetAfter(
		TEXT("wem.SkyGlitch.ResetAfter"),
		0.0f,
		TEXT("Seconds the trails build up before they are wiped. 0 never wipes them."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarSkyGlitchTurnOnAfter(
		TEXT("wem.SkyGlitch.TurnOnAfter"),
		0.0f,
		TEXT("Seconds the glitch stays off after wem.SkyGlitch.ResetAfter wipes it, before it turns back on over a clean sky.\n")
		TEXT("While it is off no trails are drawn, and the sky shows as normal, or stays black with wem.SkyGlitch.Black.\n")
		TEXT("0 turns it straight back on."),
		ECVF_RenderThreadSafe);

	FAutoConsoleCommand CmdSkyGlitchClear(
		TEXT("wem.SkyGlitch.Clear"),
		TEXT("Wipes the trails from the sky and starts the wem.SkyGlitch.ResetAfter clock over."),
		FConsoleCommandDelegate::CreateStatic(&Clear));

	/** A view that stops drawing (a play session that ended) has its trail let go after this many frames. */
	constexpr uint64 UnusedTrailFrames = 300;

	bool IsDrawnFor(const FSceneView& View)
	{
		return View.State && View.Family && View.Family->EngineShowFlags.PostProcessing && !View.bIsSceneCapture
			&& !View.bIsReflectionCapture && !View.bIsPlanarReflection && !View.bIsVirtualTexture;
	}
}

void SkyGlitchTitle::Show(UTexture* Texture, const float Opacity, const FLinearColor& Colour, const float Width, const float MaxHeight)
{
	check(IsInGameThread());

	// The resource is let go of by a command sent after this one, so it is still there when this one runs.
	FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;

	ENQUEUE_RENDER_COMMAND(SkyGlitchShowTitle)([Resource, Opacity, Colour, Width, MaxHeight](FRHICommandListImmediate&)
	{
		SkyGlitchPrivate::FTitle& Title = SkyGlitchPrivate::ShownTitle;
		Title.Texture = Resource ? Resource->GetTextureRHI() : nullptr;
		Title.Colour = Colour;
		Title.Opacity = FMath::Clamp(Opacity, 0.0f, 1.0f);
		Title.Width = Width;
		Title.MaxHeight = MaxHeight;
	});
}

void SkyGlitchTitle::Hide()
{
	check(IsInGameThread());

	ENQUEUE_RENDER_COMMAND(SkyGlitchHideTitle)([](FRHICommandListImmediate&)
	{
		SkyGlitchPrivate::ShownTitle = SkyGlitchPrivate::FTitle();
	});
}

void SkyGlitchTitle::BurnIn()
{
	check(IsInGameThread());

	ENQUEUE_RENDER_COMMAND(SkyGlitchBurnInTitle)([](FRHICommandListImmediate&)
	{
		SkyGlitchPrivate::BurningTitle = SkyGlitchPrivate::ShownTitle;
		SkyGlitchPrivate::BurningTitle.Opacity = 1.0f;
		++SkyGlitchPrivate::Burns;
	});
}

void SkyGlitchTitle::Reset()
{
	check(IsInGameThread());

	ENQUEUE_RENDER_COMMAND(SkyGlitchResetTitle)([](FRHICommandListImmediate&)
	{
		SkyGlitchPrivate::ShownTitle = SkyGlitchPrivate::FTitle();
		SkyGlitchPrivate::BurningTitle = SkyGlitchPrivate::FTitle();
	});
}

FBox2f SkyGlitchTitle::Fit(const FIntPoint& ViewSize, const FIntPoint& TextureSize, const float Width, const float MaxHeight)
{
	if (ViewSize.X <= 0 || ViewSize.Y <= 0 || TextureSize.X <= 0 || TextureSize.Y <= 0)
	{
		return FBox2f(ForceInit);
	}

	// As wide as asked, unless that would make it taller than allowed.
	const float Aspect = static_cast<float>(TextureSize.X) / TextureSize.Y;
	FVector2f Size(FMath::Max(Width, 0.0f) * ViewSize.X, 0.0f);
	Size.Y = Size.X / Aspect;

	const float Tallest = FMath::Max(MaxHeight, 0.0f) * ViewSize.Y;
	if (Size.Y > Tallest)
	{
		Size = FVector2f(Tallest * Aspect, Tallest);
	}

	const FVector2f Min = 0.5f * (FVector2f(ViewSize) - Size);
	return FBox2f(Min, Min + Size);
}

/** Both passes of SkyGlitch.usf: STAMP paints the trail texture, otherwise it lays the trails over the frame. */
class FSkyGlitchPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSkyGlitchPS);
	SHADER_USE_PARAMETER_STRUCT(FSkyGlitchPS, FGlobalShader);

	class FStamp : SHADER_PERMUTATION_BOOL("STAMP");
	using FPermutationDomain = TShaderPermutationDomain<FStamp>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TrailTexture)
		SHADER_PARAMETER(FIntPoint, OutputMin)
		SHADER_PARAMETER(FVector2f, OutputSize)
		SHADER_PARAMETER(FIntPoint, SceneColorMin)
		SHADER_PARAMETER(FVector2f, SceneColorSize)
		SHADER_PARAMETER(FIntPoint, DepthMin)
		SHADER_PARAMETER(FIntPoint, DepthMax)
		SHADER_PARAMETER(FVector2f, DepthSize)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(float, BackgroundDistance)
		SHADER_PARAMETER(uint32, bHasTrail)
		SHADER_PARAMETER(uint32, bBlackSky)
		SHADER_PARAMETER_TEXTURE(Texture2D, TitleTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TitleSampler)
		SHADER_PARAMETER(FVector2f, TitleMin)
		SHADER_PARAMETER(FVector2f, TitleSize)
		SHADER_PARAMETER(FVector3f, TitleColour)
		SHADER_PARAMETER(float, TitleOpacity)
		SHADER_PARAMETER(float, Visibility)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSkyGlitchPS, "/WEM_2026/Private/SkyGlitch.usf", "MainPS", SF_Pixel);

FSkyGlitchViewExtension::FSkyGlitchViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FSkyGlitchViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	const UWorld* World = Context.GetWorld();
	return SkyGlitchPrivate::CVarSkyGlitch.GetValueOnAnyThread() != 0 && World && World->IsGameWorld();
}

FSkyGlitchViewExtension::FTrail& FSkyGlitchViewExtension::FindTrail_RenderThread(const FSceneView& View)
{
	const uint64 Frame = GFrameCounterRenderThread;

	FScopeLock Lock(&TrailsLock);
	for (auto It = Trails.CreateIterator(); It; ++It)
	{
		if (It.Value()->LastUsedFrame + SkyGlitchPrivate::UnusedTrailFrames < Frame)
		{
			It.RemoveCurrent();
		}
	}

	TUniquePtr<FTrail>& Slot = Trails.FindOrAdd(View.GetViewKey());
	if (!Slot)
	{
		// A view that first draws after a title was burnt in does not take it in late.
		Slot = MakeUnique<FTrail>();
		Slot->Burns = SkyGlitchPrivate::Burns;
	}
	Slot->LastUsedFrame = Frame;
	return *Slot;
}

void FSkyGlitchViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	using namespace SkyGlitchPrivate;

	// After tonemapping, so a painted copy keeps the exact colours it was seen in.
	if (Pass != EPostProcessingPass::Tonemap || !bIsPassEnabled || !IsDrawnFor(View) || CVarSkyGlitch.GetValueOnAnyThread() == 0)
	{
		return;
	}

	// This runs once a frame for each view, so the on and off times are kept here. While the glitch is off, only
	// a black sky is drawn (wem.SkyGlitch.Black), or no pass is added at all.
	FTrail& Trail = FindTrail_RenderThread(View);
	const double Now = View.Family->Time.GetRealTimeSeconds();

	const uint32 Clears = ClearCount.load();
	if (Trail.PhaseStartSeconds < 0.0 || Trail.ClearCount != Clears)
	{
		Trail.Texture.SafeRelease();
		Trail.ClearCount = Clears;
		Trail.PhaseStartSeconds = Now;
		Trail.bOff = false;
	}

	const double InPhase = Now - Trail.PhaseStartSeconds;
	if (Trail.bOff)
	{
		if (InPhase >= CVarSkyGlitchTurnOnAfter.GetValueOnAnyThread())
		{
			Trail.bOff = false;
			Trail.PhaseStartSeconds = Now;
		}
	}
	else
	{
		const float ResetAfter = CVarSkyGlitchResetAfter.GetValueOnAnyThread();
		if (ResetAfter > 0.0f && InPhase >= ResetAfter)
		{
			Trail.Texture.SafeRelease();
			Trail.bOff = CVarSkyGlitchTurnOnAfter.GetValueOnAnyThread() > 0.0f;
			Trail.PhaseStartSeconds = Now;
		}
	}

	// A black sky stays black throughout, also while the glitch is off.
	if (!Trail.bOff || CVarSkyGlitchBlack.GetValueOnAnyThread() != 0)
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FSkyGlitchViewExtension::DrawAfterTonemap_RenderThread));
	}
	else
	{
		// A title burnt in while the glitch is off is not burnt in later over a clean sky.
		Trail.Burns = Burns;
	}
}

FScreenPassTexture FSkyGlitchViewExtension::DrawAfterTonemap_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	using namespace SkyGlitchPrivate;

	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	FRDGTextureRef SceneDepth = Inputs.SceneTextures.SceneTextures ? Inputs.SceneTextures.SceneTextures->GetContents()->SceneDepthTexture : nullptr;
	if (!SceneColor.IsValid() || !SceneDepth)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("SkyGlitch.Output"));
	}

	const FIntPoint Size = Output.ViewRect.Size();

	FTrail* Trail = &FindTrail_RenderThread(View);
	if (Trail->Texture && Trail->Texture->GetDesc().Extent != Size)
	{
		Trail->Texture.SafeRelease();
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	// Depth is at render resolution, in the view's own rect.
	const FIntRect DepthRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);
	const auto MakeParameters = [&](FIntPoint OutputMin, const FTitle& Title)
	{
		FSkyGlitchPS::FParameters* Parameters = GraphBuilder.AllocParameters<FSkyGlitchPS::FParameters>();
		Parameters->SceneColorTexture = SceneColor.Texture;
		Parameters->SceneDepthTexture = SceneDepth;
		Parameters->OutputMin = OutputMin;
		Parameters->OutputSize = FVector2f(Size.X, Size.Y);
		Parameters->SceneColorMin = SceneColor.ViewRect.Min;
		Parameters->SceneColorSize = FVector2f(SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height());
		Parameters->DepthMin = DepthRect.Min;
		Parameters->DepthMax = DepthRect.Max;
		Parameters->DepthSize = FVector2f(DepthRect.Width(), DepthRect.Height());
		Parameters->InvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;
		Parameters->BackgroundDistance = CVarSkyGlitchDistance.GetValueOnAnyThread();
		Parameters->bBlackSky = CVarSkyGlitchBlack.GetValueOnAnyThread() != 0 ? 1 : 0;

		const FBox2f TitleBox = Title.IsDrawn()
			? SkyGlitchTitle::Fit(Size, FIntPoint(Title.Texture->GetDesc().Extent), Title.Width, Title.MaxHeight)
			: FBox2f(ForceInit);
		const bool bTitle = TitleBox.bIsValid && TitleBox.GetSize().X > 0.0f && TitleBox.GetSize().Y > 0.0f;
		Parameters->TitleTexture = bTitle ? Title.Texture.GetReference() : GBlackTexture->TextureRHI.GetReference();
		Parameters->TitleSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Parameters->TitleMin = bTitle ? TitleBox.Min : FVector2f::ZeroVector;
		Parameters->TitleSize = bTitle ? TitleBox.GetSize() : FVector2f::UnitVector;
		Parameters->TitleColour = FVector3f(Title.Colour);
		Parameters->TitleOpacity = bTitle ? Title.Opacity : 0.0f;
		return Parameters;
	};

	FRDGTextureRef TrailTexture = Trail->Texture ? GraphBuilder.RegisterExternalTexture(Trail->Texture) : nullptr;

	// A title asked to be burnt in goes into this frame's stamp, whatever the interval - unless the glitch is off, when
	// it is not burnt in at all.
	const bool bBurn = Trail->Burns != Burns && BurningTitle.IsDrawn();
	Trail->Burns = Burns;

	const double Now = View.Family->Time.GetRealTimeSeconds();
	const double Interval = FMath::Max(CVarSkyGlitchInterval.GetValueOnAnyThread(), 0.0f);
	// Nothing is painted while the glitch is off; only the black sky is drawn then.
	if (!Trail->bOff && (!TrailTexture || Now - Trail->LastStampSeconds >= Interval || bBurn))
	{
		FRDGTextureRef Painted = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(Size, PF_FloatRGBA, FClearValueBinding::Transparent, TexCreate_ShaderResource | TexCreate_RenderTargetable),
			TEXT("SkyGlitch.Trail"));

		FSkyGlitchPS::FPermutationDomain Permutation;
		Permutation.Set<FSkyGlitchPS::FStamp>(true);
		TShaderMapRef<FSkyGlitchPS> PixelShader(ShaderMap, Permutation);

		FSkyGlitchPS::FParameters* Parameters = MakeParameters(FIntPoint::ZeroValue, bBurn ? BurningTitle : FTitle());
		Parameters->TrailTexture = TrailTexture ? TrailTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
		Parameters->bHasTrail = TrailTexture ? 1 : 0;
		Parameters->RenderTargets[0] = FRenderTargetBinding(Painted, ERenderTargetLoadAction::ENoAction);

		FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("SkyGlitch.Stamp"), PixelShader, Parameters,
			FIntRect(FIntPoint::ZeroValue, Size));

		GraphBuilder.QueueTextureExtraction(Painted, &Trail->Texture);
		Trail->LastStampSeconds = Now;
		TrailTexture = Painted;
	}

	FSkyGlitchPS::FPermutationDomain Permutation;
	Permutation.Set<FSkyGlitchPS::FStamp>(false);
	TShaderMapRef<FSkyGlitchPS> PixelShader(ShaderMap, Permutation);

	FSkyGlitchPS::FParameters* Parameters = MakeParameters(Output.ViewRect.Min, ShownTitle);
	Parameters->TrailTexture = TrailTexture ? TrailTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
	Parameters->bHasTrail = TrailTexture ? 1 : 0;
	Parameters->Visibility = Trail->bOff ? 0.0f : 1.0f;
	Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("SkyGlitch.Composite"), PixelShader, Parameters,
		Output.ViewRect);

	return MoveTemp(Output);
}

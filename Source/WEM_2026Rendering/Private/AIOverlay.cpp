// Copyright Epic Games, Inc. All Rights Reserved.

#include "AIOverlay.h"

#include "Engine/World.h"
#include "FXRenderingUtils.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneTexturesConfig.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogAIOverlay, Log, All);

namespace AIOverlayPrivate
{
	/** Bumped by wem.AIOverlay.Start; the render thread starts a cycle when it sees it change. */
	std::atomic<uint32> StartRequests{0};

	TAutoConsoleVariable<int32> CVarAIOverlay(
		TEXT("wem.AIOverlay"),
		1,
		TEXT("1 lets the AI (Tools/AIOverlay/bridge.py) take over the platform pieces on screen every so often: it ramps in,\n")
		TEXT("holds, and ramps out. Game and play-in-editor views only. Without the bridge running nothing changes."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarGapMin(
		TEXT("wem.AIOverlay.GapMin"),
		20.0f,
		TEXT("Shortest time, in seconds, the picture stays normal between two AI cycles."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarGapMax(
		TEXT("wem.AIOverlay.GapMax"),
		30.0f,
		TEXT("Longest time, in seconds, the picture stays normal between two AI cycles. Each gap is picked at random\n")
		TEXT("between wem.AIOverlay.GapMin and this."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarRampUp(
		TEXT("wem.AIOverlay.RampUp"),
		8.0f,
		TEXT("Seconds the AI takes to come in fully at the start of a cycle."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarHold(
		TEXT("wem.AIOverlay.Hold"),
		5.0f,
		TEXT("Seconds the AI stays fully in before it ramps out."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarRampDown(
		TEXT("wem.AIOverlay.RampDown"),
		8.0f,
		TEXT("Seconds the AI takes to go again at the end of a cycle."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarConstant(
		TEXT("wem.AIOverlay.Constant"),
		0,
		TEXT("1 keeps the AI in: it ramps in once, with no gap first, and stays at full. Set back to 0, it holds for\n")
		TEXT("wem.AIOverlay.Hold seconds, ramps out, and the gaps start again."),
		ECVF_RenderThreadSafe);

	/** Read by ARoomManager, on the game thread, as it sorts the pieces it draws. */
	TAutoConsoleVariable<float> CVarSegmentPercent(
		TEXT("wem.AIOverlay.SegmentPercent"),
		100.0f,
		TEXT("What share of the segments, in percent, the AI takes over; the others stay as they are. Each segment is in\n")
		TEXT("or out by itself, its fill on both faces together, and comes with the beams and posts round its rim, which\n")
		TEXT("also frame its neighbours. A higher share keeps every segment a lower one took in. Changed during play,\n")
		TEXT("every piece that changes sides is drawn again, which takes a moment."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarOpacity(
		TEXT("wem.AIOverlay.Opacity"),
		1.0f,
		TEXT("How much of the AI's picture covers the pieces at the height of a cycle. 1 covers them completely."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarStrength(
		TEXT("wem.AIOverlay.Strength"),
		0.6f,
		TEXT("How far the AI may move from the pieces it is shown, at the height of a cycle: 0 hands them back as they\n")
		TEXT("are, 1 lets it paint whatever the prompt asks for. It ramps up and down with the cycle."),
		ECVF_RenderThreadSafe);

	/** wem.AIOverlay.Prompt as the render thread sees it: a text console variable cannot be render thread safe itself. */
	FCriticalSection PromptLock;
	FString RenderThreadPrompt;

	TAutoConsoleVariable<FString> CVarPrompt(
		TEXT("wem.AIOverlay.Prompt"),
		TEXT(""),
		TEXT("What the AI should turn the pieces into. Empty leaves the bridge on the prompt it was started with."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Variable)
		{
			FScopeLock Lock(&PromptLock);
			RenderThreadPrompt = Variable->GetString();
		}));

	FString GetPrompt_RenderThread()
	{
		FScopeLock Lock(&PromptLock);
		return RenderThreadPrompt;
	}

	TAutoConsoleVariable<int32> CVarClipToMask(
		TEXT("wem.AIOverlay.ClipToMask"),
		1,
		TEXT("1 cuts the AI's picture to where the pieces were when it was sent as well as to where they are now, so it\n")
		TEXT("does not smear past them while the camera moves (it comes back a few frames late). 0 lets it trail."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarStencil(
		TEXT("wem.AIOverlay.Stencil"),
		1,
		TEXT("The custom stencil value that marks the platform pieces (AIOverlayStencil in RoomManager.cpp)."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarDepth(
		TEXT("wem.AIOverlay.Depth"),
		1,
		TEXT("1 sends the pieces' depth with each picture when the bridge's AI takes a depth map (its depth ControlNet), so\n")
		TEXT("the AI keeps their shape and what is in front of what, even at a strength that repaints their colours completely."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarDepthWeight(
		TEXT("wem.AIOverlay.DepthWeight"),
		0.7f,
		TEXT("How strongly the depth map steers the AI (the depth ControlNet's weight). 0 leaves the AI to the picture\n")
		TEXT("alone; above 1 holds the shape harder still."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarDepthFit(
		TEXT("wem.AIOverlay.DepthFit"),
		1,
		TEXT("1 fits the depth map to the view: the nearest pieces on screen are white and the furthest black, as in the\n")
		TEXT("depth maps the depth ControlNet learned from, and the range follows the camera over a fraction of a second.\n")
		TEXT("0 keeps it from wem.AIOverlay.DepthNear to .DepthFar."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarDepthNear(
		TEXT("wem.AIOverlay.DepthNear"),
		0.5f,
		TEXT("In metres: pieces this near the camera, or nearer, are white in the depth map. Fitted to the view, the\n")
		TEXT("nearest the range goes."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarDepthFar(
		TEXT("wem.AIOverlay.DepthFar"),
		200.0f,
		TEXT("In metres: pieces this far from the camera, or further, are black in the depth map, like the background. In\n")
		TEXT("between, even steps of grey are even ratios of distance. Fitted to the view, the furthest the range goes."),
		ECVF_RenderThreadSafe);

	/** A fitted depth range spans at least this ratio, so a wall seen straight on does not turn its every speck of depth
	 * into black and white. */
	constexpr float MinDepthRatio = 2.0f;

	/** wem.AIOverlay.DepthNear and .DepthFar in world units (centimetres), at least MinDepthRatio apart. */
	FVector2f GetDepthLimits()
	{
		const float Near = FMath::Max(CVarDepthNear.GetValueOnRenderThread() * 100.0f, 1.0f);
		return FVector2f(Near, FMath::Max(CVarDepthFar.GetValueOnRenderThread() * 100.0f, Near * MinDepthRatio));
	}

	/** The distance a depth map brightness (0..1) stands for on a range from Near to Far, as in AIOverlay.usf. */
	float BrightnessToDistance(const float Brightness, const FVector2f Range)
	{
		return Range.X * FMath::Exp((1.0f - Brightness) * FMath::Loge(Range.Y / Range.X));
	}

	/** wem.AIOverlay.Show: the AI's picture over the pieces, or what the AI is sent, over the whole view. */
	constexpr int32 ShowAI = 0;
	constexpr int32 ShowPicture = 1;
	constexpr int32 ShowDepth = 2;

	TAutoConsoleVariable<int32> CVarShow(
		TEXT("wem.AIOverlay.Show"),
		ShowAI,
		TEXT("What the overlay draws: 0 the AI's picture over the pieces; 1 the picture the AI is sent, over the whole view;\n")
		TEXT("2 the depth map it is sent. 1 and 2 draw all the time, also between cycles and without the bridge."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarPlayer(
		TEXT("wem.AIOverlay.Player"),
		0,
		TEXT("With more than one player, whose screen the AI comes to: 0 the players take turns, a cycle each, the first\n")
		TEXT("cycle to player 1 (left); 1 always player 1; 2 always player 2. One screen at a time either way, so the bridge\n")
		TEXT("works no harder than for one player. With one player it is always theirs."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarMaxRate(
		TEXT("wem.AIOverlay.MaxRate"),
		0.0f,
		TEXT("At most this many pictures a second for the AI to make. 0 lets it make one from every frame it can (unless the\n")
		TEXT("bridge was started with a --max-rate of its own), so it follows the game's frame rate as closely as the GPU they\n")
		TEXT("share allows. 15 is how it ran until 2026-10-20: the game keeps more of the GPU, and the AI changes in steps."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarReproject(
		TEXT("wem.AIOverlay.Reproject"),
		1,
		TEXT("1 lays each of the AI's pictures where the pieces it shows are now, every frame: a picture comes back a few\n")
		TEXT("frames late, and is moved, by the pieces' depth, from where the camera was when it was sent to where it is.\n")
		TEXT("0 lays it where it was on screen, so it trails (or, with ClipToMask, is cut) while the camera moves."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarBlend(
		TEXT("wem.AIOverlay.Blend"),
		1,
		TEXT("1 fades each new picture of the AI's in over the one before, over the time between two of them (at most a\n")
		TEXT("fifth of a second), so the pieces change smoothly rather than in steps. 0 switches at once."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarSize(
		TEXT("wem.AIOverlay.Size"),
		0,
		TEXT("Experimental. The side, in pixels, of the pictures the AI works at: 384 to 1024 in steps of 64; 0 keeps the\n")
		TEXT("bridge's own (512 unless it was started with another). Smaller is faster (384: about a fifth more pictures a\n")
		TEXT("second) and coarser. Changed, the bridge loads its model again at the new size, which pauses the AI a few seconds."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarMatchFrameRate(
		TEXT("wem.AIOverlay.MatchFrameRate"),
		0,
		TEXT("Experimental. 1 holds the game's frame rate (t.MaxFPS) near the AI's pictures a second while it trades pictures\n")
		TEXT("with the bridge, where about 85% of the frames get a picture of their own: the game leaves the AI the GPU time\n")
		TEXT("it would have spent on frames the AI could not keep up with, and the two settle where the GPU is full. t.MaxFPS\n")
		TEXT("is put back as it was when the cycle ends."),
		ECVF_RenderThreadSafe);

	FAutoConsoleCommand CmdStart(
		TEXT("wem.AIOverlay.Start"),
		TEXT("Starts an AI cycle now instead of waiting for the gap to end."),
		FConsoleCommandDelegate::CreateLambda([]() { StartRequests.fetch_add(1); }));

	/** Pictures that can be on their way back from the GPU at once; a frame with none free sends nothing. */
	constexpr int32 ReadbackCount = 3;

	/** Pictures sent whose view is kept, to lay the AI's answer to them where the pieces are now; an answer to an older one
	 * is laid as it lies on screen. At 120 frames a second this covers a fifth of a second. */
	constexpr int32 SentViewCount = 24;

	/** How far, as a share of the depth, a spot of the pieces may lie from the depths a picture saw there and still count
	 * as seen by it. */
	constexpr float SeenDepthTolerance = 0.03f;

	/** The longest a new picture of the AI's takes to fade in over the one before. */
	constexpr double MaxFadeSeconds = 0.2;

	/** No picture from the AI for this long: how many it makes a second is unknown again, and the frame rate is let go. */
	constexpr double RateTimeoutSeconds = 0.5;

	/** The bridge counts as gone once its heartbeat has stood still for this long. */
	constexpr double BridgeTimeoutSeconds = 3.0;

	/** No game view drawn for this long (a play session that ended, a minimized editor): the next one starts a fresh gap. */
	constexpr double RestartAfterSeconds = 2.0;

	bool IsDrawnFor(const FSceneView& View)
	{
		return View.State && View.Family && View.Family->EngineShowFlags.PostProcessing && !View.bIsSceneCapture
			&& !View.bIsReflectionCapture && !View.bIsPlanarReflection && !View.bIsVirtualTexture;
	}

	/** The cycle's timing, for a frame of Views player views. */
	AIOverlay::FTiming ReadTiming(const int32 Views)
	{
		AIOverlay::FTiming Timing;
		Timing.GapMin = CVarGapMin.GetValueOnRenderThread();
		Timing.GapMax = CVarGapMax.GetValueOnRenderThread();
		Timing.RampUp = CVarRampUp.GetValueOnRenderThread();
		Timing.Hold = CVarHold.GetValueOnRenderThread();
		Timing.RampDown = CVarRampDown.GetValueOnRenderThread();
		Timing.bConstant = CVarConstant.GetValueOnRenderThread() != 0;

		// Players taking turns keep doing so with the AI kept in: one cycle straight after another, each on the next screen.
		if (Timing.bConstant && Views > 1 && CVarPlayer.GetValueOnRenderThread() <= 0)
		{
			Timing.bConstant = false;
			Timing.GapMin = 0.0f;
			Timing.GapMax = 0.0f;
		}
		return Timing;
	}

	/** Eases in and out, so a ramp starts and lands softly. */
	float Ease(const double X)
	{
		const float T = FMath::Clamp((float)X, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	uint32 ReadSequence(const AIOverlay::FSharedHeader& Header)
	{
		return (uint32)FPlatformAtomics::AtomicRead((volatile const int32*)&Header.Sequence);
	}

	void WriteSequence(AIOverlay::FSharedHeader& Header, const uint32 Sequence)
	{
		FPlatformAtomics::InterlockedExchange((volatile int32*)&Header.Sequence, (int32)Sequence);
	}
}

FString AIOverlay::GetSharedName(const TCHAR* Name, const int32 Channel)
{
	// Windows shares a block without administrator rights only under a bare {GUID} (MapNamedSharedMemoryRegion), so a
	// channel adds itself to the GUID's second group of digits.
	FString Result(Name);
	if (Channel != 0)
	{
		const uint32 Group = FParse::HexNumber(*Result.Mid(10, 4));
		Result = Result.Left(10) + FString::Printf(TEXT("%04X"), (Group + (uint32)Channel) & 0xFFFF) + Result.Mid(14);
	}
	return Result;
}

FMatrix AIOverlay::MakeTranslatedWorldToEarlierClip(const FVector& PreViewTranslation, const FVector& EarlierPreViewTranslation,
	const FMatrix& EarlierTranslatedWorldToClip)
{
	// Translated world = world + PreViewTranslation, so from this view's to the earlier one's is the difference.
	return FTranslationMatrix(EarlierPreViewTranslation - PreViewTranslation) * EarlierTranslatedWorldToClip;
}

FMatrix AIOverlay::GetTranslatedWorldToClipNoAA(const FViewMatrices& ViewMatrices)
{
	// Worked out from the jittered projection rather than read from ViewToClipNoAA, which only the renderer fills in.
	return ViewMatrices.GetTranslatedViewMatrix() * ViewMatrices.ComputeProjectionNoAAMatrix();
}

float AIOverlay::GetMatchedFrameRate(const float Rate)
{
	// The AI can never make more pictures than the game draws frames, so a hold only a hair above its rate creeps down
	// with every hitch and never comes back up. Held at the rate that gives this share of the frames a picture of their
	// own, the hold rises while the AI keeps up with every frame and falls while it misses more than that.
	constexpr float Share = 0.85f;
	return FMath::Clamp(Rate / Share, 20.0f, 240.0f);
}

int32 AIOverlay::PickTurn(const uint32 CycleCount, const int32 Views, const int32 Player)
{
	if (Views <= 1)
	{
		return 0;
	}
	if (Player > 0)
	{
		return Player <= Views ? Player - 1 : INDEX_NONE;
	}
	return CycleCount > 0 ? (int32)((CycleCount - 1) % (uint32)Views) : 0;
}

float AIOverlay::FCycle::Advance(const double Now, const FTiming& Timing)
{
	using namespace AIOverlayPrivate;

	const float GapMin = FMath::Max(Timing.GapMin, 0.0f);
	const float GapMax = FMath::Max(Timing.GapMax, GapMin);

	if (GapEnd < 0.0)
	{
		GapEnd = Now + Random.FRandRange(GapMin, GapMax);
	}

	if (CycleStart < 0.0)
	{
		if (bStartNow || Timing.bConstant)
		{
			GapEnd = Now;
		}
		bStartNow = false;

		if (Now < GapEnd)
		{
			return 0.0f;
		}

		CycleStart = Now;
		++CycleCount;
	}
	bStartNow = false;

	const double Up = FMath::Max(Timing.RampUp, 0.0f);
	const double Hold = FMath::Max(Timing.Hold, 0.0f);
	const double Down = FMath::Max(Timing.RampDown, 0.0f);
	double T = Now - CycleStart;

	// Kept in: once up, the cycle is held at the start of its hold. Caught ramping out, it turns back up from the same
	// value, since both ramps ease alike.
	if (Timing.bConstant && T >= Up)
	{
		const double Left = Down > 0.0 && T >= Up + Hold ? FMath::Max(1.0 - (T - Up - Hold) / Down, 0.0) : 1.0;
		T = Up * Left;
		CycleStart = Now - T;

		if (Left >= 1.0)
		{
			return 1.0f;
		}
	}

	if (T < Up)
	{
		return Ease(T / Up);
	}
	if (T < Up + Hold)
	{
		return 1.0f;
	}
	if (T < Up + Hold + Down)
	{
		return Ease(1.0 - (T - Up - Hold) / Down);
	}

	// Over: the next gap starts where this cycle ended.
	CycleStart = -1.0;
	GapEnd = Now + Random.FRandRange(GapMin, GapMax);
	return 0.0f;
}

/**
 * The passes of AIOverlay.usf: PACK makes the bridge's picture and PACK DEPTH its depth map, PACK SENT DEPTH keeps the
 * depths the game reprojects the AI's answer by, UNPACK takes the AI's picture back, COMPOSITE lays it over, and SHOW
 * draws what the AI is sent instead.
 */
class FAIOverlayPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAIOverlayPS);
	SHADER_USE_PARAMETER_STRUCT(FAIOverlayPS, FGlobalShader);

	class FStage : SHADER_PERMUTATION_INT("STAGE", 6);
	using FPermutationDomain = TShaderPermutationDomain<FStage>;

	static constexpr int32 StageComposite = 0;
	static constexpr int32 StagePack = 1;
	static constexpr int32 StageUnpack = 2;
	static constexpr int32 StagePackDepth = 3;
	static constexpr int32 StageShow = 4;
	static constexpr int32 StagePackSentDepth = 5;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencilTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AITexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AIPrevTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SentDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevSentDepthTexture)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AIPixels)
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER(FIntPoint, OutputMin)
		SHADER_PARAMETER(FVector2f, OutputSize)
		SHADER_PARAMETER(FIntPoint, SceneColorMin)
		SHADER_PARAMETER(FVector2f, SceneColorSize)
		SHADER_PARAMETER(FVector2f, SceneColorInvExtent)
		SHADER_PARAMETER(FIntPoint, StencilMin)
		SHADER_PARAMETER(FIntPoint, StencilMax)
		SHADER_PARAMETER(FVector2f, StencilSize)
		SHADER_PARAMETER(uint32, StencilValue)
		SHADER_PARAMETER(uint32, AIWidth)
		SHADER_PARAMETER(float, Weight)
		SHADER_PARAMETER(float, ClipToMask)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(float, DepthRangeNear)
		SHADER_PARAMETER(float, DepthRangeFar)
		SHADER_PARAMETER(float, DepthNear)
		SHADER_PARAMETER(float, DepthFar)
		SHADER_PARAMETER(FVector4f, AIUVScaleBias)
		SHADER_PARAMETER(FMatrix44f, ClipToTranslatedWorld)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToSentClip)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToPrevSentClip)
		SHADER_PARAMETER(FVector4f, ReprojectFlags)
		SHADER_PARAMETER(float, Blend)
		SHADER_PARAMETER(float, DepthTolerance)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FAIOverlayPS, "/WEM_2026/Private/AIOverlay.usf", "MainPS", SF_Pixel);

FAIOverlayViewExtension::FAIOverlayViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
	, Cycle((int32)FPlatformTime::Cycles())
{
	SentViews.SetNum(AIOverlayPrivate::SentViewCount);
	FParse::Value(FCommandLine::Get(), TEXT("AIOverlayChannel="), Channel);
	BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddRaw(this, &FAIOverlayViewExtension::HoldFrameRate_GameThread);
}

FAIOverlayViewExtension::~FAIOverlayViewExtension()
{
	FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
	if (HeldFrameRate >= 0.0f)
	{
		if (IConsoleVariable* MaxFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
		{
			MaxFPS->Set(FrameRateBeforeHold, ECVF_SetByCode);
		}
	}

	if (InputRegion)
	{
		FPlatformMemory::UnmapNamedSharedMemoryRegion(InputRegion);
	}
	if (OutputRegion)
	{
		FPlatformMemory::UnmapNamedSharedMemoryRegion(OutputRegion);
	}
	if (ControlRegion)
	{
		FPlatformMemory::UnmapNamedSharedMemoryRegion(ControlRegion);
	}
}

bool FAIOverlayViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	const UWorld* World = Context.GetWorld();
	return AIOverlayPrivate::CVarAIOverlay.GetValueOnAnyThread() != 0 && World && World->IsGameWorld();
}

bool FAIOverlayViewExtension::MapShared_RenderThread()
{
	using namespace AIOverlay;

	if (InputRegion && OutputRegion && ControlRegion)
	{
		return true;
	}
	if (bMapFailed)
	{
		return false;
	}

	// Created by whichever side starts first and opened by the other, so the game and the bridge can start in any order.
	const uint32 Access = FPlatformMemory::ESharedMemoryAccess::Read | FPlatformMemory::ESharedMemoryAccess::Write;
	InputRegion = FPlatformMemory::MapNamedSharedMemoryRegion(*GetSharedName(InputName, Channel), true, Access, SharedSize);
	OutputRegion = FPlatformMemory::MapNamedSharedMemoryRegion(*GetSharedName(OutputName, Channel), true, Access, SharedSize);
	ControlRegion = FPlatformMemory::MapNamedSharedMemoryRegion(*GetSharedName(ControlName, Channel), true, Access, SharedSize);
	if (!InputRegion || !OutputRegion || !ControlRegion)
	{
		UE_LOG(LogAIOverlay, Warning, TEXT("Could not map the shared memory the AI bridge uses; the AI overlay stays off."));
		bMapFailed = true;
		return false;
	}
	if (Channel != 0)
	{
		UE_LOG(LogAIOverlay, Log, TEXT("The AI overlay trades pictures on channel %d (a bridge started with --channel %d)."), Channel, Channel);
	}

	FSharedHeader& Input = *(FSharedHeader*)InputRegion->GetAddress();
	Input.Magic = SharedMagic;
	Input.Version = SharedVersion;
	return true;
}

void FAIOverlayViewExtension::WriteWishes_RenderThread()
{
	using namespace AIOverlay;
	using namespace AIOverlayPrivate;

	if (!MapShared_RenderThread())
	{
		return;
	}

	// Every frame, also between cycles, so a new size is loaded during a gap rather than at the start of the next cycle.
	FSharedHeader& Input = *(FSharedHeader*)InputRegion->GetAddress();
	Input.MaxRate = FMath::Max(CVarMaxRate.GetValueOnRenderThread(), 0.0f);
	const int32 Side = CVarSize.GetValueOnRenderThread();
	Input.Size = Side > 0 ? (uint32)FMath::Clamp(Side / 64 * 64, 384, (int32)MaxSide) : 0;
}

void FAIOverlayViewExtension::HoldFrameRate_GameThread()
{
	using namespace AIOverlayPrivate;

	// What the render thread last saw: the AI's pictures a second while they are to be matched, or 0. Stale once no game
	// view has drawn for a while (the play session ended).
	const float Rate = MatchRate.load();
	const bool bHold = Rate > 0.0f && FPlatformTime::Seconds() - MatchRateSeconds.load() < RateTimeoutSeconds;
	if (!bHold && HeldFrameRate < 0.0f)
	{
		return;
	}

	IConsoleVariable* MaxFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
	if (!MaxFPS)
	{
		return;
	}

	if (!bHold)
	{
		MaxFPS->Set(FrameRateBeforeHold, ECVF_SetByCode);
		HeldFrameRate = -1.0f;
		UE_LOG(LogAIOverlay, Log, TEXT("The game's frame rate is free again (t.MaxFPS %g)."), FrameRateBeforeHold);
		return;
	}

	const bool bStarting = HeldFrameRate < 0.0f;
	if (bStarting)
	{
		FrameRateBeforeHold = MaxFPS->GetFloat();
	}
	// Never above a cap the game already had.
	float Target = AIOverlay::GetMatchedFrameRate(Rate);
	if (FrameRateBeforeHold > 0.0f)
	{
		Target = FMath::Min(Target, FrameRateBeforeHold);
	}
	if (bStarting || FMath::Abs(Target - HeldFrameRate) >= 1.0f)
	{
		if (bStarting)
		{
			UE_LOG(LogAIOverlay, Log, TEXT("Holding the game's frame rate to the AI's pictures a second (%.0f a second, for now)."), Target);
		}
		MaxFPS->Set(FMath::RoundToFloat(Target), ECVF_SetByCode);
		HeldFrameRate = Target;
	}
}

bool FAIOverlayViewExtension::IsBridgeAlive_RenderThread(const double Now)
{
	using namespace AIOverlay;

	if (!MapShared_RenderThread())
	{
		return false;
	}

	const FSharedHeader& Output = *(const FSharedHeader*)OutputRegion->GetAddress();
	const bool bValid = Output.Magic == SharedMagic && Output.Version == SharedVersion;
	if (bValid && Output.Heartbeat != LastHeartbeat)
	{
		LastHeartbeat = Output.Heartbeat;
		LastHeartbeatChangeSeconds = Now;
	}

	const bool bAlive = bValid && Now - LastHeartbeatChangeSeconds < AIOverlayPrivate::BridgeTimeoutSeconds;
	if (bAlive)
	{
		// The size the bridge's model was built for; the picture is stretched to it and back.
		const auto Side = [](const uint32 Value) { return FMath::Clamp((int32)Value / 8 * 8, 64, (int32)MaxSide); };
		BridgeSize = FIntPoint(Side(Output.Width), Side(Output.Height));
		BridgeControls = Output.Controls;
		BridgeRate = FMath::Max(Output.Rate, 0.0f);
	}
	if (bAlive != bBridgeWasAlive)
	{
		UE_LOG(LogAIOverlay, Log, TEXT("%s"), bAlive ? TEXT("AI bridge found.") : TEXT("AI bridge gone; the overlay waits for it."));
		bBridgeWasAlive = bAlive;
	}
	return bAlive;
}

void FAIOverlayViewExtension::SendReadyPictures_RenderThread(const float InIntensity, const bool bSend)
{
	using namespace AIOverlay;
	using namespace AIOverlayPrivate;

	// Only the newest finished picture matters: the bridge always works on the latest one.
	FPendingReadback* Newest = nullptr;
	for (FPendingReadback& Pending : Readbacks)
	{
		if (Pending.bInFlight && Pending.Readback->IsReady())
		{
			if (Newest)
			{
				if (Newest->FrameId > Pending.FrameId)
				{
					Pending.bInFlight = false;
					continue;
				}
				Newest->bInFlight = false;
			}
			Newest = &Pending;
		}
	}
	if (!Newest)
	{
		return;
	}
	Newest->bInFlight = false;

	int32 RowPitchInPixels = 0;
	const uint8* Source = (const uint8*)Newest->Readback->Lock(RowPitchInPixels);
	if (!Source)
	{
		return;
	}

	if (Newest->bHasDepth)
	{
		FitDepthRange_RenderThread(Source + SIZE_T(Newest->Size.Y) * RowPitchInPixels * 4, RowPitchInPixels, Newest->Size,
			Newest->DepthLimits);
	}
	if (!bSend)
	{
		Newest->Readback->Unlock();
		return;
	}

	uint8* Base = (uint8*)InputRegion->GetAddress();
	FSharedHeader& Header = *(FSharedHeader*)Base;

	const uint32 Sequence = ReadSequence(Header) | 1;
	WriteSequence(Header, Sequence);

	Header.Magic = SharedMagic;
	Header.Version = SharedVersion;
	Header.Width = Newest->Size.X;
	Header.Height = Newest->Size.Y;
	Header.FrameId = Newest->FrameId;
	Header.Intensity = InIntensity;
	Header.Strength = FMath::Clamp(CVarStrength.GetValueOnRenderThread(), 0.0f, 1.0f) * InIntensity;

	const FTCHARToUTF8 PromptUtf8(*GetPrompt_RenderThread());
	Header.PromptLength = FMath::Min(PromptUtf8.Length(), (int32)sizeof(Header.Prompt));
	FMemory::Memcpy(Header.Prompt, PromptUtf8.Get(), Header.PromptLength);

	// A weight tells the bridge a depth map came with the picture; with none, it leaves its ControlNet out.
	const bool bSendDepth = Newest->bHasDepth && (BridgeControls & ControlDepth) != 0 && CVarDepth.GetValueOnRenderThread() != 0;
	Header.DepthWeight = bSendDepth ? FMath::Max(CVarDepthWeight.GetValueOnRenderThread(), 0.0f) : 0.0f;

	const SIZE_T RowBytes = SIZE_T(Newest->Size.X) * 4;
	for (int32 Row = 0; Row < Newest->Size.Y; ++Row)
	{
		FMemory::Memcpy(Base + PixelOffset + Row * RowBytes, Source + SIZE_T(Row) * RowPitchInPixels * 4, RowBytes);
	}
	if (bSendDepth)
	{
		// The depth map is the lower half of the texture.
		uint8* ControlBase = (uint8*)ControlRegion->GetAddress();
		for (int32 Row = 0; Row < Newest->Size.Y; ++Row)
		{
			FMemory::Memcpy(ControlBase + PixelOffset + Row * RowBytes,
				Source + SIZE_T(Newest->Size.Y + Row) * RowPitchInPixels * 4, RowBytes);
		}
	}
	Newest->Readback->Unlock();

	WriteSequence(Header, Sequence + 1);
}

void FAIOverlayViewExtension::FitDepthRange_RenderThread(const uint8* Rows, const int32 RowPitchInPixels, const FIntPoint Size,
	const FVector2f Limits)
{
	using namespace AIOverlayPrivate;

	// How many pixels lie at each fixed-range brightness (in alpha; 0 = not wholly on the pieces), every other one.
	uint32 Histogram[256] = {};
	uint32 Count = 0;
	for (int32 Y = 0; Y < Size.Y; Y += 2)
	{
		const uint8* Row = Rows + SIZE_T(Y) * RowPitchInPixels * 4;
		for (int32 X = 0; X < Size.X; X += 2)
		{
			const uint8 Value = Row[X * 4 + 3];
			Histogram[Value] += Value != 0;
			Count += Value != 0;
		}
	}

	// Too little of the pieces on screen to go by: the range stays.
	if (Count < 32)
	{
		return;
	}

	// The nearest and the furthest pieces, leaving out the nearest and the furthest 2 % (a beam that crosses the camera).
	const uint32 Skip = Count / 50;
	int32 NearValue = 255;
	for (uint32 Seen = Histogram[NearValue]; Seen <= Skip && NearValue > 1; Seen += Histogram[--NearValue])
	{
	}
	int32 FarValue = 1;
	for (uint32 Seen = Histogram[FarValue]; Seen <= Skip && FarValue < 255; Seen += Histogram[++FarValue])
	{
	}

	float Near = BrightnessToDistance(NearValue / 255.0f, Limits);
	float Far = BrightnessToDistance(FarValue / 255.0f, Limits);
	if (Far < Near * MinDepthRatio)
	{
		const float Middle = FMath::Sqrt(Near * Far);
		Near = Middle / FMath::Sqrt(MinDepthRatio);
		Far = Middle * FMath::Sqrt(MinDepthRatio);
	}

	// Eased towards, in ratios, so the map does not pulse as pieces come into view and go: a picture comes back about
	// every frame, so this follows the camera over a fraction of a second.
	constexpr float Rate = 0.15f;
	if (!bDepthRangeFitted)
	{
		FittedDepthRange = FVector2f(Near, Far);
		bDepthRangeFitted = true;
	}
	else
	{
		FittedDepthRange.X = FMath::Exp(FMath::Lerp(FMath::Loge(FittedDepthRange.X), FMath::Loge(Near), Rate));
		FittedDepthRange.Y = FMath::Exp(FMath::Lerp(FMath::Loge(FittedDepthRange.Y), FMath::Loge(Far), Rate));
	}
}

FVector2f FAIOverlayViewExtension::GetDepthRange_RenderThread() const
{
	using namespace AIOverlayPrivate;

	const FVector2f Limits = GetDepthLimits();
	if (!bDepthRangeFitted || CVarDepthFit.GetValueOnRenderThread() == 0)
	{
		return Limits;
	}
	const float Near = FMath::Clamp(FittedDepthRange.X, Limits.X, Limits.Y / MinDepthRatio);
	return FVector2f(Near, FMath::Clamp(FittedDepthRange.Y, Near * MinDepthRatio, Limits.Y));
}

bool FAIOverlayViewExtension::ReceivePicture_RenderThread(uint32& OutWidth, uint32& OutHeight, uint32& OutFrameId)
{
	using namespace AIOverlay;
	using namespace AIOverlayPrivate;

	const uint8* Base = (const uint8*)OutputRegion->GetAddress();
	const FSharedHeader& Header = *(const FSharedHeader*)Base;

	const uint32 Before = ReadSequence(Header);
	if ((Before & 1) != 0 || Before == LastOutputSequence || Header.Magic != SharedMagic)
	{
		return false;
	}

	const uint32 Width = Header.Width;
	const uint32 Height = Header.Height;
	const uint32 FrameId = Header.FrameId;
	if (FrameId == 0 || Width == 0 || Height == 0 || Width > MaxSide || Height > MaxSide)
	{
		return false;
	}

	NewPixels.SetNumUninitialized(Width * Height * 4);
	FMemory::Memcpy(NewPixels.GetData(), Base + PixelOffset, NewPixels.Num());

	// Written over while it was copied: the next frame tries again.
	if (ReadSequence(Header) != Before)
	{
		return false;
	}
	LastOutputSequence = Before;

	// Made from a picture of an earlier cycle.
	if (FrameId < CycleFirstFrameId)
	{
		return false;
	}

	OutWidth = Width;
	OutHeight = Height;
	OutFrameId = FrameId;
	return true;
}

void FAIOverlayViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	using namespace AIOverlayPrivate;

	// What the AI is sent is shown after the last pass there is, so the sky glitch and the phone camera, which work on
	// what this leaves after tonemapping, do not paint over it. That pass is usually off, but its followers still run.
	if (Pass == EPostProcessingPass::VisualizeDepthOfField)
	{
		if (IsDrawnFor(View) && CVarAIOverlay.GetValueOnAnyThread() != 0 && CVarShow.GetValueOnRenderThread() != ShowAI)
		{
			InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FAIOverlayViewExtension::DrawShow_RenderThread));
		}
		return;
	}

	// After tonemapping, so the AI sees and paints the colours on screen.
	if (Pass != EPostProcessingPass::Tonemap || !bIsPassEnabled || !IsDrawnFor(View) || CVarAIOverlay.GetValueOnAnyThread() == 0)
	{
		return;
	}

	// One cycle for every view: the first view drawn each frame moves it on. It runs on the wall clock rather than
	// the view's: a play session's views count from 0 each session, editor viewports from the editor's start.
	const uint64 Frame = GFrameCounterRenderThread;
	const double Now = FPlatformTime::Seconds();
	const int32 Views = View.Family->Views.Num();
	if (LastCycleFrame != Frame)
	{
		LastCycleFrame = Frame;
		if (LastDrawSeconds >= 0.0 && Now - LastDrawSeconds > RestartAfterSeconds)
		{
			Cycle.Restart();
		}
		LastDrawSeconds = Now;

		if (StartRequests.exchange(0) > 0)
		{
			Cycle.StartNow();
		}

		const uint32 CyclesBefore = Cycle.GetCycleCount();
		Intensity = Cycle.Advance(Now, ReadTiming(Views));
		TurnView = AIOverlay::PickTurn(Cycle.GetCycleCount(), Views, CVarPlayer.GetValueOnRenderThread());
		if (Cycle.GetCycleCount() != CyclesBefore)
		{
			UE_LOG(LogAIOverlay, Log, TEXT("AI overlay cycle %u starts%s."), Cycle.GetCycleCount(),
				Views > 1 ? *FString::Printf(TEXT(" on player %d's screen"), TurnView + 1) : TEXT(""));
		}

		WriteWishes_RenderThread();
	}

	// With more than one player, only the screen whose turn it is trades pictures with the bridge and takes the AI's.
	const bool bTurn = View.Family->Views.Find(&View) == TurnView;

	// Between cycles, and without the bridge, nothing is drawn or sent, unless what the AI is sent is to be shown.
	bExchanging = bTurn && Intensity > 0.0f && IsBridgeAlive_RenderThread(Now);
	if (bTurn)
	{
		// For HoldFrameRate_GameThread: the AI's pictures a second, while the game is to be held to them. The bridge's own
		// count when it gives one: counted here, two pictures that come in the same frame are one, which would hold the
		// game lower than the AI needs, and lower still as the structure grows.
		const bool bMatch = bExchanging && CVarMatchFrameRate.GetValueOnRenderThread() != 0 && PictureIntervalSeconds > 0.0
			&& Now - Latest.ArrivalSeconds < RateTimeoutSeconds;
		const float Rate = BridgeRate > 0.0f ? BridgeRate : (float)(1.0 / FMath::Max(PictureIntervalSeconds, 1.0e-3));
		MatchRate.store(bMatch ? Rate : 0.0f);
		MatchRateSeconds.store(Now);
	}
	if (bTurn && (bExchanging || CVarShow.GetValueOnRenderThread() != ShowAI))
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FAIOverlayViewExtension::DrawAfterTonemap_RenderThread));
	}
}

FScreenPassTexture FAIOverlayViewExtension::DrawAfterTonemap_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	using namespace AIOverlayPrivate;

	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	const FSceneTextureUniformParameters* SceneTextures =
		Inputs.SceneTextures.SceneTextures ? Inputs.SceneTextures.SceneTextures->GetContents() : nullptr;
	if (!SceneColor.IsValid() || !SceneTextures || !SceneTextures->CustomStencilTexture)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	// Custom stencil is at render resolution, in the view's own rect, like depth.
	const FIntRect StencilRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(View);
	const FIntPoint SceneColorExtent = SceneColor.Texture->Desc.Extent;

	const auto MakeParameters = [&](const FIntPoint OutputMin, const FIntPoint OutputSize)
	{
		FAIOverlayPS::FParameters* Parameters = GraphBuilder.AllocParameters<FAIOverlayPS::FParameters>();
		Parameters->SceneColorTexture = SceneColor.Texture;
		Parameters->SceneDepthTexture = SceneTextures->SceneDepthTexture ? SceneTextures->SceneDepthTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
		Parameters->CustomStencilTexture = SceneTextures->CustomStencilTexture;
		Parameters->AITexture = GSystemTextures.GetBlackDummy(GraphBuilder);
		Parameters->AIPrevTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
		Parameters->SentDepthTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
		Parameters->PrevSentDepthTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
		Parameters->AIPixels = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(uint32)));
		Parameters->BilinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Parameters->PointSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Parameters->OutputMin = OutputMin;
		Parameters->OutputSize = FVector2f(OutputSize.X, OutputSize.Y);
		Parameters->SceneColorMin = SceneColor.ViewRect.Min;
		Parameters->SceneColorSize = FVector2f(SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height());
		Parameters->SceneColorInvExtent = FVector2f(1.0f / SceneColorExtent.X, 1.0f / SceneColorExtent.Y);
		Parameters->StencilMin = StencilRect.Min;
		Parameters->StencilMax = StencilRect.Max;
		Parameters->StencilSize = FVector2f(StencilRect.Width(), StencilRect.Height());
		Parameters->StencilValue = (uint32)FMath::Clamp(CVarStencil.GetValueOnRenderThread(), 0, 255);
		Parameters->InvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;
		const FVector2f Limits = GetDepthLimits();
		const FVector2f Range = GetDepthRange_RenderThread();
		Parameters->DepthNear = Limits.X;
		Parameters->DepthFar = Limits.Y;
		Parameters->DepthRangeNear = Range.X;
		Parameters->DepthRangeFar = Range.Y;
		Parameters->AIUVScaleBias = FVector4f(1.0f, 1.0f, 0.0f, 0.0f);
		Parameters->ClipToTranslatedWorld = FMatrix44f::Identity;
		Parameters->TranslatedWorldToSentClip = FMatrix44f::Identity;
		Parameters->TranslatedWorldToPrevSentClip = FMatrix44f::Identity;
		Parameters->ReprojectFlags = FVector4f::Zero();
		Parameters->Blend = 1.0f;
		Parameters->DepthTolerance = SeenDepthTolerance;
		return Parameters;
	};

	const auto Stage = [ShaderMap](const int32 Value)
	{
		FAIOverlayPS::FPermutationDomain Permutation;
		Permutation.Set<FAIOverlayPS::FStage>(Value);
		return TShaderMapRef<FAIOverlayPS>(ShaderMap, Permutation);
	};

	FRDGTextureRef AIThisFrame = nullptr;
	// What the AI is sent, when this view made it this frame: the picture, and below it the depth map if there is one.
	FRDGTextureRef Packed = nullptr;
	const int32 Show = CVarShow.GetValueOnRenderThread();

	// One view a frame trades pictures with the bridge.
	const uint64 Frame = GFrameCounterRenderThread;
	if (LastExchangeFrame != Frame)
	{
		LastExchangeFrame = Frame;

		// The depth map also comes back while it is shown without the bridge, to fit its range to the view.
		FPendingReadback* Free = nullptr;
		if (bExchanging || Show == ShowDepth)
		{
			// Pictures the GPU copied on earlier frames go to the bridge first.
			SendReadyPictures_RenderThread(Intensity, bExchanging);

			Free = Readbacks.FindByPredicate([](const FPendingReadback& Pending) { return !Pending.bInFlight; });
			if (!Free && Readbacks.Num() < ReadbackCount)
			{
				// Its readback is made below, for the size of the picture it first takes.
				Free = &Readbacks.AddDefaulted_GetRef();
			}
		}

		if (Free || Show != ShowAI)
		{
			// The depth map goes under the picture, in a texture twice as tall, so one copy brings both back.
			const bool bDepth = Show == ShowDepth
				|| (bExchanging && (BridgeControls & AIOverlay::ControlDepth) != 0 && CVarDepth.GetValueOnRenderThread() != 0);
			Packed = GraphBuilder.CreateTexture(
				FRDGTextureDesc::Create2D(FIntPoint(BridgeSize.X, BridgeSize.Y * (bDepth ? 2 : 1)), PF_R8G8B8A8, FClearValueBinding::Black,
					TexCreate_ShaderResource | TexCreate_RenderTargetable),
				TEXT("AIOverlay.Packed"));

			// The pieces as they are on screen, everything else black, with the pieces' outline in alpha.
			FAIOverlayPS::FParameters* Parameters = MakeParameters(FIntPoint::ZeroValue, BridgeSize);
			Parameters->RenderTargets[0] = FRenderTargetBinding(Packed, ERenderTargetLoadAction::ENoAction);
			FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("AIOverlay.Pack"), Stage(FAIOverlayPS::StagePack),
				Parameters, FIntRect(FIntPoint::ZeroValue, BridgeSize));

			if (bDepth)
			{
				const FIntPoint DepthMin(0, BridgeSize.Y);
				Parameters = MakeParameters(DepthMin, BridgeSize);
				Parameters->RenderTargets[0] = FRenderTargetBinding(Packed, ERenderTargetLoadAction::ELoad);
				FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("AIOverlay.PackDepth"),
					Stage(FAIOverlayPS::StagePackDepth), Parameters, FIntRect(DepthMin, DepthMin + BridgeSize));
			}

			if (Free)
			{
				// A readback keeps the size of the first texture it copies (FRHIGPUTextureReadback::EnqueueCopy), and copying
				// a bigger one into it fails on the GPU and stops the game: a picture of another size (the bridge works at
				// another size now, or the depth map came or went) gets a readback of its own.
				if (!Free->Readback || Free->TextureSize != Packed->Desc.Extent)
				{
					Free->Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("AIOverlay.Readback"));
					Free->TextureSize = Packed->Desc.Extent;
				}
				AddEnqueueCopyPass(GraphBuilder, Free->Readback.Get(), Packed);
				Free->Size = BridgeSize;
				Free->FrameId = ++SentCount;
				Free->bInFlight = true;
				Free->bHasDepth = bDepth;
				Free->DepthLimits = GetDepthLimits();

				// A new cycle starts clean: nothing the AI made in an earlier one is shown.
				if (CycleOfFirstFrame != Cycle.GetCycleCount())
				{
					CycleOfFirstFrame = Cycle.GetCycleCount();
					CycleFirstFrameId = Free->FrameId;
					Latest = FShownPicture();
					Previous = FShownPicture();
					PictureIntervalSeconds = 0.0;
				}

				if (CVarReproject.GetValueOnRenderThread() != 0)
				{
					// Where the camera was and how far the pieces lay, for laying the AI's answer where they are by then.
					FRDGTextureRef SentDepth = GraphBuilder.CreateTexture(
						FRDGTextureDesc::Create2D(BridgeSize, PF_G16R16F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_RenderTargetable),
						TEXT("AIOverlay.SentDepth"));
					Parameters = MakeParameters(FIntPoint::ZeroValue, BridgeSize);
					Parameters->RenderTargets[0] = FRenderTargetBinding(SentDepth, ERenderTargetLoadAction::ENoAction);
					FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("AIOverlay.PackSentDepth"),
						Stage(FAIOverlayPS::StagePackSentDepth), Parameters, FIntRect(FIntPoint::ZeroValue, BridgeSize));

					FSentView& Sent = SentViews[Free->FrameId % SentViewCount];
					Sent.FrameId = Free->FrameId;
					Sent.TranslatedWorldToClip = AIOverlay::GetTranslatedWorldToClipNoAA(View.ViewMatrices);
					Sent.PreViewTranslation = View.ViewMatrices.GetPreViewTranslation();
					GraphBuilder.QueueTextureExtraction(SentDepth, &Sent.Depth);
				}
			}
		}

		uint32 Width = 0;
		uint32 Height = 0;
		uint32 FrameId = 0;
		if (bExchanging && ReceivePicture_RenderThread(Width, Height, FrameId))
		{
			FRDGBufferRef Upload = CreateStructuredBuffer(GraphBuilder, TEXT("AIOverlay.Upload"), sizeof(uint32), Width * Height,
				NewPixels.GetData(), NewPixels.Num());

			const FIntPoint Size(Width, Height);
			AIThisFrame = GraphBuilder.CreateTexture(
				FRDGTextureDesc::Create2D(Size, PF_R8G8B8A8, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_RenderTargetable),
				TEXT("AIOverlay.AIPicture"));

			FAIOverlayPS::FParameters* Parameters = MakeParameters(FIntPoint::ZeroValue, Size);
			Parameters->AIPixels = GraphBuilder.CreateSRV(Upload);
			Parameters->AIWidth = Width;
			Parameters->RenderTargets[0] = FRenderTargetBinding(AIThisFrame, ERenderTargetLoadAction::ENoAction);
			FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("AIOverlay.Unpack"), Stage(FAIOverlayPS::StageUnpack),
				Parameters, FIntRect(FIntPoint::ZeroValue, Size));

			// The newest becomes the one before, and the new one takes the view it was sent from, while that is still kept.
			const double Now = FPlatformTime::Seconds();
			if (Latest.FrameId != 0)
			{
				const double Interval = FMath::Min(Now - Latest.ArrivalSeconds, 1.0);
				PictureIntervalSeconds = PictureIntervalSeconds > 0.0 ? FMath::Lerp(PictureIntervalSeconds, Interval, 0.2) : Interval;
			}
			Previous = MoveTemp(Latest);
			Latest = FShownPicture();
			Latest.FrameId = FrameId;
			Latest.ArrivalSeconds = Now;
			if (const FSentView& Sent = SentViews[FrameId % SentViewCount]; Sent.FrameId == FrameId)
			{
				Latest.View = Sent;
			}
			GraphBuilder.QueueTextureExtraction(AIThisFrame, &Latest.Texture);
			++ReportPictures;
		}

		// How the AI keeps up with the game, every few seconds while they trade pictures.
		if (bExchanging)
		{
			// A pause in the trading (the bridge was gone, or a gap between cycles) starts a fresh count.
			const double Now = FPlatformTime::Seconds();
			if (ReportStartSeconds < 0.0 || Now - ReportLastSeconds > 0.5)
			{
				ReportStartSeconds = Now;
				ReportFrames = 0;
				ReportPictures = 0;
			}
			ReportLastSeconds = Now;
			++ReportFrames;
			if (Now - ReportStartSeconds >= ReportSeconds)
			{
				const double Seconds = Now - ReportStartSeconds;
				UE_LOG(LogAIOverlay, Log, TEXT("AI overlay: %.1f frames a second, %.1f new pictures a second from the AI (%.0f%% of frames)."),
					ReportFrames / Seconds, ReportPictures / Seconds, ReportFrames > 0 ? 100.0 * ReportPictures / ReportFrames : 0.0);
				ReportStartSeconds = Now;
				ReportFrames = 0;
				ReportPictures = 0;
			}
		}
	}

	if (Show != ShowAI)
	{
		// Drawn at the end instead (DrawShow_RenderThread), and the AI's picture is not laid over meanwhile.
		if (Packed)
		{
			ShowPicture = Packed;
			ShowPictureFrame = Frame;
			ShowPictureBuilder = &GraphBuilder;
		}
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	if (!AIThisFrame && Latest.Texture)
	{
		AIThisFrame = GraphBuilder.RegisterExternalTexture(Latest.Texture);
	}
	if (!AIThisFrame)
	{
		// The AI has not answered yet this cycle.
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("AIOverlay.Output"));
	}

	FAIOverlayPS::FParameters* Parameters = MakeParameters(Output.ViewRect.Min, Output.ViewRect.Size());
	Parameters->AITexture = AIThisFrame;
	Parameters->Weight = FMath::Clamp(CVarOpacity.GetValueOnRenderThread(), 0.0f, 1.0f) * Intensity;
	Parameters->ClipToMask = CVarClipToMask.GetValueOnRenderThread() != 0 ? 1.0f : 0.0f;
	Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	// Each picture from where its view saw the pieces now on screen, when that view is still known.
	const bool bPrevious = Previous.Texture.IsValid();
	const bool bReproject = CVarReproject.GetValueOnRenderThread() != 0;
	const bool bReprojectLatest = bReproject && Latest.View.Depth.IsValid();
	const bool bReprojectPrevious = bReproject && bPrevious && Previous.View.Depth.IsValid();
	if (bPrevious)
	{
		Parameters->AIPrevTexture = GraphBuilder.RegisterExternalTexture(Previous.Texture);
	}
	if (bReprojectLatest || bReprojectPrevious)
	{
		const FViewMatrices& Matrices = View.ViewMatrices;
		Parameters->ClipToTranslatedWorld = FMatrix44f(Matrices.ComputeInvProjectionNoAAMatrix() * Matrices.GetInvTranslatedViewMatrix());
		const FVector PreViewTranslation = Matrices.GetPreViewTranslation();
		if (bReprojectLatest)
		{
			Parameters->TranslatedWorldToSentClip = FMatrix44f(AIOverlay::MakeTranslatedWorldToEarlierClip(PreViewTranslation,
				Latest.View.PreViewTranslation, Latest.View.TranslatedWorldToClip));
			Parameters->SentDepthTexture = GraphBuilder.RegisterExternalTexture(Latest.View.Depth);
		}
		if (bReprojectPrevious)
		{
			Parameters->TranslatedWorldToPrevSentClip = FMatrix44f(AIOverlay::MakeTranslatedWorldToEarlierClip(PreViewTranslation,
				Previous.View.PreViewTranslation, Previous.View.TranslatedWorldToClip));
			Parameters->PrevSentDepthTexture = GraphBuilder.RegisterExternalTexture(Previous.View.Depth);
		}
	}
	Parameters->ReprojectFlags = FVector4f(bReprojectLatest ? 1.0f : 0.0f, bReprojectPrevious ? 1.0f : 0.0f, bPrevious ? 1.0f : 0.0f, 0.0f);

	// The newest fades in over the one before, a little faster than the next one usually comes, so it is mostly in by then.
	const double Fade = CVarBlend.GetValueOnRenderThread() != 0 ? FMath::Min(PictureIntervalSeconds * 0.75, MaxFadeSeconds) : 0.0;
	Parameters->Blend = Fade > 0.0 ? (float)FMath::Clamp((FPlatformTime::Seconds() - Latest.ArrivalSeconds) / Fade, 0.0, 1.0) : 1.0f;
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("AIOverlay.Composite"), Stage(FAIOverlayPS::StageComposite),
		Parameters, Output.ViewRect);

	return MoveTemp(Output);
}

FScreenPassTexture FAIOverlayViewExtension::DrawShow_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	using namespace AIOverlayPrivate;

	// Only a picture packed this frame and in this graph is still there to read, and it is shown once.
	FRDGTextureRef Picture = ShowPictureFrame == GFrameCounterRenderThread && ShowPictureBuilder == &GraphBuilder ? ShowPicture : nullptr;
	ShowPicture = nullptr;

	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	if (!Picture || !SceneColor.IsValid())
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("AIOverlay.Show"));
	}

	// The picture is the upper half of the texture when the depth map is under it.
	const bool bHasDepth = Picture->Desc.Extent.Y > BridgeSize.Y;
	const bool bShowDepth = CVarShow.GetValueOnRenderThread() == ShowDepth && bHasDepth;
	const FIntPoint SceneColorExtent = SceneColor.Texture->Desc.Extent;

	FAIOverlayPS::FParameters* Parameters = GraphBuilder.AllocParameters<FAIOverlayPS::FParameters>();
	Parameters->SceneColorTexture = SceneColor.Texture;
	Parameters->AITexture = Picture;
	Parameters->BilinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->OutputMin = Output.ViewRect.Min;
	Parameters->OutputSize = FVector2f(Output.ViewRect.Width(), Output.ViewRect.Height());
	Parameters->SceneColorMin = SceneColor.ViewRect.Min;
	Parameters->SceneColorSize = FVector2f(SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height());
	Parameters->SceneColorInvExtent = FVector2f(1.0f / SceneColorExtent.X, 1.0f / SceneColorExtent.Y);
	Parameters->AIUVScaleBias = FVector4f(1.0f, bHasDepth ? 0.5f : 1.0f, 0.0f, bShowDepth ? 0.5f : 0.0f);
	Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	FAIOverlayPS::FPermutationDomain Permutation;
	Permutation.Set<FAIOverlayPS::FStage>(FAIOverlayPS::StageShow);
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("AIOverlay.Show"),
		TShaderMapRef<FAIOverlayPS>(ShaderMap, Permutation), Parameters, Output.ViewRect);

	return MoveTemp(Output);
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformMemory.h"
#include "Math/RandomStream.h"
#include "RendererInterface.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"

#include <atomic>

class FRHIGPUTextureReadback;
struct FViewMatrices;

namespace AIOverlay
{
	/** How long each part of a cycle lasts, in seconds. */
	struct FTiming
	{
		float GapMin = 20.0f;
		float GapMax = 30.0f;
		float RampUp = 8.0f;
		float Hold = 5.0f;
		float RampDown = 8.0f;
		/** Keeps the AI in: no gaps, and the hold lasts for as long as this stays on. */
		bool bConstant = false;
	};

	/**
	 * The overlay's rhythm: a normal picture for a random GapMin..GapMax seconds, then the AI ramps in over RampUp
	 * seconds, stays at full for Hold, and ramps out over RampDown; then the next gap. The ramps ease in and out.
	 *
	 * Constant skips the gap and holds for as long as it stays on: the AI ramps in once and stays. Turned on mid-ramp
	 * out, it turns back up from where it is; turned off, the hold runs its Hold seconds and the AI ramps out as usual.
	 */
	class FCycle
	{
	public:
		explicit FCycle(int32 Seed = 0) : Random(Seed) {}

		/** How far the AI is in at Now, 0 (off) to 1 (full). Now must not go backwards. */
		float Advance(double Now, const FTiming& Timing);

		/** Ends the gap at once, so the next Advance starts a cycle. */
		void StartNow() { bStartNow = true; }

		/** Drops the running cycle or gap, so the next Advance starts a fresh gap from then. */
		void Restart()
		{
			GapEnd = -1.0;
			CycleStart = -1.0;
			bStartNow = false;
		}

		/** Counts the cycles started, so a picture made in an earlier one can be told apart. */
		uint32 GetCycleCount() const { return CycleCount; }

	private:
		FRandomStream Random;
		/** When the current gap ends; below 0 until the first Advance. */
		double GapEnd = -1.0;
		/** When the running cycle began; below 0 during a gap. */
		double CycleStart = -1.0;
		uint32 CycleCount = 0;
		bool bStartNow = false;
	};

	/**
	 * The two blocks of Windows shared memory the AI bridge (Tools/AIOverlay/bridge.py) reads and writes. Both start
	 * with this header, and the picture follows at PixelOffset as RGBA8 rows, Width * 4 bytes each, top row first.
	 *
	 * Each side only ever writes one block: the game writes the input, the bridge the output. A writer makes Sequence
	 * odd before it changes anything and even again once it is done, so a reader that sees the same even Sequence before
	 * and after copying has a whole picture.
	 */
	struct FSharedHeader
	{
		uint32 Magic;
		uint32 Version;
		uint32 Sequence;
		uint32 Width;
		uint32 Height;
		/** Input: the game's count of pictures sent. Output: the input count the picture was made from. */
		uint32 FrameId;
		/** Input: where the cycle is, 0..1. */
		float Intensity;
		/** Input: how far the AI may move from the picture it is given, 0..1. */
		float Strength;
		/** Output: goes up while the bridge runs, so the game can tell it is there. */
		uint32 Heartbeat;
		/** Input: the length of Prompt in bytes; 0 leaves the bridge on its own prompt. */
		uint32 PromptLength;
		/** Input: how strongly the depth map steers the AI (its ControlNet's weight); 0 when none is sent with the picture. */
		float DepthWeight;
		/** Output: the maps besides the picture that the bridge's AI is steered by, as Control* bits. */
		uint32 Controls;
		/** Input: at most this many pictures a second for the bridge to make (wem.AIOverlay.MaxRate); 0 leaves it to the
		 * bridge, which makes one from every picture it can unless it was started with a cap of its own. Written every
		 * frame, also between cycles, outside Sequence. */
		float MaxRate;
		/** Input: the side, in pixels, of the pictures the bridge's AI should work at (wem.AIOverlay.Size); 0 leaves it at
		 * the bridge's own. Written like MaxRate. */
		uint32 Size;
		/** Output: how many pictures a second the bridge makes, over about the last second; 0 before the first or from an
		 * older bridge. More than the game can count from what reaches it: two pictures that come in the same frame count
		 * there as one. */
		float Rate;
		uint32 Reserved;
		/** Input: UTF-8, not terminated. */
		char Prompt[1024];
	};

	/**
	 * Whose screen the cycle numbered CycleCount (the first is 1) goes to, as an index among the Views player views drawn,
	 * left to right. Player 0 lets the players take turns, a cycle each, the first to the first; Player 1 or 2 keeps it on
	 * that one, or on none (INDEX_NONE) when there are fewer views. One view always gets it (0).
	 */
	int32 PickTurn(uint32 CycleCount, int32 Views, int32 Player);

	constexpr uint32 SharedMagic = 0x414D4557; // "WEMA"
	constexpr uint32 SharedVersion = 1;
	constexpr SIZE_T PixelOffset = 2048;
	constexpr uint32 MaxSide = 1024;
	constexpr SIZE_T SharedSize = PixelOffset + SIZE_T(MaxSide) * MaxSide * 4;

	/** FSharedHeader::Controls: the bridge takes a depth map. */
	constexpr uint32 ControlDepth = 1;

	/** Names without the "Global\" prefix, which would need administrator rights (see MapNamedSharedMemoryRegion). */
	inline const TCHAR* InputName = TEXT("{5E1A7C3B-2D44-4F0E-9B6A-41A0C3D2E701}");
	inline const TCHAR* OutputName = TEXT("{5E1A7C3B-2D44-4F0E-9B6A-41A0C3D2E702}");

	/**
	 * A third block, written by the game, for the maps that steer the AI. It holds no header of its own: its picture,
	 * at PixelOffset and the same size as the input's (depth in red, the pieces' outline in alpha), belongs to the
	 * input block's FrameId and is written under that block's Sequence.
	 */
	inline const TCHAR* ControlName = TEXT("{5E1A7C3B-2D44-4F0E-9B6A-41A0C3D2E703}");

	static_assert(sizeof(FSharedHeader) <= PixelOffset, "The header runs into the picture.");

	/**
	 * The name of one of the blocks of shared memory on a channel, as shared_name in bridge.py. Channel 0, the usual one,
	 * keeps the name as it is; a game started with -AIOverlayChannel=N trades pictures only with a bridge started with
	 * --channel N, so a second game (a measurement run) does not take the first one's bridge.
	 */
	FString GetSharedName(const TCHAR* Name, int32 Channel);

	/**
	 * Takes a point from the current view's translated world space to the clip space of a view drawn earlier, to find
	 * where a picture made from that view saw it. Each view's translated world is the world moved by its own
	 * PreViewTranslation. Row vectors, as everywhere in Unreal: Clip = Point * Matrix.
	 */
	FMatrix MakeTranslatedWorldToEarlierClip(const FVector& PreViewTranslation, const FVector& EarlierPreViewTranslation,
		const FMatrix& EarlierTranslatedWorldToClip);

	/** A view's translated world to clip space without the temporal anti-aliasing jitter, which moves by a pixel a frame. */
	FMatrix GetTranslatedWorldToClipNoAA(const FViewMatrices& ViewMatrices);

	/**
	 * With wem.AIOverlay.MatchFrameRate, the frame rate the game is held to while the AI makes Rate pictures a second: the
	 * rate at which 85 % of the frames get a picture of their own. While the AI keeps up with more of them, the hold rises;
	 * the game takes more of the GPU, the AI less, until the two settle where the GPU is full.
	 */
	float GetMatchedFrameRate(float Rate);
}

/**
 * The AI overlay. Every 20 to 30 seconds a locally run AI (Tools/AIOverlay/bridge.py, StreamDiffusion with
 * SD-Turbo) takes over the platform pieces on screen: it ramps in, holds at full, and ramps out again (see
 * AIOverlay::FCycle).
 *
 * While a cycle runs, each frame the pieces on screen, with everything else black, go to the bridge as a small
 * picture (the bridge picks its size, 512 x 512 by default), and the latest picture the AI made comes back and is
 * laid over the frame. It only ever covers the pieces: ARoomManager draws every piece into custom stencil (value
 * wem.AIOverlay.Stencil), and only pixels with that value take the AI's picture. The AI's picture runs a few frames
 * behind the game, so each frame it is moved from where the camera was when it was sent to where the camera is now,
 * by the pieces' depth (wem.AIOverlay.Reproject), and where its view did not see the pieces now on screen, the picture
 * before it fills in, or else it is cut (wem.AIOverlay.ClipToMask). Each new picture fades in over the one before
 * (wem.AIOverlay.Blend). The bridge makes a picture from every frame it can (wem.AIOverlay.MaxRate caps it), and can be
 * asked to work at another size (wem.AIOverlay.Size); wem.AIOverlay.MatchFrameRate holds the game's frame rate to the
 * AI's. With
 * wem.AIOverlay.SegmentPercent below 100, only that share of the segments, with the beams and posts round them, is drawn
 * with that value, and the rest stay as they are (ARoomManager picks them).
 *
 * The cycle drives two things: how much of the AI's picture is laid over the pieces (up to wem.AIOverlay.Opacity),
 * and how far the AI may move away from the picture it is given (up to wem.AIOverlay.Strength). Between cycles
 * nothing is drawn or sent and the bridge sleeps; the model stays loaded, since loading it takes far longer than a
 * gap.
 *
 * When the bridge's AI takes a depth map (a depth ControlNet), the pieces' depth goes with each picture, so the AI
 * keeps their shape and what is in front of what even when it moves far from their colours (wem.AIOverlay.Depth,
 * .DepthWeight, .DepthNear, .DepthFar). wem.AIOverlay.Show draws what the AI is sent instead of what it makes.
 *
 * It runs after tonemapping and before the sky glitch, so the AI sees the colours on screen, and the sky glitch's
 * trails carry its look. Game and play-in-editor views only. Without the bridge running, nothing changes.
 *
 * With more than one player, a cycle comes to one screen only, so the bridge works no harder than for one player and
 * its pictures, which follow on from each other, never mix two views: the players take turns, a cycle each
 * (wem.AIOverlay.Player, AIOverlay::PickTurn). With Constant on, the players still take turns: the gaps are dropped
 * instead of the hold running on.
 *
 * Console: wem.AIOverlay 0/1, wem.AIOverlay.Start, wem.AIOverlay.GapMin, .GapMax, .RampUp, .Hold, .RampDown,
 * .Constant, .Player, .Opacity, .Strength, .Prompt, .ClipToMask, .SegmentPercent, .Stencil, .Depth, .DepthWeight,
 * .DepthNear, .DepthFar, .Show, .MaxRate, .Reproject, .Blend, .Size, .MatchFrameRate. -AIOverlayChannel=N on the
 * command line picks the bridge.
 */
class FAIOverlayViewExtension : public FSceneViewExtensionBase
{
public:
	FAIOverlayViewExtension(const FAutoRegister& AutoRegister);
	virtual ~FAIOverlayViewExtension() override;

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

	/** Above the sky glitch's 0 and the phone camera's -1: those run on the picture this one leaves. */
	virtual int32 GetPriority() const override { return 1; }

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	/** A picture on its way back from the GPU to go to the bridge. */
	struct FPendingReadback
	{
		TUniquePtr<FRHIGPUTextureReadback> Readback;
		/** The size of the textures Readback copies (the picture, with the depth map under it when there is one). */
		FIntPoint TextureSize = FIntPoint::ZeroValue;
		FIntPoint Size = FIntPoint::ZeroValue;
		uint32 FrameId = 0;
		bool bInFlight = false;
		/** The depth map lies under the picture, in a texture twice as tall. */
		bool bHasDepth = false;
		/** The fixed range of the depth in the depth map's alpha (AIOverlayPrivate::GetDepthLimits), in world units. */
		FVector2f DepthLimits = FVector2f::ZeroVector;
	};

	FScreenPassTexture DrawAfterTonemap_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	/** wem.AIOverlay.Show: draws what the AI is sent over the whole view, after every other effect. */
	FScreenPassTexture DrawShow_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	/** Maps the three blocks of shared memory the first time it is called; false while any cannot be mapped. */
	bool MapShared_RenderThread();

	/** Whether the bridge's heartbeat has moved in the last few seconds. Also reads the size and maps it wants. */
	bool IsBridgeAlive_RenderThread(double Now);

	/**
	 * Takes the newest picture the GPU has finished copying, fits the depth map's range to it, and with bSend hands it to
	 * the bridge.
	 */
	void SendReadyPictures_RenderThread(float Intensity, bool bSend);

	/** Moves the fitted depth range towards the nearest and furthest pieces in a depth map's alpha (see AIOverlay.usf). */
	void FitDepthRange_RenderThread(const uint8* Rows, int32 RowPitchInPixels, FIntPoint Size, FVector2f Limits);

	/** The range, in world units, the depth map runs between now: fitted to the view, or wem.AIOverlay.DepthNear and .DepthFar. */
	FVector2f GetDepthRange_RenderThread() const;

	/** Copies the bridge's newest picture into NewPixels; false if there is none since the last call. */
	bool ReceivePicture_RenderThread(uint32& OutWidth, uint32& OutHeight, uint32& OutFrameId);

	/** Tells the bridge, in the input's header, the cap on its pictures a second and the size to work at. Once a frame. */
	void WriteWishes_RenderThread();

	/**
	 * wem.AIOverlay.MatchFrameRate: holds t.MaxFPS to the AI's pictures a second while they are traded, and puts it back
	 * as it was after. At the start of each game thread frame.
	 */
	void HoldFrameRate_GameThread();

	/** Where the camera was for a picture sent to the bridge, so the AI's answer to it can be laid where the pieces are now. */
	struct FSentView
	{
		uint32 FrameId = 0;
		/** Without the anti-aliasing jitter. */
		FMatrix TranslatedWorldToClip = FMatrix::Identity;
		FVector PreViewTranslation = FVector::ZeroVector;
		/** The nearest and furthest depth of the pieces under each of the picture's pixels, in red and green; 0 where none
		 * were (AIOverlay.usf, PACK SENT DEPTH). */
		TRefCountPtr<IPooledRenderTarget> Depth;
	};

	/** One of the AI's pictures, as it is laid over the pieces. */
	struct FShownPicture
	{
		TRefCountPtr<IPooledRenderTarget> Texture;
		uint32 FrameId = 0;
		/** When it came back, in FPlatformTime seconds. */
		double ArrivalSeconds = 0.0;
		/** The view it was made from; without a depth when that was no longer kept (an answer to a picture long sent). */
		FSentView View;
	};

	AIOverlay::FCycle Cycle;
	/** The cycle's value for this frame, worked out by the first view that draws it. */
	float Intensity = 0.0f;
	/** Whose screen the cycle is on, as an index among the frame's views (AIOverlay::PickTurn). */
	int32 TurnView = 0;
	uint64 LastCycleFrame = MAX_uint64;
	/** When a game view last drew, in FPlatformTime seconds; a long pause (a new play session) restarts the gap. */
	double LastDrawSeconds = -1.0;
	/** The frame whose view sent a picture and fetched the AI's; other views that frame only lay it over. */
	uint64 LastExchangeFrame = MAX_uint64;
	/** This frame trades pictures with the bridge: a cycle runs and the bridge is there. */
	bool bExchanging = false;
	/** For DrawShow_RenderThread: what the AI is sent, as packed this frame, in this graph. */
	FRDGTextureRef ShowPicture = nullptr;
	uint64 ShowPictureFrame = MAX_uint64;
	const FRDGBuilder* ShowPictureBuilder = nullptr;

	FPlatformMemory::FSharedMemoryRegion* InputRegion = nullptr;
	FPlatformMemory::FSharedMemoryRegion* OutputRegion = nullptr;
	FPlatformMemory::FSharedMemoryRegion* ControlRegion = nullptr;
	bool bMapFailed = false;

	uint32 LastHeartbeat = 0;
	double LastHeartbeatChangeSeconds = -1.0e9;
	bool bBridgeWasAlive = false;
	FIntPoint BridgeSize = FIntPoint(512, 512);
	/** The maps the bridge's AI is steered by, as AIOverlay::Control* bits. */
	uint32 BridgeControls = 0;
	/** The bridge's own count of its pictures a second (FSharedHeader::Rate); 0 when it does not say. */
	float BridgeRate = 0.0f;

	/** The depth range fitted to the view (wem.AIOverlay.DepthFit), in world units; none until the first depth map is back. */
	FVector2f FittedDepthRange = FVector2f::ZeroVector;
	bool bDepthRangeFitted = false;

	uint32 SentCount = 0;
	/** The first picture sent in the running cycle; the AI's pictures made from earlier ones are not shown. */
	uint32 CycleFirstFrameId = 0;
	uint32 CycleOfFirstFrame = MAX_uint32;
	uint32 LastOutputSequence = 0;
	TArray<FPendingReadback> Readbacks;

	TArray<uint8> NewPixels;
	/** The AI's newest picture, as it came back, at the bridge's size, and the one before, which it fades in over. */
	FShownPicture Latest;
	FShownPicture Previous;
	/** About how long the AI takes from one picture to the next, in seconds; 0 until two have come this cycle. */
	double PictureIntervalSeconds = 0.0;
	/** The views of the last pictures sent, by FrameId % AIOverlayPrivate::SentViewCount. */
	TArray<FSentView> SentViews;

	/** For the log's report on how the AI keeps up: frames drawn and pictures taken since ReportStartSeconds. */
	static constexpr double ReportSeconds = 5.0;
	double ReportStartSeconds = -1.0;
	double ReportLastSeconds = -1.0;
	uint32 ReportFrames = 0;
	uint32 ReportPictures = 0;

	/** -AIOverlayChannel=N on the command line: which bridge this game trades pictures with (AIOverlay::GetSharedName). */
	int32 Channel = 0;

	/** From the render thread to HoldFrameRate_GameThread: the AI's pictures a second while the game is to be held to them,
	 * else 0, and when that was last said. */
	std::atomic<float> MatchRate{0.0f};
	std::atomic<double> MatchRateSeconds{-1.0e9};
	/** Game thread: the frame rate t.MaxFPS is held to, or below 0 when it is not held; and what it was before. */
	float HeldFrameRate = -1.0f;
	float FrameRateBeforeHold = 0.0f;
	FDelegateHandle BeginFrameHandle;
};

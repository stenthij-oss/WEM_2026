// Copyright Epic Games, Inc. All Rights Reserved.

#include "StartScreen.h"

#include "Components/AudioComponent.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EngineUtils.h"
#include "Fonts/CompositeFont.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/Paths.h"
#include "RoomManager.h"
#include "SkyGlitchTitle.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "StartPrompt.h"
#include "Styling/CoreStyle.h"

namespace StartScreenPrivate
{
	/** Over the orb HUD (10) and the emoji (20), so nothing covers the sign. */
	constexpr int32 PromptZOrder = 30;

	/** How far short of its end the music may stop and still count as played out: the last report comes a little early. */
	constexpr double EndTolerance = 1.0;
}

float WEMStartScreen::GetTitleOpacity(const double Seconds, const float TitleSeconds)
{
	if (TitleSeconds <= 0.0f)
	{
		return Seconds >= 0.0 ? 1.0f : 0.0f;
	}

	return FMath::Clamp(static_cast<float>(Seconds / TitleSeconds), 0.0f, 1.0f);
}

bool WEMStartScreen::IsPromptShown(const double Seconds, const float OnSeconds, const float OffSeconds)
{
	const double Cycle = FMath::Max(OnSeconds, 0.0f) + FMath::Max(OffSeconds, 0.0f);

	if (Cycle <= 0.0 || Seconds < 0.0)
	{
		return true;
	}

	return FMath::Fmod(Seconds, Cycle) < OnSeconds;
}

AStartScreen::AStartScreen()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// Before the room manager's beats and the orbs, so growth starts on the frame the title is lit.
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	StartAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Drone/IA_Drone_Interact.IA_Drone_Interact")));

	// The drone's pad, as IM_HoverDrone, IMC_Drone and the emoji menu (UEmojiComponent) map it.
	Controls = {
		{TEXT("left stick"), TEXT("fly")},
		{TEXT("right stick"), TEXT("look")},
		{TEXT("LB / RB"), TEXT("turn left / right")},
		{TEXT("RT / LT"), TEXT("up / down")},
		{TEXT("d-pad up / down"), TEXT("faster / slower")},
		{TEXT("A"), TEXT("catch an orb")},
		{TEXT("hold B"), TEXT("zoom")},
		{TEXT("X"), TEXT("flashlight")},
		{TEXT("hold right stick in"), TEXT("send an emoji")},
	};

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

#if WITH_EDITORONLY_DATA
	// Loaded with the level whatever the camera's place, so it is there to hold the room managers before they begin play.
	bIsSpatiallyLoaded = false;
#endif
}

void AStartScreen::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Every actor in the level is set up before any of them begins play, so the room managers are held here, before
	// their BeginPlay would start them growing.
	UWorld* World = GetWorld();

	if (!bShowStartScreen || !World || !World->IsGameWorld())
	{
		return;
	}

	for (ARoomManager* Manager : GetRoomManagers())
	{
		Manager->HoldGrowth();
	}
}

void AStartScreen::BeginPlay()
{
	Super::BeginPlay();

	if (!bShowStartScreen)
	{
		return;
	}

	Stage = EStartScreenStage::Waiting;
	MusicSeconds = 0.0f;

	ShowPrompt();
	HoldDrones(true);
	ShowDrones(false);
	BindStart();

	SetActorTickEnabled(true);
}

void AStartScreen::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindStart();
	HidePrompt();

	if (MusicComponent)
	{
		// Stopped on purpose: not the end of the music, so nothing quits.
		MusicComponent->OnAudioPlaybackPercentNative.RemoveAll(this);
		MusicComponent->OnAudioFinishedNative.RemoveAll(this);
		MusicComponent->Stop();
		MusicComponent = nullptr;
	}

	if (bShowStartScreen)
	{
		SkyGlitchTitle::Reset();
	}

	Super::EndPlay(EndPlayReason);
}

bool AStartScreen::HoldsGrowth(const ARoomManager* Manager) const
{
	const UWorld* World = GetWorld();

	return bShowStartScreen && Stage != EStartScreenStage::Playing && World && World->IsGameWorld()
		&& (!RoomManager || RoomManager == Manager);
}

TArray<ARoomManager*> AStartScreen::GetRoomManagers() const
{
	TArray<ARoomManager*> Managers;

	if (RoomManager)
	{
		Managers.Add(RoomManager);
	}
	else if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ARoomManager> It(World); It; ++It)
		{
			Managers.Add(*It);
		}
	}

	return Managers;
}

void AStartScreen::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	switch (Stage)
	{
	case EStartScreenStage::Waiting:
		// Players joining, and drones spawning, after play began.
		BindStart();
		ShowPrompt();
		HoldDrones(true);
		ShowDrones(false);
		break;

	case EStartScreenStage::Title:
	{
		ShowDrones(false);

		// Never back, so the title never dims when a report from the audio comes in a little behind the clock.
		const double Seconds = FMath::Max(GetMusicSeconds(), static_cast<double>(MusicSeconds));
		MusicSeconds = static_cast<float>(Seconds);

		SkyGlitchTitle::Show(TitleTexture, WEMStartScreen::GetTitleOpacity(Seconds, TitleSeconds), TitleColor, TitleWidth, TitleMaxHeight);

		if (Seconds >= TitleSeconds)
		{
			FinishTitle();
		}

		break;
	}

	case EStartScreenStage::Playing:
		MusicSeconds = static_cast<float>(GetMusicSeconds());

		// The frame that burnt the title in has been sent to be drawn by now, so the title shown can go: what stays is
		// the trail's.
		if (bTitleBurning)
		{
			bTitleBurning = false;
			SkyGlitchTitle::Hide();
		}

		break;
	}
}

void AStartScreen::StartRun()
{
	if (Stage != EStartScreenStage::Waiting || !HasActorBegunPlay())
	{
		return;
	}

	Stage = EStartScreenStage::Title;
	StartedAt = FPlatformTime::Seconds();
	HeardAt = -1.0;
	HeardSeconds = 0.0;

	UnbindStart();
	HidePrompt();
	HoldDrones(false);

	if (Music)
	{
		// Made rather than spawned, so the playback reports are bound before it starts: they are only sent to a sound
		// that had someone listening when it began.
		MusicComponent = UGameplayStatics::CreateSound2D(this, Music, 1.0f, 1.0f, 0.0f, nullptr, /*bPersistAcrossLevelTransition=*/false, /*bAutoDestroy=*/false);
	}

	if (MusicComponent)
	{
		MusicComponent->OnAudioPlaybackPercentNative.AddUObject(this, &AStartScreen::OnMusicPlayback);
		MusicComponent->OnAudioFinishedNative.AddUObject(this, &AStartScreen::OnMusicFinished);
		MusicComponent->Play();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("StartScreen: no music to play (none set, or no audio device); the title keeps to the clock and the game does not quit on its own."));
	}

	UE_LOG(LogTemp, Log, TEXT("StartScreen: started; the title lights up over %.2f s."), TitleSeconds);
}

void AStartScreen::FinishTitle()
{
	Stage = EStartScreenStage::Playing;

	// Fully lit as it burns in, whatever opacity the last frame reached.
	SkyGlitchTitle::Show(TitleTexture, 1.0f, TitleColor, TitleWidth, TitleMaxHeight);
	SkyGlitchTitle::BurnIn();
	bTitleBurning = true;

	// Seen from the frame the title is fully lit.
	ShowDrones(true);

	for (ARoomManager* Manager : GetRoomManagers())
	{
		Manager->StartGrowth();
	}

	UE_LOG(LogTemp, Log, TEXT("StartScreen: title lit at %.3f s into the music; burnt into the sky, and growth starts."), GetMusicSeconds());
}

double AStartScreen::GetMusicSeconds() const
{
	const double Now = FPlatformTime::Seconds();

	if (HeardAt >= 0.0)
	{
		return HeardSeconds + (Now - HeardAt);
	}

	// No report yet: the music is only just starting, or there is no audio to report.
	return StartedAt >= 0.0 ? Now - StartedAt : 0.0;
}

void AStartScreen::OnMusicPlayback(const UAudioComponent* Component, const USoundWave* Wave, const float Percent)
{
	if (!Wave)
	{
		return;
	}

	// Reports come once per audio buffer; between them the clock carries on from the last.
	HeardSeconds = static_cast<double>(Percent) * Wave->Duration;
	HeardAt = FPlatformTime::Seconds();
}

void AStartScreen::OnMusicFinished(UAudioComponent* Component)
{
	// Also sent when the music is stopped before its end - as play ends, among others - when quitting would carry over
	// to the next play. Only the music played out quits.
	const double Seconds = GetMusicSeconds();
	const float Duration = Music ? Music->GetDuration() : 0.0f;
	const UWorld* World = GetWorld();

	if (Seconds < Duration - StartScreenPrivate::EndTolerance || !World || World->bIsTearingDown)
	{
		UE_LOG(LogTemp, Log, TEXT("StartScreen: the music was stopped at %.2f s of %.2f s."), Seconds, Duration);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("StartScreen: the music has ended at %.2f s."), Seconds);

	if (bQuitWhenMusicEnds)
	{
		Quit();
	}
}

void AStartScreen::Quit()
{
	UWorld* World = GetWorld();
	UKismetSystemLibrary::QuitGame(this, World ? World->GetFirstPlayerController() : nullptr, EQuitPreference::Quit, /*bIgnorePlatformRestrictions=*/false);
}

void AStartScreen::BindStart()
{
	if (StartInput || bStartActionMissing)
	{
		return;
	}

	// Player 1 is the first local player, whose pad is the first.
	const UGameInstance* GameInstance = GetGameInstance();
	const TArray<ULocalPlayer*>& Locals = GameInstance ? GameInstance->GetLocalPlayers() : TArray<ULocalPlayer*>();
	APlayerController* Controller = Locals.Num() > 0 && Locals[0] ? Locals[0]->GetPlayerController(GetWorld()) : nullptr;

	if (!Controller || !Controller->IsLocalController())
	{
		return;
	}

	UInputAction* Action = StartAction.LoadSynchronous();

	if (!Action)
	{
		bStartActionMissing = true;
		UE_LOG(LogTemp, Warning, TEXT("StartScreen: no start action set, so nothing starts the run."));
		return;
	}

	StartInput = NewObject<UEnhancedInputComponent>(this, NAME_None, RF_Transient);
	StartInput->RegisterComponent();
	StartInput->BindAction(Action, ETriggerEvent::Started, this, &AStartScreen::StartRun);
	Controller->PushInputComponent(StartInput);
	StartInputPlayer = Controller;
}

void AStartScreen::UnbindStart()
{
	if (!StartInput)
	{
		return;
	}

	if (APlayerController* Controller = StartInputPlayer.Get())
	{
		Controller->PopInputComponent(StartInput);
	}

	StartInput->DestroyComponent();
	StartInput = nullptr;
	StartInputPlayer.Reset();
}

void AStartScreen::HoldDrones(const bool bHold)
{
	UWorld* World = GetWorld();

	if (!World)
	{
		return;
	}

	// A held pawn's input is left off its controller's stack, so its drone neither flies nor turns.
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* Controller = It->Get();
		APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;

		if (!Pawn || Pawn->InputEnabled() != bHold)
		{
			continue;
		}

		if (bHold)
		{
			Pawn->DisableInput(Controller);
		}
		else
		{
			Pawn->EnableInput(Controller);
		}
	}
}

void AStartScreen::ShowDrones(const bool bShow)
{
	UWorld* World = GetWorld();

	if (!World)
	{
		return;
	}

	// The whole drone: its body and every piece of its trail are its components. The body's own flag, which shows it
	// only while someone flies it, is left as it is.
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* Controller = It->Get();
		APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;

		if (Pawn && Pawn->IsHidden() == bShow)
		{
			Pawn->SetActorHiddenInGame(!bShow);
		}
	}
}

FStartPromptStyle AStartScreen::MakePromptStyle() const
{
	FStartPromptStyle Style;
	Style.Text = PromptText;
	Style.Color = PromptColor;
	Style.Height = PromptHeight;
	Style.Position = PromptPosition;
	Style.BlinkOnSeconds = BlinkOnSeconds;
	Style.BlinkOffSeconds = BlinkOffSeconds;
	Style.ControlsColor = ControlsColor;
	Style.ControlsHeight = ControlsHeight;
	Style.ControlsPosition = ControlsPosition;
	Style.ControlsLineSpacing = ControlsLineSpacing;

	for (const FStartScreenControl& Control : Controls)
	{
		Style.Controls.Emplace(Control.Control, Control.Action);
	}

	const FString Garamond = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("WINDIR")), TEXT("Fonts"), TEXT("GARA.TTF"));

	if (PromptFont)
	{
		Style.Font = FSlateFontInfo(PromptFont, 24.0f);
	}
	else if (FPaths::FileExists(Garamond))
	{
		Style.Font = FSlateFontInfo(MakeShared<FStandaloneCompositeFont>(TEXT("Garamond"), Garamond, EFontHinting::Default, EFontLoadingPolicy::LazyLoad), 24.0f);
	}
	else
	{
		Style.Font = FCoreStyle::GetDefaultFontStyle("Regular", 24);
	}

	return Style;
}

void AStartScreen::ShowPrompt()
{
	// Only a game view with players in it: Simulate has neither.
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	const UGameInstance* GameInstance = GetGameInstance();

	if (!Viewport || !GameInstance)
	{
		return;
	}

	// Made once a player needs it: this runs every frame while players may still join.
	TOptional<FStartPromptStyle> Style;

	for (ULocalPlayer* Local : GameInstance->GetLocalPlayers())
	{
		if (!Local || !Local->GetPlayerController(World) || PromptPlayers.Contains(Local))
		{
			continue;
		}

		if (!Style)
		{
			Style = MakePromptStyle();
		}

		TSharedRef<SStartPrompt> Prompt = SNew(SStartPrompt);
		Prompt->SetStyle(*Style);
		Viewport->AddViewportWidgetForPlayer(Local, Prompt, StartScreenPrivate::PromptZOrder);
		Prompts.Add(Prompt);
		PromptPlayers.Add(Local);
	}
}

void AStartScreen::HidePrompt()
{
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;

	for (int32 Index = 0; Index < Prompts.Num(); ++Index)
	{
		if (Viewport && Prompts[Index] && PromptPlayers[Index].IsValid())
		{
			Viewport->RemoveViewportWidgetForPlayer(PromptPlayers[Index].Get(), Prompts[Index].ToSharedRef());
		}
	}

	Prompts.Reset();
	PromptPlayers.Reset();
}

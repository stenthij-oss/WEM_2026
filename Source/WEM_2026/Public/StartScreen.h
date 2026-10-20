// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "StartScreen.generated.h"

class APlayerController;
class ARoomManager;
class SStartPrompt;
class UAudioComponent;
class UEnhancedInputComponent;
class UFont;
class UInputAction;
class ULocalPlayer;
class USoundBase;
class USoundWave;
struct FStartPromptStyle;
class UTexture2D;

/** Where a run stands. */
UENUM(BlueprintType)
enum class EStartScreenStage : uint8
{
	/** The sign blinks, the drones are held still and unseen and nothing grows, until player 1 presses A. */
	Waiting,

	/** The music has started and the title is lighting up in the sky. The drones fly, still unseen. */
	Title,

	/** The title is burnt into the sky glitch, the drones are seen and the room manager is growing, until the music ends. */
	Playing
};

/** One line of the controls the start screen explains: what to press, and what it does. */
USTRUCT(BlueprintType)
struct FStartScreenControl
{
	GENERATED_BODY()

	FStartScreenControl() = default;

	FStartScreenControl(const TCHAR* InControl, const TCHAR* InAction)
		: Control(InControl)
		, Action(InAction)
	{
	}

	/** What to press, written in the column on the left. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	FString Control;

	/** What it does, written in the column on the right. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	FString Action;
};

/**
 * The start screen, and the run it starts.
 *
 * Until player 1 presses A, a sign blinks "press A to start" on every player's screen below the controls (Controls),
 * the drones are held still and the room manager does not grow. On A, the music starts, and at once the title lights up in the sky, fully lit TitleSeconds
 * into the music. Then the title goes, burnt into the sky glitch (SkyGlitchTitle.h), where it stays until the
 * platforms' copies paint it over, and the room manager starts up as it would have at the start of play. The drones,
 * and their trails, are unseen until that moment. When the music ends, the game quits. With several players, each
 * screen has its own sign and its own title.
 *
 * The time is the music's own, as the audio reports it playing, so the title and the room manager keep to the
 * song rather than to when A was pressed.
 *
 * Place one in the level. With bShowStartScreen off, or with none in the level, play starts as before.
 */
UCLASS()
class WEM_2026_API AStartScreen : public AActor
{
	GENERATED_BODY()

public:
	AStartScreen();

	/** Whether play waits on this start screen. Off, the room manager grows from the start of play, with no music. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	bool bShowStartScreen = true;

	/** The room manager held back until the title is lit. None holds every one in the level. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	TObjectPtr<ARoomManager> RoomManager;

	/** What player 1 presses to start: the drone's interact action, A on the pad and the space bar. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	TSoftObjectPtr<UInputAction> StartAction;

	/** The sign that blinks until the game starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Sign")
	FString PromptText = TEXT("press A to start");

	/** The typeface of the sign. None uses Garamond from Windows' fonts, as the orb snippets do. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Sign")
	TObjectPtr<UFont> PromptFont;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Sign")
	FLinearColor PromptColor = FLinearColor::White;

	/** A letter's height, as a share of the frame's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Sign", meta = (ClampMin = "0.005", ClampMax = "0.5"))
	float PromptHeight = 0.04f;

	/** How far down the frame the sign's middle sits, as a share of its height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Sign", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PromptPosition = 0.75f;

	/** How long the sign shows on each blink, and how long it is gone between. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Sign", meta = (ClampMin = "0.05"))
	float BlinkOnSeconds = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Sign", meta = (ClampMin = "0.05"))
	float BlinkOffSeconds = 0.4f;

	/**
	 * The controls explained above the sign while it blinks, a line each: what to press on the left, what it does on the
	 * right. They go when the run starts. Empty shows none. In the sign's typeface.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Controls", meta = (TitleProperty = "{Control}: {Action}"))
	TArray<FStartScreenControl> Controls;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Controls")
	FLinearColor ControlsColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.85f);

	/** A letter's height, as a share of the frame's height. Smaller where the lines would not fit across the frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Controls", meta = (ClampMin = "0.005", ClampMax = "0.2"))
	float ControlsHeight = 0.024f;

	/** How far down the frame the controls' middle sits, as a share of its height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Controls", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ControlsPosition = 0.44f;

	/** From one line to the next, as a share of a letter's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Controls", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float ControlsLineSpacing = 1.7f;

	/** What plays from A until the game quits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Music")
	TObjectPtr<USoundBase> Music;

	/** Quits the game the moment the music ends. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Music")
	bool bQuitWhenMusicEnds = true;

	/**
	 * The title: its red channel is how much of each pixel it covers (T_Title_Catastrophe, CATASTROPHE in Garamond,
	 * made by Tools/StartScreen/render_title.py).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Title")
	TObjectPtr<UTexture2D> TitleTexture;

	/** Seconds into the music the title takes to light up fully. Then it is burnt into the sky and the room manager starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Title", meta = (ClampMin = "0.0"))
	float TitleSeconds = 0.79f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Title")
	FLinearColor TitleColor = FLinearColor::White;

	/** How wide the title is drawn, as a share of the view's width - unless that would make it taller than TitleMaxHeight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Title", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float TitleWidth = 0.8f;

	/** The tallest the title is drawn, as a share of the view's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen|Title", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float TitleMaxHeight = 0.3f;

	/** Where the run stands. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Start Screen")
	EStartScreenStage Stage = EStartScreenStage::Waiting;

	/** Seconds into the music. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Start Screen")
	float MusicSeconds = 0.0f;

	/** Starts the run as player 1's A does. Does nothing once it has started. */
	UFUNCTION(BlueprintCallable, Category = "Start Screen")
	void StartRun();

	/**
	 * Whether this holds Manager's growth back: until the title is lit, for RoomManager or, with none set, any. A room
	 * manager asks as it begins play, since one loaded with its part of the world can begin after this has.
	 */
	bool HoldsGrowth(const ARoomManager* Manager) const;

	virtual void PostInitializeComponents() override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** The room managers this holds back: RoomManager, or every one in the level. */
	TArray<ARoomManager*> GetRoomManagers() const;

	/** Binds StartAction for player 1, once there is a player 1 to bind it for. */
	void BindStart();

	/** Holds every player's drone still, or lets them all go. Drones that arrive later are held as they come. */
	void HoldDrones(bool bHold);

	/** Shows every player's drone, its trail with it, or hides them all. Drones that arrive later are hidden as they come. */
	void ShowDrones(bool bShow);

	/** Shows the sign over each player's own frame, as they join. */
	void ShowPrompt();
	FStartPromptStyle MakePromptStyle() const;
	void HidePrompt();

	/** The title has lit up: burnt into the sky, and the room managers start. */
	void FinishTitle();

	void OnMusicPlayback(const UAudioComponent* Component, const USoundWave* Wave, float Percent);
	void OnMusicFinished(UAudioComponent* Component);

	/** Ends the game, as catching every orb does. */
	void Quit();

	/** Seconds into the music now: the last the audio reported, plus the time since; or, before any report, since A. */
	double GetMusicSeconds() const;

	void UnbindStart();

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> MusicComponent;

	/** Player 1's input for StartAction, on their controller's stack until the run starts. */
	UPROPERTY(Transient)
	TObjectPtr<UEnhancedInputComponent> StartInput;

	TWeakObjectPtr<APlayerController> StartInputPlayer;

	/** The sign over each player's frame, and whose frame it is over. */
	TArray<TSharedPtr<SStartPrompt>> Prompts;
	TArray<TWeakObjectPtr<ULocalPlayer>> PromptPlayers;

	bool bStartActionMissing = false;

	/** The title has been burnt in and is still shown, to be taken away on the next tick, once the frame burning it in is drawn. */
	bool bTitleBurning = false;

	/** Real seconds when A was pressed, and when the audio last reported, with how far into the music it was. */
	double StartedAt = -1.0;
	double HeardAt = -1.0;
	double HeardSeconds = 0.0;
};

/** The pure parts of the start screen, for the tests. */
namespace WEMStartScreen
{
	/** How lit the title is, from 0 to 1, Seconds into the music for a title that lights up over TitleSeconds. */
	WEM_2026_API float GetTitleOpacity(double Seconds, float TitleSeconds);

	/** Whether the sign shows, Seconds after it first did, blinking on for OnSeconds and off for OffSeconds. */
	WEM_2026_API bool IsPromptShown(double Seconds, float OnSeconds, float OffSeconds);
}

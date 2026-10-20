// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "InputCoreTypes.h"
#include "Math/RandomStream.h"
#include "EmojiComponent.generated.h"

class APlayerController;
class SEmojiOverlay;
class UInputAction;
class UInputMappingContext;
class UInstancedStaticMeshComponent;
class ULocalPlayer;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;
struct FWEMViewer;

/** One emoji rising over the screen, where it is this frame. Centre is a share of the frame: x of its width, y of its height from the top. Size is a share of its height. */
struct FEmojiScreenSprite
{
	int32 Emoji = 0;
	FVector2f Centre = FVector2f::ZeroVector;
	float Size = 0.0f;

	/** Radians, clockwise on screen. */
	float Angle = 0.0f;
	float Opacity = 1.0f;
};

/** What the screen shows this frame: the menu, as far open as it is, and the emoji rising over it. */
struct FEmojiScreenState
{
	/** 0 shut, 1 open. */
	float MenuOpen = 0.0f;

	/** The emoji the stick points at, or INDEX_NONE. */
	int32 Picked = INDEX_NONE;

	/** How far each emoji has grown towards picked, 0 to 1. */
	TArray<float> Highlight;

	TArray<FEmojiScreenSprite> Sprites;
};

/**
 * An emoji menu on the drone. Hold the menu key - the right stick, pressed in - and a small ring of
 * emoji opens in the middle of the frame. Lean the stick towards one and it grows to show it is
 * picked; turn the stick round to pick another. While the menu is open the stick no longer turns the
 * drone. Let go of the key and the picked emoji is sent:
 *
 * - on the screen of the one who sent it, a stream of the emoji rises from the bottom of the frame,
 *   swaying as it goes, and fades away at the top;
 * - in the world, the drone gives off a short burst of the emoji on little squares that turn to face
 *   whoever looks. They fly out from here, big near the drone and smaller the farther from it they
 *   are, and are gone in a second or two. Everyone sees these.
 *
 * Put it on the drone where the burst should come out. Letting go with nothing picked sends nothing.
 */
UCLASS(ClassGroup = (WEM), meta = (BlueprintSpawnableComponent), hidecategories = (Activation, AssetUserData, Collision, Cooking, Navigation, Physics, Tags))
class WEM_2026_API UEmojiComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UEmojiComponent();

	/** The emoji to choose from, round the menu clockwise from the top. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji")
	TArray<TObjectPtr<UTexture2D>> Emoji;

	/** Held down, keeps the menu open. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Input")
	FKey MenuKey = EKeys::Gamepad_RightThumbstick;

	/** Picks the emoji it leans towards while the menu is open. It stops turning the drone meanwhile. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Input")
	FKey AimKey = EKeys::Gamepad_Right2D;

	/**
	 * Whether the stick pushed up reads as a negative lean. It does on the controllers here, and it is
	 * also what tilts the drone's view up, so up on the stick is up in the ring. Turn it off for a
	 * controller that reads up as positive.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Input")
	bool bStickUpIsNegative = true;

	/** How far the stick has to lean, from 0 to 1, before it picks. Back inside this it keeps the last pick. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Input", meta = (ClampMin = "0.05", ClampMax = "0.95"))
	float AimDeadZone = 0.5f;

	/** How far the emoji sit from the middle of the menu, as a share of the frame's height. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Menu", meta = (ClampMin = "0.01", UIMax = "0.5"))
	float MenuRadius = 0.12f;

	/** How big each emoji in the menu is, as a share of the frame's height. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Menu", meta = (ClampMin = "0.01", UIMax = "0.3"))
	float MenuIconSize = 0.065f;

	/** How much bigger the picked emoji grows. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Menu", meta = (ClampMin = "1.0", UIMax = "2.5"))
	float PickedScale = 1.45f;

	/** Seconds the menu takes to open, and to shut. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Menu", meta = (ClampMin = "0.01", UIMax = "1.0", Units = "s"))
	float MenuOpenSeconds = 0.16f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Menu", meta = (ClampMin = "0.01", UIMax = "1.0", Units = "s"))
	float MenuCloseSeconds = 0.12f;

	/** How many of the emoji rise over the screen when one is sent. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Screen", meta = (ClampMin = "0", UIMax = "60"))
	int32 ScreenCount = 14;

	/** Seconds over which they set off, one after another. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Screen", meta = (ClampMin = "0.0", UIMax = "3.0", Units = "s"))
	float ScreenSpreadSeconds = 0.8f;

	/** Seconds each takes to rise from the bottom of the frame to the top, give or take a fifth. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Screen", meta = (ClampMin = "0.2", UIMax = "8.0", Units = "s"))
	float ScreenSeconds = 2.6f;

	/** How big they are, as a share of the frame's height, give or take a quarter. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Screen", meta = (ClampMin = "0.01", UIMax = "0.3"))
	float ScreenSize = 0.085f;

	/** How many emoji the drone gives off for one sent. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0", UIMax = "200"))
	int32 BurstCount = 30;

	/** Seconds the drone keeps giving them off. The first third come at once. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0.0", UIMax = "2.0", Units = "s"))
	float EmitSeconds = 0.4f;

	/** How fast they fly out, at most; the slowest go at 60% of it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0.0", UIMax = "3000.0", Units = "cm/s"))
	float Speed = 650.0f;

	/** How quickly the air slows them: the share of their speed lost in a second is about this, while it is small. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0.0", UIMax = "5.0"))
	float Drag = 1.2f;

	/** 0 sends them out every way alike; towards 1 more of them go the way this component faces. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ForwardBias = 0.5f;

	/** How much of the drone's own speed they set off with. At 0 a fast drone leaves them straight behind. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float InheritVelocity = 0.5f;

	/** Seconds each lasts at most; some go a quarter sooner. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0.1", UIMax = "5.0", Units = "s"))
	float Lifetime = 1.5f;

	/** How wide one is right at the drone. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0.0", UIMax = "300.0", Units = "cm"))
	float NearSize = 40.0f;

	/** How wide one is once ShrinkDistance or farther from the drone. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "0.0", UIMax = "100.0", Units = "cm"))
	float FarSize = 3.0f;

	/** How far from the drone they have shrunk all the way to FarSize. They shrink quickest near the drone. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone", meta = (ClampMin = "1.0", UIMax = "5000.0", Units = "cm"))
	float ShrinkDistance = 700.0f;

	/** The square each is drawn on: a 100 cm plane facing +Z, like the engine's. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone|Advanced")
	TObjectPtr<UStaticMesh> QuadMesh;

	/** Takes the texture parameter Emoji and the scalar Brightness. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emoji|Drone|Advanced")
	TObjectPtr<UMaterialInterface> Material;

	/** Sends emoji Index as letting go of the menu would: rising over this drone's player's screen, and a burst from the drone. */
	UFUNCTION(BlueprintCallable, Category = "Emoji")
	void SendEmoji(int32 Index);

	UFUNCTION(BlueprintPure, Category = "Emoji")
	bool IsMenuOpen() const { return bMenuHeld; }

	/**
	 * The emoji a stick leaning this way points at, of NumEmoji round a ring clockwise from the top.
	 * Inside DeadZone, or close to the edge of Current's share of the ring, it stays Current, so the
	 * pick does not flicker between two neighbours.
	 */
	static int32 PickEmoji(const FVector2D& Stick, int32 NumEmoji, float DeadZone, int32 Current);

	/** How wide an emoji in the burst is this far from the drone, before it grows in and shrinks out. */
	float GetSizeAtDistance(float Distance) const;

	/** Steps the menu on with the menu key held or not and the stick where it is. Every tick feeds it the player's input; tests feed it their own. */
	void UpdateMenu(bool bHeld, const FVector2D& Stick, float DeltaTime);

	/** Steps the burst and the emoji rising over the screen on, and draws the burst. Every tick does this. */
	void Advance(float DeltaTime);

	const FEmojiScreenState& GetScreenState() const { return Screen; }

	int32 GetNumParticles() const { return Particles.Num(); }

	/**
	 * What draws the burst's emoji Index for viewer Viewer (0 the first player; each player sees squares of their own,
	 * turned to them), or null while it has never been sent.
	 */
	UInstancedStaticMeshComponent* GetCloud(int32 Index, int32 Viewer = 0) const;

	//~ Begin UActorComponent Interface
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	//~ End UActorComponent Interface

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** One emoji of a burst, flying through the world. */
	struct FParticle
	{
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.0f;
		float Lifetime = 1.0f;

		/** Turn about the line to the viewer, in radians, and how fast it turns. */
		float Roll = 0.0f;
		float Spin = 0.0f;
		int32 Emoji = 0;
	};

	/** A burst still giving off emoji. */
	struct FEmitter
	{
		int32 Emoji = 0;
		float Age = 0.0f;
		int32 Spawned = 0;
	};

	/** One emoji rising over the screen. Places are shares of the frame, as in FEmojiScreenSprite. */
	struct FRiser
	{
		int32 Emoji = 0;

		/** Below zero until it sets off. */
		float Age = 0.0f;
		float Lifetime = 1.0f;
		float X = 0.5f;
		float Size = 0.1f;
		float Sway = 0.0f;
		float SwayRate = 1.0f;
		float Phase = 0.0f;
		float Tilt = 0.0f;
	};

	/** Reads the menu key and the stick from the player who controls the drone, and keeps the stick from turning the drone while the menu needs it. */
	void ReadInput(float DeltaTime, bool& bOutHeld, FVector2D& OutStick);

	/** Moves the menu's input over to the player now controlling the drone, or takes it away when there is none. */
	void SetInputPlayer(APlayerController* Player);
	void AddAimContext();
	void RemoveAimContext();

	/** The player on this machine who controls the drone, or null. */
	APlayerController* GetLocalPlayerController() const;

	void Emit(FEmitter& Emitter, int32 Count);
	void UpdateParticles(float DeltaTime);
	void UpdateRisers(float DeltaTime);
	void DrawParticles();
	UInstancedStaticMeshComponent* FindOrMakeCloud(int32 Index, int32 Viewer, const TArray<FWEMViewer>& Viewers);

	/** Shows each viewer's squares to that viewer's player only, again whenever the players change. */
	void ShowCloudsTo(const TArray<FWEMViewer>& Viewers);

	/** Puts the menu on the screen of the player now controlling the drone, or takes it off when there is none. */
	void ShowOverlay(ULocalPlayer* Viewer);
	void HideOverlay();
	void ApplyStyle();

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> AimAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MenuContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> AimContext;

	TWeakObjectPtr<APlayerController> InputPlayer;
	bool bAimContextAdded = false;

	/** Seconds since the menu shut while the stick still leans. */
	float SinceMenuShut = 0.0f;

	bool bMenuHeld = false;
	FEmojiScreenState Screen;

	TArray<FParticle> Particles;
	TArray<FEmitter> Emitters;
	TArray<FRiser> Risers;

	/**
	 * One per emoji and viewer, drawing that emoji's squares of every burst, turned to that viewer: viewer 0's for every
	 * emoji, then viewer 1's, and so on. They belong to the drone, so they go when it does.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Clouds;

	/** The player each viewer's squares were last shown to. */
	TArray<TWeakObjectPtr<APlayerController>> CloudPlayers;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> CloudMaterials;

	TSharedPtr<SEmojiOverlay> Overlay;
	TWeakObjectPtr<ULocalPlayer> OverlayPlayer;

	FRandomStream Random;
};

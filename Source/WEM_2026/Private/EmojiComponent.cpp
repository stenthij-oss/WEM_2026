// Copyright Epic Games, Inc. All Rights Reserved.

#include "EmojiComponent.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "EmojiOverlay.h"
#include "WEMPlayers.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(EmojiComponent)

namespace EmojiPrivate
{
	const FName EmojiParameter(TEXT("Emoji"));

	/** The emoji in /Game/Emoji the component starts with, round the menu clockwise from the top. */
	const TCHAR* const DefaultEmoji[] = {
		TEXT("Grin"), TEXT("Joy"), TEXT("HeartEyes"), TEXT("Wow"), TEXT("Cry"), TEXT("Angry"), TEXT("ThumbsUp"), TEXT("Heart"), TEXT("Fire")
	};

	/** Above the drone's own input, so while the menu is open the stick's lean goes to it rather than to turning. */
	constexpr int32 MenuPriority = 50;
	constexpr int32 AimPriority = 100;

	/** Over the orb counter and anything else drawn on the game view. */
	constexpr int32 OverlayZOrder = 20;

	/** A pick holds until the stick leans this far past the edge of its slice of the ring, as a share of a slice. */
	constexpr double PickHysteresis = 0.15;

	/** Once the menu shuts, the stick turns the drone again when it is back inside this lean, or after this many seconds whatever. */
	constexpr float AimReleaseLean = 0.25f;
	constexpr float AimReleaseSeconds = 0.6f;

	/** How quickly a picked emoji grows, and one no longer picked shrinks back: about the share of the way left covered in a second. */
	constexpr float HighlightRate = 18.0f;

	/** The engine's plane, which the quad is expected to be like, is 100 cm across. */
	constexpr float QuadSize = 100.0f;

	/** The engine's plane has the picture's top along its -Y. A quarter turn about its face brings the top to +X, which faces the viewer's up. */
	const FQuat PictureTurn(FVector(0.0, 0.0, 1.0), UE_HALF_PI);

	/** Seconds a burst emoji takes to grow in, and the share of its life at the end it spends shrinking away. */
	constexpr float GrowSeconds = 0.08f;
	constexpr float ShrinkShare = 0.25f;

	/** The share of a burst given off at once. */
	constexpr float BurstFirstShare = 1.0f / 3.0f;

	/** How far from the component a burst emoji starts, at most, in cm. */
	constexpr float SpawnRadius = 10.0f;

	/** The most emoji of bursts in flight at once, so a quick run of sends cannot pile up. */
	constexpr int32 MaxParticles = 600;

	/** How far a burst emoji starts turned, and how fast it turns as it flies, at most, in degrees and degrees a second. */
	constexpr float MaxRollDegrees = 15.0f;
	constexpr float MaxSpinDegrees = 35.0f;

	/** The emoji rising over the screen: where across the frame they rise, as shares of its width, how far each sways to the sides and how often, and how far it leans as it sways. */
	constexpr float ScreenMinX = 0.08f;
	constexpr float ScreenMaxX = 0.92f;
	constexpr float MinSway = 0.015f;
	constexpr float MaxSway = 0.045f;
	constexpr float MinSwayRate = 0.5f;
	constexpr float MaxSwayRate = 1.0f;
	constexpr float MinTiltDegrees = 6.0f;
	constexpr float MaxTiltDegrees = 14.0f;

	UEnhancedInputLocalPlayerSubsystem* GetInputSubsystem(const APlayerController* Player)
	{
		const ULocalPlayer* LocalPlayer = Player ? Player->GetLocalPlayer() : nullptr;
		return LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	}

	/** Keys already down when a context comes or goes still count, so the stick picks from the moment the menu opens. */
	FModifyContextOptions KeepPressedKeys()
	{
		FModifyContextOptions Options;
		Options.bIgnoreAllPressedKeysUntilRelease = false;
		return Options;
	}

	void SendFromPlaying(const TArray<FString>& Args)
	{
		const int32 Index = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
		for (TObjectIterator<UEmojiComponent> It; It; ++It)
		{
			if (It->HasBegunPlay())
			{
				It->SendEmoji(Index);
			}
		}
	}

	FAutoConsoleCommand SendCommand(
		TEXT("wem.Emoji.Send"),
		TEXT("wem.Emoji.Send <index, default 0>: sends that emoji from every playing drone, as letting go of the emoji menu would."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&SendFromPlaying));
}

UEmojiComponent::UEmojiComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// After the drone has moved this frame, so the burst sets off from where it is.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	struct FDefaults
	{
		ConstructorHelpers::FObjectFinder<UStaticMesh> Quad;
		ConstructorHelpers::FObjectFinder<UMaterialInterface> Material;
		TArray<UTexture2D*> Emoji;

		FDefaults()
			: Quad(TEXT("/Engine/BasicShapes/Plane.Plane"))
			, Material(TEXT("/Game/Emoji/M_Emoji.M_Emoji"))
		{
			for (const TCHAR* Name : EmojiPrivate::DefaultEmoji)
			{
				ConstructorHelpers::FObjectFinder<UTexture2D> Finder(*FString::Printf(TEXT("/Game/Emoji/T_Emoji_%s.T_Emoji_%s"), Name, Name));
				if (Finder.Object)
				{
					Emoji.Add(Finder.Object);
				}
			}
		}
	};
	static FDefaults Defaults;

	QuadMesh = Defaults.Quad.Object;
	Material = Defaults.Material.Object;
	Emoji.Append(Defaults.Emoji);
}

int32 UEmojiComponent::PickEmoji(const FVector2D& Stick, const int32 NumEmoji, const float DeadZone, const int32 Current)
{
	if (NumEmoji <= 0)
	{
		return INDEX_NONE;
	}

	const bool bHasCurrent = Current >= 0 && Current < NumEmoji;

	if (Stick.Size() < DeadZone)
	{
		return bHasCurrent ? Current : INDEX_NONE;
	}

	// Which way it leans, in slices of the ring: 0 straight up, growing clockwise.
	const double Turn = FMath::Atan2(Stick.X, Stick.Y) / UE_DOUBLE_TWO_PI * NumEmoji;

	if (bHasCurrent)
	{
		double Off = Turn - Current;
		Off -= NumEmoji * FMath::RoundToDouble(Off / NumEmoji);

		if (FMath::Abs(Off) < 0.5 + EmojiPrivate::PickHysteresis)
		{
			return Current;
		}
	}

	const int32 Nearest = FMath::RoundToInt32(Turn);
	return (Nearest % NumEmoji + NumEmoji) % NumEmoji;
}

float UEmojiComponent::GetSizeAtDistance(const float Distance) const
{
	// Quickest to shrink near the drone, then slower as it nears the smallest.
	const float Along = FMath::Clamp(Distance / FMath::Max(ShrinkDistance, 1.0f), 0.0f, 1.0f);
	return FMath::Lerp(NearSize, FarSize, 1.0f - FMath::Square(1.0f - Along));
}

UInstancedStaticMeshComponent* UEmojiComponent::GetCloud(const int32 Index, const int32 Viewer) const
{
	const int32 Slot = Viewer * Emoji.Num() + Index;
	return Emoji.IsValidIndex(Index) && Viewer >= 0 && Clouds.IsValidIndex(Slot) ? Clouds[Slot].Get() : nullptr;
}

void UEmojiComponent::BeginPlay()
{
	Super::BeginPlay();

	Random.GenerateNewSeed();

	MenuAction = NewObject<UInputAction>(this, TEXT("EmojiMenu"), RF_Transient);
	MenuAction->ValueType = EInputActionValueType::Boolean;

	AimAction = NewObject<UInputAction>(this, TEXT("EmojiAim"), RF_Transient);
	AimAction->ValueType = EInputActionValueType::Axis2D;

	MenuContext = NewObject<UInputMappingContext>(this, TEXT("EmojiMenuContext"), RF_Transient);
	MenuContext->MapKey(MenuAction, MenuKey);

	// Only there while the menu needs the stick. Mapped above the drone's own input, it keeps the stick from turning the drone meanwhile.
	AimContext = NewObject<UInputMappingContext>(this, TEXT("EmojiAimContext"), RF_Transient);
	AimContext->MapKey(AimAction, AimKey);
}

void UEmojiComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	SetInputPlayer(nullptr);
	HideOverlay();

	for (UInstancedStaticMeshComponent* Cloud : Clouds)
	{
		if (Cloud)
		{
			Cloud->DestroyComponent();
		}
	}

	Clouds.Reset();
	CloudMaterials.Reset();
	Particles.Reset();
	Emitters.Reset();
	Risers.Reset();

	Super::EndPlay(EndPlayReason);
}

void UEmojiComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	APlayerController* Player = GetLocalPlayerController();
	SetInputPlayer(Player);

	ULocalPlayer* Viewer = Player ? Player->GetLocalPlayer() : nullptr;

	if (Viewer != OverlayPlayer.Get() || (Viewer && !Overlay.IsValid()))
	{
		HideOverlay();
		ShowOverlay(Viewer);
	}

	bool bHeld = false;
	FVector2D Stick = FVector2D::ZeroVector;
	ReadInput(DeltaTime, bHeld, Stick);

	UpdateMenu(bHeld, Stick, DeltaTime);
	Advance(DeltaTime);

	if (Overlay.IsValid())
	{
		Overlay->SetState(Screen);
	}
}

APlayerController* UEmojiComponent::GetLocalPlayerController() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* Player = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	return Player && Player->IsLocalController() ? Player : nullptr;
}

void UEmojiComponent::SetInputPlayer(APlayerController* Player)
{
	using namespace EmojiPrivate;

	if (Player == InputPlayer.Get())
	{
		return;
	}

	// Leave the last player's input as it was.
	if (UEnhancedInputLocalPlayerSubsystem* Input = GetInputSubsystem(InputPlayer.Get()))
	{
		Input->RemoveMappingContext(MenuContext, KeepPressedKeys());

		if (bAimContextAdded)
		{
			Input->RemoveMappingContext(AimContext, KeepPressedKeys());
		}
	}

	bAimContextAdded = false;
	InputPlayer = Player;

	if (UEnhancedInputLocalPlayerSubsystem* Input = GetInputSubsystem(Player))
	{
		if (MenuContext)
		{
			Input->AddMappingContext(MenuContext, MenuPriority, KeepPressedKeys());
		}
	}
}

void UEmojiComponent::AddAimContext()
{
	if (UEnhancedInputLocalPlayerSubsystem* Input = EmojiPrivate::GetInputSubsystem(InputPlayer.Get()))
	{
		Input->AddMappingContext(AimContext, EmojiPrivate::AimPriority, EmojiPrivate::KeepPressedKeys());
		bAimContextAdded = true;
	}
}

void UEmojiComponent::RemoveAimContext()
{
	if (UEnhancedInputLocalPlayerSubsystem* Input = EmojiPrivate::GetInputSubsystem(InputPlayer.Get()))
	{
		Input->RemoveMappingContext(AimContext, EmojiPrivate::KeepPressedKeys());
	}

	bAimContextAdded = false;
}

void UEmojiComponent::ReadInput(const float DeltaTime, bool& bOutHeld, FVector2D& OutStick)
{
	using namespace EmojiPrivate;

	bOutHeld = false;
	OutStick = FVector2D::ZeroVector;

	const UEnhancedInputLocalPlayerSubsystem* Input = GetInputSubsystem(InputPlayer.Get());
	const UEnhancedPlayerInput* PlayerInput = Input ? Input->GetPlayerInput() : nullptr;

	if (!PlayerInput || !MenuAction || !AimAction)
	{
		return;
	}

	bOutHeld = PlayerInput->GetActionValue(MenuAction).Get<bool>();

	if (bAimContextAdded)
	{
		// The menu takes up as positive.
		OutStick = PlayerInput->GetActionValue(AimAction).Get<FVector2D>();
		OutStick.Y *= bStickUpIsNegative ? -1.0 : 1.0;
	}

	if (bOutHeld)
	{
		SinceMenuShut = 0.0f;

		if (!bAimContextAdded)
		{
			AddAimContext();
		}
	}
	else if (bAimContextAdded)
	{
		// The stick goes back to turning the drone once it is let go, so the view does not swing with the hand coming off it.
		SinceMenuShut += DeltaTime;

		if (OutStick.Size() < AimReleaseLean || SinceMenuShut >= AimReleaseSeconds)
		{
			RemoveAimContext();
		}
	}
}

void UEmojiComponent::UpdateMenu(const bool bHeld, const FVector2D& Stick, const float DeltaTime)
{
	using namespace EmojiPrivate;

	const int32 NumEmoji = Emoji.Num();
	Screen.Highlight.SetNumZeroed(NumEmoji);

	if (bHeld)
	{
		// Every opening starts with nothing picked; after that the last pick holds while the stick rests.
		Screen.Picked = PickEmoji(Stick, NumEmoji, AimDeadZone, bMenuHeld ? Screen.Picked : INDEX_NONE);
	}
	else if (bMenuHeld && Emoji.IsValidIndex(Screen.Picked))
	{
		SendEmoji(Screen.Picked);
	}

	bMenuHeld = bHeld;

	const float Step = bHeld ? DeltaTime / FMath::Max(MenuOpenSeconds, 0.01f) : -DeltaTime / FMath::Max(MenuCloseSeconds, 0.01f);
	Screen.MenuOpen = FMath::Clamp(Screen.MenuOpen + Step, 0.0f, 1.0f);

	if (!bHeld && Screen.MenuOpen <= 0.0f)
	{
		Screen.Picked = INDEX_NONE;
	}

	const float Approach = 1.0f - FMath::Exp(-HighlightRate * DeltaTime);

	for (int32 Index = 0; Index < NumEmoji; ++Index)
	{
		const float Target = Index == Screen.Picked ? 1.0f : 0.0f;
		Screen.Highlight[Index] += (Target - Screen.Highlight[Index]) * Approach;
	}
}

void UEmojiComponent::SendEmoji(const int32 Index)
{
	using namespace EmojiPrivate;

	if (!Emoji.IsValidIndex(Index))
	{
		return;
	}

	FEmitter& Emitter = Emitters.AddDefaulted_GetRef();
	Emitter.Emoji = Index;

	// Each rises in its own strip of the frame, the strips shuffled, so they spread across it rather than bunching.
	const int32 Count = FMath::Max(ScreenCount, 0);
	TArray<int32> Strips;
	Strips.SetNum(Count);

	for (int32 Strip = 0; Strip < Count; ++Strip)
	{
		Strips[Strip] = Strip;
	}

	for (int32 Strip = Count - 1; Strip > 0; --Strip)
	{
		Strips.Swap(Strip, Random.RandRange(0, Strip));
	}

	for (int32 Number = 0; Number < Count; ++Number)
	{
		FRiser& Riser = Risers.AddDefaulted_GetRef();
		Riser.Emoji = Index;
		Riser.Age = -ScreenSpreadSeconds * (Number + 0.8f * Random.FRand()) / Count;
		Riser.Lifetime = ScreenSeconds * Random.FRandRange(0.8f, 1.2f);
		Riser.X = FMath::Lerp(ScreenMinX, ScreenMaxX, (Strips[Number] + Random.FRand()) / Count);
		Riser.Size = ScreenSize * Random.FRandRange(0.75f, 1.25f);
		Riser.Sway = Random.FRandRange(MinSway, MaxSway);
		Riser.SwayRate = Random.FRandRange(MinSwayRate, MaxSwayRate);
		Riser.Phase = Random.FRandRange(0.0f, UE_TWO_PI);
		Riser.Tilt = FMath::DegreesToRadians(Random.FRandRange(MinTiltDegrees, MaxTiltDegrees));
	}
}

void UEmojiComponent::Advance(const float DeltaTime)
{
	using namespace EmojiPrivate;

	for (int32 Index = Emitters.Num() - 1; Index >= 0; --Index)
	{
		FEmitter& Emitter = Emitters[Index];
		Emitter.Age += DeltaTime;

		const float Share = EmitSeconds > 0.0f ? FMath::Lerp(BurstFirstShare, 1.0f, FMath::Clamp(Emitter.Age / EmitSeconds, 0.0f, 1.0f)) : 1.0f;
		const int32 Due = FMath::RoundToInt32(BurstCount * Share);
		Emit(Emitter, Due - Emitter.Spawned);

		if (Emitter.Spawned >= BurstCount)
		{
			Emitters.RemoveAtSwap(Index);
		}
	}

	UpdateParticles(DeltaTime);
	UpdateRisers(DeltaTime);
	DrawParticles();
}

void UEmojiComponent::Emit(FEmitter& Emitter, const int32 Count)
{
	using namespace EmojiPrivate;

	if (Count <= 0)
	{
		return;
	}

	Emitter.Spawned += Count;

	const FVector Origin = GetComponentLocation();
	const FVector Forward = GetForwardVector();
	const FVector Carried = GetOwner() ? GetOwner()->GetVelocity() * InheritVelocity : FVector::ZeroVector;

	for (int32 Number = FMath::Min(Count, MaxParticles - Particles.Num()); Number > 0; --Number)
	{
		// A way out at random, pulled towards forward: at ForwardBias 1 they all go within 30 degrees of it.
		FVector Direction = (Random.GetUnitVector() + 2.0f * ForwardBias * Forward).GetSafeNormal();

		if (Direction.IsZero())
		{
			Direction = Forward;
		}

		FParticle& Particle = Particles.AddDefaulted_GetRef();
		Particle.Emoji = Emitter.Emoji;
		Particle.Position = Origin + Direction * Random.FRandRange(0.0f, SpawnRadius);
		Particle.Velocity = Direction * Speed * Random.FRandRange(0.6f, 1.0f) + Carried;
		Particle.Lifetime = Lifetime * Random.FRandRange(0.75f, 1.0f);
		Particle.Roll = FMath::DegreesToRadians(Random.FRandRange(-MaxRollDegrees, MaxRollDegrees));
		Particle.Spin = FMath::DegreesToRadians(Random.FRandRange(-MaxSpinDegrees, MaxSpinDegrees));
	}
}

void UEmojiComponent::UpdateParticles(const float DeltaTime)
{
	const float Slowing = FMath::Exp(-Drag * DeltaTime);

	for (int32 Index = Particles.Num() - 1; Index >= 0; --Index)
	{
		FParticle& Particle = Particles[Index];
		Particle.Age += DeltaTime;

		if (Particle.Age >= Particle.Lifetime)
		{
			Particles.RemoveAtSwap(Index);
			continue;
		}

		Particle.Velocity *= Slowing;
		Particle.Position += Particle.Velocity * DeltaTime;
		Particle.Roll += Particle.Spin * DeltaTime;
	}
}

void UEmojiComponent::UpdateRisers(const float DeltaTime)
{
	Screen.Sprites.Reset();

	for (int32 Index = Risers.Num() - 1; Index >= 0; --Index)
	{
		FRiser& Riser = Risers[Index];
		Riser.Age += DeltaTime;

		if (Riser.Age >= Riser.Lifetime)
		{
			Risers.RemoveAtSwap(Index);
		}
	}

	for (const FRiser& Riser : Risers)
	{
		if (Riser.Age < 0.0f)
		{
			continue;
		}

		const float Along = Riser.Age / Riser.Lifetime;

		// From just below the frame to just above it, quickening a little as it goes, like a bubble.
		const float Rise = 0.75f * Along + 0.25f * Along * Along;
		const float Wave = UE_TWO_PI * Riser.SwayRate * Riser.Age + Riser.Phase;

		// Coming up to the surface: it grows and clears in its first moments, and fades out as it nears the top.
		const float Surfacing = FMath::SmoothStep(0.0f, 0.3f, Along);

		FEmojiScreenSprite& Sprite = Screen.Sprites.AddDefaulted_GetRef();
		Sprite.Emoji = Riser.Emoji;
		Sprite.Size = Riser.Size * (0.55f + 0.45f * Surfacing);
		Sprite.Centre = FVector2f(Riser.X + Riser.Sway * FMath::Sin(Wave), FMath::Lerp(1.0f + 0.5f * Riser.Size, -0.5f * Riser.Size, Rise));
		Sprite.Angle = Riser.Tilt * FMath::Cos(Wave);
		Sprite.Opacity = FMath::SmoothStep(0.0f, 0.12f, Along) * (1.0f - FMath::SmoothStep(0.8f, 1.0f, Along));
	}
}

void UEmojiComponent::DrawParticles()
{
	using namespace EmojiPrivate;

	bool bAnyDrawn = false;

	for (const UInstancedStaticMeshComponent* Cloud : Clouds)
	{
		bAnyDrawn |= Cloud && Cloud->GetInstanceCount() > 0;
	}

	if (Particles.IsEmpty() && !bAnyDrawn)
	{
		return;
	}

	// Every player sees squares of their own, turned to them, the other players' drones' bursts too; with nobody looking
	// (Simulate shows the editor's view), one set, and with no view at all it faces back along the drone.
	TArray<FWEMViewer> Viewers;
	WEMPlayers::GatherViewers(GetWorld(), Viewers);
	ShowCloudsTo(Viewers);

	const int32 NumViewers = FMath::Max(Viewers.Num(), 1);
	const FVector Drone = GetComponentLocation();
	const FVector Behind = -GetForwardVector();

	TArray<TArray<FTransform>> PerEmoji;

	for (int32 Viewer = 0; Viewer < NumViewers; ++Viewer)
	{
		const bool bHasViewer = Viewers.IsValidIndex(Viewer);
		const FVector ViewerLocation = bHasViewer ? Viewers[Viewer].Location : FVector::ZeroVector;
		const FVector ViewerUp = bHasViewer ? Viewers[Viewer].Up : FVector::UpVector;

		PerEmoji.Reset();
		PerEmoji.SetNum(Emoji.Num());

		for (const FParticle& Particle : Particles)
		{
			if (!PerEmoji.IsValidIndex(Particle.Emoji))
			{
				continue;
			}

			const float Grown = FMath::SmoothStep(0.0f, 1.0f, Particle.Age / GrowSeconds);
			const float Left = FMath::Clamp((Particle.Lifetime - Particle.Age) / (ShrinkShare * Particle.Lifetime), 0.0f, 1.0f);
			const float Size = GetSizeAtDistance(static_cast<float>(FVector::Distance(Particle.Position, Drone))) * Grown * Left;

			// The square's face towards the viewer and the picture's top towards their up, so it reads upright however the drone is turned; then its own turn.
			FVector ToViewer = bHasViewer ? (ViewerLocation - Particle.Position).GetSafeNormal() : Behind;

			if (ToViewer.IsZero())
			{
				ToViewer = Behind;
			}

			const FQuat Facing = FQuat(ToViewer, Particle.Roll) * FRotationMatrix::MakeFromZX(ToViewer, ViewerUp).ToQuat() * PictureTurn;
			PerEmoji[Particle.Emoji].Emplace(Facing, Particle.Position, FVector(FMath::Max(Size, 0.0f) / QuadSize));
		}

		for (int32 Index = 0; Index < PerEmoji.Num(); ++Index)
		{
			const TArray<FTransform>& Transforms = PerEmoji[Index];
			UInstancedStaticMeshComponent* Cloud = GetCloud(Index, Viewer);

			if (Transforms.IsEmpty())
			{
				if (Cloud && Cloud->GetInstanceCount() > 0)
				{
					Cloud->ClearInstances();
				}

				continue;
			}

			Cloud = FindOrMakeCloud(Index, Viewer, Viewers);

			if (!Cloud)
			{
				continue;
			}

			if (Cloud->GetInstanceCount() == Transforms.Num())
			{
				Cloud->BatchUpdateInstancesTransforms(0, Transforms, /*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/true);
			}
			else
			{
				Cloud->ClearInstances();
				Cloud->AddInstances(Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true, /*bUpdateNavigation=*/false);
			}
		}
	}

	// A viewer gone (a player who left): their squares go too.
	for (int32 Slot = NumViewers * Emoji.Num(); Slot < Clouds.Num(); ++Slot)
	{
		if (Clouds[Slot] && Clouds[Slot]->GetInstanceCount() > 0)
		{
			Clouds[Slot]->ClearInstances();
		}
	}
}

void UEmojiComponent::ShowCloudsTo(const TArray<FWEMViewer>& Viewers)
{
	bool bPlayersChanged = CloudPlayers.Num() != Viewers.Num();

	for (int32 Viewer = 0; Viewer < CloudPlayers.Num() && !bPlayersChanged; ++Viewer)
	{
		bPlayersChanged = CloudPlayers[Viewer].Get() != Viewers[Viewer].Player.Get();
	}

	if (!bPlayersChanged || Emoji.IsEmpty())
	{
		return;
	}

	CloudPlayers.Reset();

	for (const FWEMViewer& Viewer : Viewers)
	{
		CloudPlayers.Add(Viewer.Player);
	}

	for (int32 Slot = 0; Slot < Clouds.Num(); ++Slot)
	{
		WEMPlayers::ShowOnlyTo(Clouds[Slot], Viewers, Slot / Emoji.Num());
	}
}

UInstancedStaticMeshComponent* UEmojiComponent::FindOrMakeCloud(const int32 Index, const int32 Viewer, const TArray<FWEMViewer>& Viewers)
{
	if (UInstancedStaticMeshComponent* Cloud = GetCloud(Index, Viewer))
	{
		return Cloud;
	}

	AActor* Owner = GetOwner();

	if (!Owner || !QuadMesh || !Emoji.IsValidIndex(Index) || Viewer < 0 || !GetWorld())
	{
		return nullptr;
	}

	const int32 Slot = Viewer * Emoji.Num() + Index;
	Clouds.SetNum(FMath::Max(Clouds.Num(), (Viewer + 1) * Emoji.Num()));
	CloudMaterials.SetNum(Clouds.Num());

	// Instances in world space: the cloud stays where the world's origin is, wherever the drone flies.
	UInstancedStaticMeshComponent* Cloud = NewObject<UInstancedStaticMeshComponent>(Owner, NAME_None, RF_Transient);
	Cloud->SetUsingAbsoluteLocation(true);
	Cloud->SetUsingAbsoluteRotation(true);
	Cloud->SetUsingAbsoluteScale(true);
	Cloud->SetupAttachment(this);
	Cloud->SetMobility(EComponentMobility::Movable);
	Cloud->SetStaticMesh(QuadMesh);
	Cloud->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Cloud->SetGenerateOverlapEvents(false);
	Cloud->SetCanEverAffectNavigation(false);
	Cloud->SetCastShadow(false);
	Cloud->bReceivesDecals = false;
	Cloud->bAffectDistanceFieldLighting = false;
	Cloud->bAffectDynamicIndirectLighting = false;
	Cloud->bVisibleInRayTracing = false;
	Cloud->RegisterComponent();
	Cloud->SetWorldTransform(FTransform::Identity);

	if (Material)
	{
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, this);
		Instance->SetTextureParameterValue(EmojiPrivate::EmojiParameter, Emoji[Index]);
		Cloud->SetMaterial(0, Instance);
		CloudMaterials[Slot] = Instance;
	}

	// Turned to this viewer, so drawn for their player only.
	WEMPlayers::ShowOnlyTo(Cloud, Viewers, Viewer);

	Clouds[Slot] = Cloud;
	return Cloud;
}

void UEmojiComponent::ShowOverlay(ULocalPlayer* Viewer)
{
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;

	if (!Viewer || !Viewport)
	{
		return;
	}

	Overlay = SNew(SEmojiOverlay);
	ApplyStyle();
	Viewport->AddViewportWidgetForPlayer(Viewer, Overlay.ToSharedRef(), EmojiPrivate::OverlayZOrder);
	OverlayPlayer = Viewer;
}

void UEmojiComponent::HideOverlay()
{
	if (Overlay.IsValid())
	{
		UWorld* World = GetWorld();
		UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;

		if (Viewport && OverlayPlayer.IsValid())
		{
			Viewport->RemoveViewportWidgetForPlayer(OverlayPlayer.Get(), Overlay.ToSharedRef());
		}
	}

	Overlay.Reset();
	OverlayPlayer.Reset();
}

void UEmojiComponent::ApplyStyle()
{
	if (!Overlay.IsValid())
	{
		return;
	}

	FEmojiMenuStyle Style;
	Style.Radius = MenuRadius;
	Style.IconSize = MenuIconSize;
	Style.PickedScale = PickedScale;

	for (UTexture2D* Texture : Emoji)
	{
		FSlateBrush& Icon = Style.Icons.AddDefaulted_GetRef();

		if (Texture)
		{
			Icon.SetResourceObject(Texture);
			Icon.ImageSize = FVector2f(Texture->GetSizeX(), Texture->GetSizeY());
		}
		else
		{
			Icon.DrawAs = ESlateBrushDrawType::NoDrawType;
		}
	}

	Overlay->SetStyle(Style);
}

#if WITH_EDITOR
void UEmojiComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// While playing, the menu and the squares already made take the change at once.
	ApplyStyle();

	for (int32 Slot = 0; Slot < Clouds.Num(); ++Slot)
	{
		UInstancedStaticMeshComponent* Cloud = Clouds[Slot];

		if (!Cloud)
		{
			continue;
		}

		Cloud->SetStaticMesh(QuadMesh);

		if (Material && (!CloudMaterials[Slot] || CloudMaterials[Slot]->Parent != Material))
		{
			CloudMaterials[Slot] = UMaterialInstanceDynamic::Create(Material, this);
			Cloud->SetMaterial(0, CloudMaterials[Slot]);
		}

		// Every viewer's squares of an emoji show the same picture.
		const int32 Index = Emoji.IsEmpty() ? INDEX_NONE : Slot % Emoji.Num();

		if (CloudMaterials[Slot] && Emoji.IsValidIndex(Index))
		{
			CloudMaterials[Slot]->SetTextureParameterValue(EmojiPrivate::EmojiParameter, Emoji[Index]);
		}
	}
}
#endif

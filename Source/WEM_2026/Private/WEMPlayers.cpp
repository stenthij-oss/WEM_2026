// Copyright Epic Games, Inc. All Rights Reserved.

#include "WEMPlayers.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "RoomManager.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"

namespace WEMPlayersPrivate
{
	/** The portrait frame's width for a height of 1 (wem.Portrait.Ratio, made by the rendering module), else 9:16. */
	float GetFrameRatio()
	{
		static IConsoleVariable* Ratio = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.Portrait.Ratio"));
		const float Value = Ratio ? Ratio->GetFloat() : 0.0f;
		return Value > 0.0f ? Value : 9.0f / 16.0f;
	}

	/** Whether any local player has a controller in World: a game being played, not Simulate or a test's world. */
	bool HasPlayers(const UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;

		if (!GameInstance)
		{
			return false;
		}

		for (const ULocalPlayer* Local : GameInstance->GetLocalPlayers())
		{
			if (Local && Local->GetPlayerController(World))
			{
				return true;
			}
		}

		return false;
	}

	bool GetCamera(const ULocalPlayer* Local, const UWorld* World, FWEMViewer& OutViewer)
	{
		APlayerController* Player = Local ? Local->GetPlayerController(World) : nullptr;

		if (!Player || !Player->PlayerCameraManager)
		{
			return false;
		}

		OutViewer.Location = Player->PlayerCameraManager->GetCameraLocation();
		OutViewer.Up = Player->PlayerCameraManager->GetCameraRotation().Quaternion().GetUpVector();
		OutViewer.Player = Player;
		return true;
	}

	bool GetLastView(const UWorld* World, FWEMViewer& OutViewer)
	{
		if (!World || World->ViewLocationsRenderedLastFrame.IsEmpty())
		{
			return false;
		}

		OutViewer.Location = World->ViewLocationsRenderedLastFrame[0];
		OutViewer.Up = FVector::UpVector;
		OutViewer.Player = nullptr;
		return true;
	}
}

TArray<FBox2D> WEMPlayers::GetScreenFrames(const FVector2D& ViewportSize, const int32 Players, const float Ratio)
{
	TArray<FBox2D> Frames;

	if (Players <= 0 || ViewportSize.X <= 0.0 || ViewportSize.Y <= 0.0 || Ratio <= 0.0f)
	{
		return Frames;
	}

	// As tall as the viewport; in one too narrow for that, as wide as leaves the narrowest gaps, and centred up and down.
	const double MinGap = MinGapShare * ViewportSize.X;
	double Width = ViewportSize.Y * Ratio;
	double Height = ViewportSize.Y;

	if (Players * Width + (Players + 1) * MinGap > ViewportSize.X)
	{
		Width = (ViewportSize.X - (Players + 1) * MinGap) / Players;
		Height = Width / Ratio;
	}

	const double Gap = (ViewportSize.X - Players * Width) / (Players + 1);
	const double Top = 0.5 * (ViewportSize.Y - Height);

	for (int32 Index = 0; Index < Players; ++Index)
	{
		const double Left = Gap + Index * (Width + Gap);
		Frames.Emplace(FVector2D(Left, Top) / ViewportSize, FVector2D(Left + Width, Top + Height) / ViewportSize);
	}

	return Frames;
}

FTransform WEMPlayers::GetStart(const FBox& Cube, const int32 Index, const int32 Players, const double Distance, const double Height)
{
	const FVector Centre = Cube.GetCenter();
	const FVector Extent = Cube.GetExtent();

	// Evenly round the middle, the first player towards -X.
	const double Angle = UE_DOUBLE_PI + 2.0 * UE_DOUBLE_PI * Index / FMath::Max(Players, 1);
	const FVector Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);

	// Out along that line to the cube's side - the middle of a face, for two - and on past it.
	const double ToSideX = FMath::Abs(Out.X) > UE_KINDA_SMALL_NUMBER ? Extent.X / FMath::Abs(Out.X) : UE_BIG_NUMBER;
	const double ToSideY = FMath::Abs(Out.Y) > UE_KINDA_SMALL_NUMBER ? Extent.Y / FMath::Abs(Out.Y) : UE_BIG_NUMBER;
	FVector Location = Centre + Out * (FMath::Min(ToSideX, ToSideY) + Distance);
	Location.Z = FMath::Lerp(Cube.Min.Z, Cube.Max.Z, FMath::Clamp(Height, 0.0, 1.0));

	// Looking level, back towards the middle.
	return FTransform((-Out).Rotation(), Location);
}

void WEMPlayers::GatherViewers(const UWorld* World, TArray<FWEMViewer>& OutViewers)
{
	using namespace WEMPlayersPrivate;

	OutViewers.Reset();

	if (HasPlayers(World))
	{
		for (const ULocalPlayer* Local : World->GetGameInstance()->GetLocalPlayers())
		{
			FWEMViewer Viewer;

			if (GetCamera(Local, World, Viewer))
			{
				OutViewers.Add(Viewer);
			}
		}

		return;
	}

	// Simulate has no player: the editor's view instead, so what turns to the players can be looked at there too.
	FWEMViewer Viewer;

	if (GetLastView(World, Viewer))
	{
		OutViewers.Add(Viewer);
	}
}

bool WEMPlayers::GetViewer(const UWorld* World, const int32 Player, FWEMViewer& OutViewer)
{
	using namespace WEMPlayersPrivate;

	if (HasPlayers(World))
	{
		const TArray<ULocalPlayer*>& Locals = World->GetGameInstance()->GetLocalPlayers();
		return Locals.IsValidIndex(Player) && GetCamera(Locals[Player], World, OutViewer);
	}

	return Player == 0 && GetLastView(World, OutViewer);
}

int32 WEMPlayers::GetPlayerIndex(const APlayerController* Player)
{
	const ULocalPlayer* Local = Player ? Player->GetLocalPlayer() : nullptr;
	const UGameInstance* GameInstance = Local ? Local->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetLocalPlayers().IndexOfByKey(Local) : INDEX_NONE;
}

void WEMPlayers::ShowOnlyTo(UPrimitiveComponent* Component, const TArray<FWEMViewer>& Viewers, const int32 Index)
{
	if (!Component)
	{
		return;
	}

	for (int32 Each = 0; Each < Viewers.Num(); ++Each)
	{
		if (APlayerController* Player = Viewers[Each].Player.Get())
		{
			if (Each == Index)
			{
				Player->HiddenPrimitiveComponents.Remove(Component);
			}
			else
			{
				Player->HiddenPrimitiveComponents.AddUnique(Component);
			}
		}
	}
}

bool UWEMPlayersSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UWEMPlayersSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	const UWEMPlayersSettings* Settings = GetDefault<UWEMPlayersSettings>();
	UGameInstance* GameInstance = InWorld.GetGameInstance();

	// Screens to split need the game's own viewport: a server, or a world without one, plays as it is.
	if (Settings->Players < 2 || !GameInstance || !GameInstance->GetGameViewportClient() || InWorld.GetNetMode() == NM_Client)
	{
		return;
	}

	// This runs before any actor has begun play, and the first player's drone already stands at the player start: the
	// game mode spawns each player's drone as the player joins. The others join now, on the next pads.
	while (GameInstance->GetNumLocalPlayers() < Settings->Players)
	{
		int32 ControllerId = 0;

		while (GameInstance->FindLocalPlayerFromControllerId(ControllerId))
		{
			++ControllerId;
		}

		FString Error;

		if (!GameInstance->CreateLocalPlayer(ControllerId, Error, /*bSpawnPlayerController=*/true))
		{
			UE_LOG(LogTemp, Warning, TEXT("Players: could not add player %d: %s"), GameInstance->GetNumLocalPlayers() + 1, *Error);
			break;
		}
	}

	FBox Cube;

	if (!FindCube(InWorld, Cube))
	{
		UE_LOG(LogTemp, Warning, TEXT("Players: no room manager in the level, so the drones start where the game mode put them."));
		return;
	}

	// Moved before they begin play, so nothing - a drone's trail least of all - starts where they were spawned.
	const TArray<ULocalPlayer*>& Locals = GameInstance->GetLocalPlayers();

	for (int32 Index = 0; Index < Locals.Num(); ++Index)
	{
		APlayerController* Player = Locals[Index] ? Locals[Index]->GetPlayerController(&InWorld) : nullptr;

		if (!Player)
		{
			continue;
		}

		const FTransform Start = WEMPlayers::GetStart(Cube, Index, Locals.Num(), 100.0 * Settings->StartDistance, Settings->StartHeight);

		if (APawn* Pawn = Player->GetPawn())
		{
			Pawn->SetActorLocationAndRotation(Start.GetLocation(), Start.GetRotation(), /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		}

		// The drone turns to where its player looks.
		Player->SetControlRotation(Start.Rotator());
	}
}

bool UWEMPlayersSubsystem::FindCube(const UWorld& World, FBox& OutCube)
{
	for (TActorIterator<ARoomManager> It(&World); It; ++It)
	{
		// Its cells run up from the actor's corner along every axis. The room manager rounds the size down to whole
		// modules and finds its height when it builds, which it has not done yet; a module either way does not matter
		// for where a drone starts.
		const FVector Min = It->GetActorLocation() + FVector(0.0, 0.0, It->GridHeightOffset);
		OutCube = FBox(Min, Min + FVector(static_cast<double>(It->GridSize) * It->CellSize));
		return true;
	}

	return false;
}

void UWEMGameViewportClient::LayoutPlayers()
{
	Super::LayoutPlayers();

	TArray<ULocalPlayer*, TInlineAllocator<4>> Players;

	for (ULocalPlayer* Player : GetOuterUEngine()->GetGamePlayers(this))
	{
		if (Player && Player->ViewportClient == this)
		{
			Players.Add(Player);
		}
	}

	LaidOutSize = Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
	LaidOutRatio = WEMPlayersPrivate::GetFrameRatio();

	if (Players.Num() < 2 || LaidOutSize.X <= 0 || LaidOutSize.Y <= 0)
	{
		return;
	}

	// The engine's split would halve the window; each player gets a portrait frame instead.
	const TArray<FBox2D> Frames = WEMPlayers::GetScreenFrames(FVector2D(LaidOutSize), Players.Num(), LaidOutRatio);

	for (int32 Index = 0; Index < Players.Num() && Index < Frames.Num(); ++Index)
	{
		Players[Index]->Origin = Frames[Index].Min;
		Players[Index]->Size = Frames[Index].GetSize();
	}
}

void UWEMGameViewportClient::Draw(FViewport* InViewport, FCanvas* SceneCanvas)
{
	// The engine lays the players out only as they join or leave; the window or the frame's ratio may have changed since.
	if (InViewport && (InViewport->GetSizeXY() != LaidOutSize || WEMPlayersPrivate::GetFrameRatio() != LaidOutRatio))
	{
		LayoutPlayers();
	}

	Super::Draw(InViewport, SceneCanvas);
}

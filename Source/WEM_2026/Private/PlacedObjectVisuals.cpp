// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacedObjectVisuals.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/StreamableManager.h"
#include "GameFramework/Actor.h"

int32 UPlacedObjectVisuals::Add(
	AActor* Owner,
	USceneComponent* Parent,
	const TSoftObjectPtr<UStaticMesh>& Mesh,
	const FTransform& Transform,
	const bool bLoadSynchronously)
{
	if (!Owner)
	{
		return INDEX_NONE;
	}

	const int32 Handle = NextHandle++;

	// Made at runtime and never saved, and never copied with the placer.
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
		Owner,
		MakeUniqueObjectName(Owner, UStaticMeshComponent::StaticClass(), TEXT("PlacedObject")),
		RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);

	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetupAttachment(Parent);
	Component->RegisterComponent();
	Component->SetWorldTransform(Transform);

	Components.Add(Handle, Component);

	const FSoftObjectPath Path = Mesh.ToSoftObjectPath();
	ComponentMeshes.Add(Handle, Path);

	FMeshUse& Use = MeshUses.FindOrAdd(Path);
	Use.Users.Add(Handle);

	if (!Use.Handle.IsValid() && !Path.IsNull())
	{
		if (bLoadSynchronously || !UAssetManager::IsInitialized())
		{
			Use.Handle = UAssetManager::IsInitialized()
				? UAssetManager::GetStreamableManager().RequestSyncLoad(TArray<FSoftObjectPath>{ Path })
				: nullptr;

			if (!Use.Handle.IsValid())
			{
				Mesh.LoadSynchronous();
			}
		}
		else
		{
			Use.Handle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
				TArray<FSoftObjectPath>{ Path },
				FStreamableDelegate::CreateUObject(this, &UPlacedObjectVisuals::OnMeshLoaded, Path));
		}
	}

	// Loaded already - for another object, or by anything else - so it shows at once.
	if (UStaticMesh* Loaded = Cast<UStaticMesh>(Path.ResolveObject()))
	{
		Component->SetStaticMesh(Loaded);
	}

	return Handle;
}

void UPlacedObjectVisuals::OnMeshLoaded(const FSoftObjectPath Path)
{
	const FMeshUse* Use = MeshUses.Find(Path);
	UStaticMesh* Mesh = Cast<UStaticMesh>(Path.ResolveObject());

	if (!Use || !Mesh)
	{
		return;
	}

	for (const int32 User : Use->Users)
	{
		if (UStaticMeshComponent* Component = Components.FindRef(User))
		{
			Component->SetStaticMesh(Mesh);
		}
	}
}

void UPlacedObjectVisuals::Remove(const int32 Handle)
{
	if (TObjectPtr<UStaticMeshComponent>* Component = Components.Find(Handle))
	{
		if (IsValid(*Component))
		{
			(*Component)->DestroyComponent();
		}

		Components.Remove(Handle);
	}

	FSoftObjectPath Path;
	if (!ComponentMeshes.RemoveAndCopyValue(Handle, Path))
	{
		return;
	}

	FMeshUse* Use = MeshUses.Find(Path);
	if (!Use)
	{
		return;
	}

	Use->Users.Remove(Handle);

	// The last object using a mesh lets go of it, so it can be unloaded.
	if (Use->Users.IsEmpty())
	{
		if (Use->Handle.IsValid())
		{
			Use->Handle->IsLoadingInProgress() ? Use->Handle->CancelHandle() : Use->Handle->ReleaseHandle();
		}

		MeshUses.Remove(Path);
	}
}

void UPlacedObjectVisuals::Reset()
{
	for (const TPair<int32, TObjectPtr<UStaticMeshComponent>>& Entry : Components)
	{
		if (IsValid(Entry.Value))
		{
			Entry.Value->DestroyComponent();
		}
	}

	for (TPair<FSoftObjectPath, FMeshUse>& Entry : MeshUses)
	{
		if (Entry.Value.Handle.IsValid())
		{
			Entry.Value.Handle->IsLoadingInProgress() ? Entry.Value.Handle->CancelHandle() : Entry.Value.Handle->ReleaseHandle();
		}
	}

	Components.Reset();
	ComponentMeshes.Reset();
	MeshUses.Reset();
}

int32 UPlacedObjectVisuals::CountWaiting() const
{
	int32 Waiting = 0;

	for (const TPair<int32, TObjectPtr<UStaticMeshComponent>>& Entry : Components)
	{
		Waiting += IsValid(Entry.Value) && !Entry.Value->GetStaticMesh() ? 1 : 0;
	}

	return Waiting;
}

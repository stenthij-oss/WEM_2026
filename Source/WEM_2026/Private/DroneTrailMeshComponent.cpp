// Copyright Epic Games, Inc. All Rights Reserved.

#include "DroneTrailMeshComponent.h"

#include "Engine/CollisionProfile.h"
#include "LocalVertexFactory.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MeshBatch.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "RHIResourceUtils.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "RenderResource.h"
#include "SceneInterface.h"
#include "SceneManagement.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DroneTrailMeshComponent)

DECLARE_CYCLE_STAT(TEXT("Rewrite growing piece (render thread)"), STAT_DroneTrailSetRings, STATGROUP_DroneTrail);

namespace DroneTrailMeshPrivate
{
	constexpr int32 Sides = UDroneTrailMeshComponent::Sides;
	constexpr int32 MaxRings = UDroneTrailMeshComponent::MaxRings;

	/** Room around the tube for its material, which fattens it far away so it never gets thinner than a pixel or two. */
	constexpr float WidenRoom = 50.0f;

	/** A growing piece's bounds grow by this much at a time, so the renderer hears of it every few metres, not every frame. */
	constexpr float GrowthStep = 500.0f;

	/**
	 * What the trail writes into custom stencil, as the orbs do. The AI overlay paints only where the
	 * stencil holds the pieces' value (wem.AIOverlay.Stencil, 1), so the trail in front of a piece,
	 * writing a value of its own over it, is never painted.
	 */
	constexpr int32 CustomStencilValue = 3;

	/**
	 * The same triangles serve every piece: each ring joined side by side to the next. A piece with
	 * fewer rings draws only the start of them.
	 */
	class FRingIndexBuffer : public FIndexBuffer
	{
	public:
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			TArray<uint16> Indices;
			Indices.Reserve((MaxRings - 1) * Sides * 6);
			for (int32 Ring = 0; Ring < MaxRings - 1; ++Ring)
			{
				for (int32 Side = 0; Side < Sides; ++Side)
				{
					const uint16 A = uint16(Ring * Sides + Side);
					const uint16 B = uint16(Ring * Sides + (Side + 1) % Sides);
					const uint16 C = uint16(A + Sides);
					const uint16 D = uint16(B + Sides);
					Indices.Append({A, B, C, C, B, D});
				}
			}
			IndexBufferRHI = UE::RHIResourceUtils::CreateIndexBufferFromArray(RHICmdList, TEXT("DroneTrailRingIndices"), MakeConstArrayView(Indices));
		}
	};

	TGlobalResource<FRingIndexBuffer> GRingIndexBuffer;

	/**
	 * Draws one piece. A finished piece is built once, to its exact size, and its vertices live only
	 * on the GPU. A growing piece is built for MaxRings with the spare rings folded onto its last one,
	 * keeps a copy of its vertices, and rewrites just the rings that changed, so the triangles it was
	 * first drawn with never change and the renderer's cached draw stays valid.
	 */
	class FDroneTrailSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		FDroneTrailSceneProxy(const UDroneTrailMeshComponent* Component)
			: FPrimitiveSceneProxy(Component)
			, VertexFactory(GetScene().GetFeatureLevel(), "FDroneTrailSceneProxy")
			, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetShaderPlatform()))
			, Origin(Component->GetOrigin())
			, Radius(Component->GetRadius())
			, NumRings(Component->IsGrowing() ? MaxRings : Component->GetRings().Num())
		{
			UMaterialInterface* UsedMaterial = Component->GetMaterial(0);
			Material = UsedMaterial ? UsedMaterial : UMaterial::GetDefaultMaterial(MD_Surface);

			for (int32 Corner = 0; Corner < Sides; ++Corner)
			{
				FMath::SinCos(&Sines[Corner], &Cosines[Corner], UE_TWO_PI * Corner / Sides);
			}

			const bool bGrowing = Component->IsGrowing();
			Positions.Init(NumRings * Sides, bGrowing);
			TangentsAndUVs.Init(NumRings * Sides, 1, bGrowing);

			const TConstArrayView<FDroneTrailRing> Rings = Component->GetRings();
			for (int32 Ring = 0; Ring < NumRings; ++Ring)
			{
				WriteRing(Ring, Rings[FMath::Min(Ring, Rings.Num() - 1)]);
			}

			ENQUEUE_RENDER_COMMAND(InitDroneTrailPiece)([this](FRHICommandListImmediate& RHICmdList)
			{
				Positions.InitResource(RHICmdList);
				TangentsAndUVs.InitResource(RHICmdList);

				FLocalVertexFactory::FDataType Data;
				Positions.BindPositionVertexBuffer(&VertexFactory, Data);
				TangentsAndUVs.BindTangentVertexBuffer(&VertexFactory, Data);
				TangentsAndUVs.BindPackedTexCoordVertexBuffer(&VertexFactory, Data);
				TangentsAndUVs.BindLightMapVertexBuffer(&VertexFactory, Data, 0);
				FColorVertexBuffer::BindDefaultColorVertexBuffer(&VertexFactory, Data, FColorVertexBuffer::NullBindStride::ZeroForDefaultBufferBind);
				VertexFactory.SetData(RHICmdList, Data);
				VertexFactory.InitResource(RHICmdList);
			});
		}

		virtual ~FDroneTrailSceneProxy() override
		{
			Positions.ReleaseResource();
			TangentsAndUVs.ReleaseResource();
			VertexFactory.ReleaseResource();
		}

		/** Rewrites rings from FirstRing on, in the copy and on the GPU. Only for a growing piece. */
		void SetRings_RenderThread(FRHICommandListBase& RHICmdList, const int32 FirstRing, const TArray<FDroneTrailRing>& Rings)
		{
			SCOPE_CYCLE_COUNTER(STAT_DroneTrailSetRings);

			const int32 EndRing = FMath::Min(FirstRing + Rings.Num(), NumRings);
			if (FirstRing >= EndRing || !Positions.GetVertexData())
			{
				return;
			}
			for (int32 Ring = FirstRing; Ring < EndRing; ++Ring)
			{
				WriteRing(Ring, Rings[Ring - FirstRing]);
			}

			const uint32 FirstVertex = FirstRing * Sides;
			const uint32 NumVertices = (EndRing - FirstRing) * Sides;
			Upload(RHICmdList, Positions.VertexBufferRHI, Positions.GetVertexData(), Positions.GetStride(), FirstVertex, NumVertices);
			Upload(RHICmdList, TangentsAndUVs.TangentsVertexBuffer.VertexBufferRHI, TangentsAndUVs.GetTangentData(),
				TangentsAndUVs.GetTangentSize() / TangentsAndUVs.GetNumVertices(), FirstVertex, NumVertices);
		}

		virtual void DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) override
		{
			FMeshBatch Mesh;
			Mesh.VertexFactory = &VertexFactory;
			Mesh.MaterialRenderProxy = Material->GetRenderProxy();
			Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.MeshIdInPrimitive = 0;
			Mesh.LODIndex = 0;
			Mesh.SegmentIndex = 0;
			Mesh.CastShadow = false;

			FMeshBatchElement& Element = Mesh.Elements[0];
			Element.IndexBuffer = &GRingIndexBuffer;
			Element.FirstIndex = 0;
			Element.NumPrimitives = (NumRings - 1) * Sides * 2;
			Element.MinVertexIndex = 0;
			Element.MaxVertexIndex = NumRings * Sides - 1;

			PDI->DrawMesh(Mesh, FLT_MAX);
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = IsShown(View);
			Result.bShadowRelevance = IsShadowCast(View);
			Result.bRenderInMainPass = ShouldRenderInMainPass();
			Result.bRenderCustomDepth = ShouldRenderCustomDepth();
			Result.bStaticRelevance = true;
			MaterialRelevance.SetPrimitiveViewRelevance(Result);

			// The pieces never move. The material's world position offset would otherwise draw every
			// piece again into the velocity pass each frame, for a widening too small to smear.
			Result.bVelocityRelevance = false;
			return Result;
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		virtual uint32 GetMemoryFootprint() const override
		{
			return sizeof(*this) + GetAllocatedSize();
		}

	private:
		void WriteRing(const int32 Index, const FDroneTrailRing& Ring)
		{
			// A cap ring sits one radius along the path and closes to a point.
			const FVector3f Side = FVector3f::CrossProduct(Ring.Normal, Ring.Tangent);
			const FVector3f Center = Ring.Position - Origin + Ring.Tangent * (Ring.Cap * Radius);
			const float RingRadius = Ring.Cap == 0.0f ? Radius : 0.0f;

			for (int32 Corner = 0; Corner < Sides; ++Corner)
			{
				const FVector3f Out = Ring.Normal * Cosines[Corner] + Side * Sines[Corner];
				const int32 Vertex = Index * Sides + Corner;
				Positions.VertexPosition(Vertex) = Center + Out * RingRadius;
				TangentsAndUVs.SetVertexTangents(Vertex, Ring.Tangent, FVector3f::CrossProduct(Out, Ring.Tangent), Out);
				TangentsAndUVs.SetVertexUV(Vertex, 0, FVector2f(float(Corner) / Sides, 0.0f));
			}
		}

		static void Upload(FRHICommandListBase& RHICmdList, FRHIBuffer* Buffer, const void* Data, const uint32 Stride, const uint32 FirstVertex, const uint32 NumVertices)
		{
			void* Locked = RHICmdList.LockBuffer(Buffer, FirstVertex * Stride, NumVertices * Stride, RLM_WriteOnly);
			FMemory::Memcpy(Locked, static_cast<const uint8*>(Data) + FirstVertex * Stride, NumVertices * Stride);
			RHICmdList.UnlockBuffer(Buffer);
		}

		FLocalVertexFactory VertexFactory;
		FPositionVertexBuffer Positions;
		FStaticMeshVertexBuffer TangentsAndUVs;
		UMaterialInterface* Material = nullptr;
		FMaterialRelevance MaterialRelevance;
		FVector3f Origin;
		float Radius;
		int32 NumRings;
		float Sines[Sides];
		float Cosines[Sides];
	};
}

UDroneTrailMeshComponent::UDroneTrailMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	CanCharacterStepUpOn = ECB_No;

	// A light, not a surface: no shadow, and nothing for Lumen, distance fields or ray tracing to keep up to date.
	CastShadow = false;
	bReceivesDecals = false;
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;
	bVisibleInRayTracing = false;
	bUseAsOccluder = false;
	SetRenderCustomDepth(true);
	SetCustomDepthStencilValue(DroneTrailMeshPrivate::CustomStencilValue);

	// Pieces stay where they were laid, whatever the drone does.
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);
}

void UDroneTrailMeshComponent::Begin(TArray<FDroneTrailRing>&& InRings, const float InRadius)
{
	check(InRings.Num() >= 2 && InRings.Num() <= MaxRings);

	Rings = MoveTemp(InRings);
	Radius = InRadius;
	bGrowing = true;
	Origin = Rings[0].Position;
	SetWorldLocation(FVector(Origin));

	LocalBox = FBox3f(ForceInit);
	for (const FDroneTrailRing& Ring : Rings)
	{
		LocalBox += Ring.Position - Origin;
	}
}

void UDroneTrailMeshComponent::SetRings(const int32 FirstRing, const TConstArrayView<FDroneTrailRing> NewRings)
{
	check(bGrowing && FirstRing >= 0 && FirstRing <= Rings.Num() && FirstRing + NewRings.Num() <= MaxRings);

	Rings.SetNum(FMath::Max(Rings.Num(), FirstRing + NewRings.Num()));
	bool bBoundsGrew = false;
	for (int32 Index = 0; Index < NewRings.Num(); ++Index)
	{
		Rings[FirstRing + Index] = NewRings[Index];
		const FVector3f Position = NewRings[Index].Position - Origin;
		if (!LocalBox.IsInsideOrOn(Position))
		{
			LocalBox += FBox3f(Position - FVector3f(DroneTrailMeshPrivate::GrowthStep), Position + FVector3f(DroneTrailMeshPrivate::GrowthStep));
			bBoundsGrew = true;
		}
	}

	if (SceneProxy)
	{
		DroneTrailMeshPrivate::FDroneTrailSceneProxy* Proxy = static_cast<DroneTrailMeshPrivate::FDroneTrailSceneProxy*>(SceneProxy);
		ENQUEUE_RENDER_COMMAND(SetDroneTrailRings)([Proxy, FirstRing, Copy = TArray<FDroneTrailRing>(NewRings)](FRHICommandListImmediate& RHICmdList)
		{
			Proxy->SetRings_RenderThread(RHICmdList, FirstRing, Copy);
		});
	}

	if (bBoundsGrew)
	{
		UpdateBounds();
		MarkRenderTransformDirty();
	}
}

void UDroneTrailMeshComponent::Finish(const int32 NumRings)
{
	check(bGrowing && NumRings >= 2 && NumRings <= Rings.Num());

	bGrowing = false;
	Rings.SetNum(NumRings);
	Rings.Shrink();

	LocalBox = FBox3f(ForceInit);
	for (const FDroneTrailRing& Ring : Rings)
	{
		LocalBox += Ring.Position - Origin;
	}
	UpdateBounds();
	MarkRenderStateDirty();
}

void UDroneTrailMeshComponent::SetRadius(const float InRadius)
{
	if (Radius != InRadius)
	{
		Radius = InRadius;
		UpdateBounds();
		MarkRenderStateDirty();
	}
}

FPrimitiveSceneProxy* UDroneTrailMeshComponent::CreateSceneProxy()
{
	return Rings.Num() >= 2 ? new DroneTrailMeshPrivate::FDroneTrailSceneProxy(this) : nullptr;
}

FBoxSphereBounds UDroneTrailMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!LocalBox.IsValid)
	{
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0);
	}
	return FBoxSphereBounds(FBox(LocalBox).ExpandBy(Radius + DroneTrailMeshPrivate::WidenRoom)).TransformBy(LocalToWorld);
}

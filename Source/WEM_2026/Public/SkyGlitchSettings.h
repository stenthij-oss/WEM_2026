// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "SkyGlitchSettings.generated.h"

/**
 * The sky glitch's and the portrait frame's settings as checkboxes and sliders: place one in the
 * level. It sets the wem.SkyGlitch and wem.Portrait console variables when play begins and whenever
 * a setting is edited, so both can be tuned live by selecting this in the Outliner while playing.
 * The effects themselves are described in Structures/SKY_GLITCH.md and Structures/PORTRAIT.md.
 */
UCLASS(hidecategories = (Actor, Cooking, Input, LOD, Networking, Physics, Replication))
class WEM_2026_API ASkyGlitchSettings : public AInfo
{
	GENERATED_BODY()

public:
	/** The whole effect, on or off. Off draws the scene as normal; on again starts over a clean sky. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Glitch")
	bool bEnabled = true;

	/** Draws the sky pure black, so the trails pile up on black instead of on the live sky. The sky still lights the platforms. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Glitch")
	bool bBlackSky = false;

	/** Seconds between the copies the platforms leave. 0 leaves one every frame; more spaces them out into separate outlines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Glitch", meta = (ClampMin = "0.0", UIMax = "0.5", Units = "s"))
	float CopyInterval = 0.0f;

	/** Anything farther from the camera than this counts as sky and takes trails. Everything nearer always draws live. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Glitch", meta = (ClampMin = "100.0", UIMin = "1000.0", UIMax = "500000.0", Units = "cm"))
	float SkyDistance = 100000.0f;

	/** Seconds the trails build up before they are wiped. 0 never wipes them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Glitch|Cycle", meta = (ClampMin = "0.0", UIMax = "60.0", Units = "s"))
	float ResetAfter = 0.0f;

	/** Seconds the effect stays off after a wipe, before it turns back on over a clean sky. 0 turns it straight back on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Glitch|Cycle", meta = (ClampMin = "0.0", UIMax = "60.0", Units = "s"))
	float TurnOnAfter = 0.0f;

	/** Width of the portrait frame for a height of 1. 0.5625 is 9:16; a taller phone, 9:19.5, is 0.4615. P switches portrait on and off while playing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portrait", meta = (ClampMin = "0.1", UIMin = "0.4", UIMax = "1.0"))
	float FrameRatio = 0.5625f;

	/** Multiplies the lens's focal length in the portrait frame. Below 1 is wider, above 1 narrower. The zoom still works on top. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portrait", meta = (ClampMin = "0.25", UIMin = "0.5", UIMax = "2.0"))
	float FocalLengthScale = 1.0f;

	/** How strong the whole phone look is: 0 a clean camera, 1 full, 2 twice as far. The sliders below work on top of it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portrait|Phone Look", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float PhoneLook = 1.0f;

	/** How hard edges are sharpened: the bright and dark halos along them grow with it. 0 none, 2 twice as much. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portrait|Phone Look", meta = (ClampMin = "0.0", UIMax = "3.0"))
	float Sharpen = 1.0f;

	/** How far the red and blue colour fringes spread towards the frame's edges. 0 none, 2 twice as far. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portrait|Phone Look", meta = (ClampMin = "0.0", UIMax = "3.0", DisplayName = "Colour Fringes"))
	float Fringes = 1.0f;

	/** How much the hand holding the phone moves. 0 holds it still. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portrait|Phone Look", meta = (ClampMin = "0.0", UIMax = "3.0"))
	float HandShake = 1.0f;

	/** Seconds the phone takes to read its sensor from the top row down: turning leans uprights, tilting stretches the picture. Real phones take 0.01 to 0.03. 0 turns it off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portrait|Phone Look", meta = (ClampMin = "0.0", UIMax = "0.3", Units = "s"))
	float RollingShutter = 0.08f;

	/** Sets the console variables to these settings. Play and every edit do this already. */
	UFUNCTION(BlueprintCallable, Category = "Sky Glitch")
	void Apply() const;

	/** Wipes the trails from the sky now and starts the Reset After clock over. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Sky Glitch")
	void ClearTrails();

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};

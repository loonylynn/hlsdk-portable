/*
 * Shared sprite-HUD digital readout helpers.
 *
 * The existing number_8 sprite is drawn at low intensity as a permanent
 * ghost guide behind the live value, like inactive segments on a digital display.
 */
#ifndef HUD_DIGITAL_H
#define HUD_DIGITAL_H

#include "hud.h"
#include "cl_util.h"

// Exposed tuning constants for the classic sprite HUD digital readouts.
static const int HUD_DIGITAL_DIM_ALPHA = 48;
static const int HUD_DIGITAL_FLASH_ALPHA = 255;
static const float HUD_DIGITAL_FADE_DURATION = 0.45f;
static const int HUD_DIGITAL_LOW_HEALTH_THRESHOLD = 15;
static const int HUD_DIGITAL_GHOST_DIGIT_SPACING = 0;

static inline int HUD_DigitalGetFlashAlpha( float &timeRemaining, float frameTime )
{
	if( timeRemaining <= 0.0f )
		return HUD_DIGITAL_DIM_ALPHA;

	if( timeRemaining > HUD_DIGITAL_FADE_DURATION )
		timeRemaining = HUD_DIGITAL_FADE_DURATION;

	const float fraction = timeRemaining / HUD_DIGITAL_FADE_DURATION;
	const int alpha = HUD_DIGITAL_DIM_ALPHA + (int)(
		(float)( HUD_DIGITAL_FLASH_ALPHA - HUD_DIGITAL_DIM_ALPHA ) * fraction + 0.5f );

	// Return full flash brightness on the first frame after a value update,
	// then consume the elapsed frame time for the next draw.
	if( frameTime > 0.0f )
		timeRemaining -= frameTime;

	if( timeRemaining < 0.0f )
		timeRemaining = 0.0f;

	return alpha;
}

static inline void HUD_DigitalScaleColor( int &red, int &green, int &blue, int alpha )
{
	if( alpha < 0 ) alpha = 0;
	if( alpha > 255 ) alpha = 255;

	red = ( red * alpha ) / 255;
	green = ( green * alpha ) / 255;
	blue = ( blue * alpha ) / 255;
}

static inline void HUD_DigitalDrawDigit( int digit, int x, int y,
	int red, int green, int blue )
{
	if( digit < 0 || digit > 9 )
		return;

	const int spriteIndex = gHUD.m_HUD_number_0 + digit;
	HSPRITE sprite = gHUD.GetSprite( spriteIndex );
	if( !sprite )
		return;

	SPR_Set( sprite, red, green, blue );
	SPR_DrawAdditive( 0, x, y, &gHUD.GetSpriteRect( spriteIndex ) );
}

static inline void HUD_DigitalDrawGhostSlots( int x, int y,
	int red, int green, int blue, int slotStep )
{
	int dull1Index = gHUD.GetSpriteIndex( "number_dull1" );
	int dull2Index = gHUD.GetSpriteIndex( "number_dull2" );

	int ghostRed = red;
	int ghostGreen = green;
	int ghostBlue = blue;
	HUD_DigitalScaleColor( ghostRed, ghostGreen, ghostBlue, HUD_DIGITAL_DIM_ALPHA );

	// The 640 HUD atlas provides two dedicated unlit digit sprites.
	// Match the stock three-slot pattern: dull1, dull2, dull1.
	if( dull1Index >= 0 && dull2Index >= 0 &&
		gHUD.GetSprite( dull1Index ) && gHUD.GetSprite( dull2Index ) )
	{
		SPR_Set( gHUD.GetSprite( dull1Index ), ghostRed, ghostGreen, ghostBlue );
		SPR_DrawAdditive( 0, x, y, &gHUD.GetSpriteRect( dull1Index ) );

		SPR_Set( gHUD.GetSprite( dull2Index ), ghostRed, ghostGreen, ghostBlue );
		SPR_DrawAdditive( 0, x + slotStep, y, &gHUD.GetSpriteRect( dull2Index ) );

		SPR_Set( gHUD.GetSprite( dull1Index ), ghostRed, ghostGreen, ghostBlue );
		SPR_DrawAdditive( 0, x + 2 * slotStep, y, &gHUD.GetSpriteRect( dull1Index ) );
		return;
	}

	// The 320 HUD atlas does not define number_dull1/2. Retain a
	// low-intensity number_8 fallback there so the three ghost positions
	// remain present at both HUD resolutions.
	const int fallbackIndex = gHUD.m_HUD_number_0 + 8;
	HSPRITE fallbackSprite = gHUD.GetSprite( fallbackIndex );
	if( fallbackSprite )
	{
		SPR_Set( fallbackSprite, ghostRed, ghostGreen, ghostBlue );
		for( int slot = 0; slot < 3; ++slot )
			SPR_DrawAdditive( 0, x + slot * slotStep, y,
				&gHUD.GetSpriteRect( fallbackIndex ) );
	}
}

static inline void HUD_DigitalDrawNumber( int x, int y, int value,
	int red, int green, int blue, int alpha )
{
	const int numberBase = gHUD.m_HUD_number_0;
	const int digitWidth = gHUD.GetSpriteRect( numberBase ).right -
		gHUD.GetSpriteRect( numberBase ).left;
	const int slotStep = digitWidth + HUD_DIGITAL_GHOST_DIGIT_SPACING;

	if( digitWidth <= 0 )
		return;

	HUD_DigitalDrawGhostSlots( x, y, red, green, blue, slotStep );

	// Overlay only the significant digits, aligned to the right. Unused
	// leading slots remain visible as dim ghosts instead of leading zeroes.
	int liveRed = red;
	int liveGreen = green;
	int liveBlue = blue;
	HUD_DigitalScaleColor( liveRed, liveGreen, liveBlue, alpha );

	if( value < 0 ) value = 0;
	if( value > 999 ) value = 999;

	if( value >= 100 )
		HUD_DigitalDrawDigit( value / 100, x, y, liveRed, liveGreen, liveBlue );

	if( value >= 10 )
		HUD_DigitalDrawDigit( ( value / 10 ) % 10, x + slotStep, y,
			liveRed, liveGreen, liveBlue );

	HUD_DigitalDrawDigit( value % 10, x + 2 * slotStep, y,
		liveRed, liveGreen, liveBlue );
}

#endif // HUD_DIGITAL_H

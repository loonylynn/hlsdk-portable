/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
//
// battery.cpp
//
// implementation of CHudBattery class
//

#include "hud.h"
#include "cl_util.h"
#include "hud_digital.h"
#include "hud_vgui.h"
#include "parsemsg.h"

#include <string.h>
#include <stdio.h>

DECLARE_MESSAGE( m_Battery, Battery )

int CHudBattery::Init( void )
{
	m_iBat = 0;
	m_fFade = 0;
	m_flDigitalFlashRemaining = 0.0f;
	m_iFlags = 0;

	HOOK_MESSAGE( Battery );

	gHUD.AddHudElem( this );

	return 1;
}

int CHudBattery::VidInit( void )
{
	int HUD_suit_empty = gHUD.GetSpriteIndex( "suit_empty" );
	int HUD_suit_full = gHUD.GetSpriteIndex( "suit_full" );

	m_hSprite1 = m_hSprite2 = 0;  // delaying get sprite handles until we know the sprites are loaded
	m_prc1 = &gHUD.GetSpriteRect( HUD_suit_empty );
	m_prc2 = &gHUD.GetSpriteRect( HUD_suit_full );
	m_iHeight = m_prc2->bottom - m_prc1->top;
	m_fFade = 0;
	m_flDigitalFlashRemaining = 0.0f;
	return 1;
}

int CHudBattery::MsgFunc_Battery( const char *pszName,  int iSize, void *pbuf )
{
	m_iFlags |= HUD_ACTIVE;

	BEGIN_READ( pbuf, iSize );
	int x = READ_SHORT();

	if( x != m_iBat )
	{
		m_fFade = FADE_TIME;
		m_flDigitalFlashRemaining = HUD_DIGITAL_FADE_DURATION;
		m_iBat = x;
	}

	return 1;
}

int CHudBattery::Draw( float flTime )
{
	if( ( gHUD.m_iHideHUDDisplay & ( HIDEHUD_HEALTH | HIDEHUD_ALL ) ) || gEngfuncs.IsSpectateOnly() )
	{
		HUD_VGuiSuitSetVisible( 0 );
		return 1;
	}

	int r, g, b, x, y, a;
	wrect_t rc;

	UnpackRGB( r, g, b, RGB_YELLOWISH );

	if( !( gHUD.m_iWeaponBits & ( 1 << ( WEAPON_SUIT ) ) ) )
	{
		HUD_VGuiSuitSetVisible( 0 );
		return 1;
	}

	// Preserve the original fade for the optional VGUI HUD.
	if( m_fFade )
	{
		if( m_fFade > FADE_TIME )
			m_fFade = FADE_TIME;

		m_fFade -= ( (float)gHUD.m_flTimeDelta * 20.0f );
		if( m_fFade <= 0 )
		{
			a = 128;
			m_fFade = 0;
		}

		a = MIN_ALPHA + ( m_fFade / FADE_TIME ) * 128;
	}
	else
		a = MIN_ALPHA;

	ScaleColors( r, g, b, a );

	y = ScreenHeight - gHUD.m_iFontHeight - gHUD.m_iFontHeight / 2;

	// Retain the existing VGUI panel anchor. This change only alters the
	// classic sprite HUD layout.
	int vguiX;
	if( gHUD.IsHL25() )
		vguiX = ( m_prc1->right - m_prc1->left ) * 3;
	else
		vguiX = ScreenWidth / 5;

	const int digitalAlpha = HUD_DigitalGetFlashAlpha(
		m_flDigitalFlashRemaining, (float)gHUD.m_flTimeDelta );

	if( CVAR_GET_FLOAT( "hud_vgui" ) != 0.0f &&
		HUD_VGuiSuitUpdate( m_iBat, r, g, b,
			vguiX + ( m_prc1->right - m_prc1->left ),
			y + gHUD.m_iHudNumbersYOffset ) )
		return 1;

	HUD_VGuiSuitSetVisible( 0 );

	// Make sure we have the right sprite handles.
	if( !m_hSprite1 )
		m_hSprite1 = gHUD.GetSprite( gHUD.GetSpriteIndex( "suit_empty" ) );
	if( !m_hSprite2 )
		m_hSprite2 = gHUD.GetSprite( gHUD.GetSpriteIndex( "suit_full" ) );

	int batteryValue = m_iBat;
	if( batteryValue < 0 ) batteryValue = 0;
	if( batteryValue > 100 ) batteryValue = 100;

	// The full suit sprite is cropped from the top so only the filled
	// fraction matching the current battery value is drawn over the outline.
	rc = *m_prc2;
	rc.top += (int)( (float)m_iHeight * ( (float)( 100 - batteryValue ) * 0.01f ) );

	// Position the suit readout immediately to the right of the health slots.
	const int digitWidth = gHUD.GetSpriteRect( gHUD.m_HUD_number_0 ).right -
		gHUD.GetSpriteRect( gHUD.m_HUD_number_0 ).left;
	const int crossWidth = gHUD.GetSpriteRect( gHUD.m_Health.m_HUD_cross ).right -
		gHUD.GetSpriteRect( gHUD.m_Health.m_HUD_cross ).left;
	const int healthNumbersX = crossWidth + digitWidth / 2;
	const int healthSlotsWidth = 3 * digitWidth +
		2 * HUD_DIGITAL_GHOST_DIGIT_SPACING;
	x = healthNumbersX + healthSlotsWidth + digitWidth / 2;

	const int iOffset = ( m_prc1->bottom - m_prc1->top ) / 6;

	int displayR, displayG, displayB;
	UnpackRGB( displayR, displayG, displayB, RGB_YELLOWISH );

	int iconR = displayR;
	int iconG = displayG;
	int iconB = displayB;
	HUD_DigitalScaleColor( iconR, iconG, iconB, digitalAlpha );

	SPR_Set( m_hSprite1, iconR, iconG, iconB );
	SPR_DrawAdditive( 0, x, y - iOffset, m_prc1 );

	if( rc.bottom > rc.top )
	{
		SPR_Set( m_hSprite2, iconR, iconG, iconB );
		SPR_DrawAdditive( 0, x, y - iOffset + ( rc.top - m_prc2->top ), &rc );
	}

	const int iconWidth = m_prc1->right - m_prc1->left;
	const int numberX = x + iconWidth + digitWidth / 2;
	HUD_DigitalDrawNumber( numberX, y + gHUD.m_iHudNumbersYOffset,
		batteryValue, displayR, displayG, displayB, digitalAlpha );

	return 1;
}

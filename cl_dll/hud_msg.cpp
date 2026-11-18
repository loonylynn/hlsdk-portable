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
//  hud_msg.cpp
//

#include "hud.h"
#include "cl_util.h"
#include "game_instructor.h"
#include "parsemsg.h"
#include "r_efx.h"

#include <stdio.h>

#define MAX_CLIENTS 32

extern BEAM *pBeam;
extern BEAM *pBeam2;
extern TEMPENTITY *pFlare;	// Vit_amiN

extern float g_lastFOV;			// Vit_amiN

/// USER-DEFINED SERVER MESSAGE HANDLERS

// Convert a Source locator icon identifier into the equivalent optional
// GoldSrc sprite path. Only simple filename characters are accepted.
static HSPRITE HUD_LoadInstructorHintSprite( const char *iconName )
{
	if( !iconName || !iconName[0] )
		return 0;

	char path[96];
	int out = 0;
	for( int i = 0; iconName[i] && out < 60; ++i )
	{
		const char c = iconName[i];
		if( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ||
			( c >= '0' && c <= '9' ) || c == '_' || c == '-' )
			path[out++] = c;
		else
			return 0;
	}
	path[out] = '\0';
	if( out == 0 )
		return 0;

	char spritePath[96];
	snprintf( spritePath, sizeof( spritePath ), "sprites/%s.spr", path );
	return SPR_Load( spritePath );
}

static void HUD_ClearInstructorHint( CHud *hud )
{
	hud->m_bInstructorHintActive = false;
	hud->m_bInstructorHintManaged = false;
	hud->m_bInstructorHintFixedPosition = false;
	hud->m_flInstructorHintFixedX = 0.5f;
	hud->m_flInstructorHintFixedY = 0.5f;
	hud->m_flInstructorManagedVisibleSince = 0.0f;
	hud->m_iInstructorHintId = 0;
	hud->m_iInstructorHintTarget = 0;
	hud->m_bInstructorHintHasTargetPosition = false;
	hud->m_bInstructorHintTargetVisible = false;
	hud->m_bInstructorHintOccluded = false;
	hud->m_vecInstructorHintTargetOrigin[0] = 0.0f;
	hud->m_vecInstructorHintTargetOrigin[1] = 0.0f;
	hud->m_vecInstructorHintTargetOrigin[2] = 0.0f;
	hud->m_flInstructorHintEndTime = 0.0f;
	hud->m_flInstructorHintRange = 0.0f;
	hud->m_flInstructorHintIconOffset = 0.0f;
	hud->m_iInstructorHintIconMode = 0;
	hud->m_iInstructorHintFlags = 0;
	hud->m_iInstructorHintColorR = 255;
	hud->m_iInstructorHintColorG = 255;
	hud->m_iInstructorHintColorB = 255;
	hud->m_hInstructorHintOnscreenSprite = 0;
	hud->m_hInstructorHintOffscreenSprite = 0;
	hud->m_szInstructorHintIconOnscreen[0] = '\0';
	hud->m_szInstructorHintIconOffscreen[0] = '\0';
	hud->m_szInstructorHintName[0] = '\0';
	hud->m_szInstructorHintReplaceKey[0] = '\0';
	hud->m_szInstructorHintCaption[0] = '\0';
	hud->m_szInstructorHintBinding[0] = '\0';
}

int CHud::MsgFunc_InstructorHint( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );

	const int action = READ_BYTE();
	const int hintId = READ_SHORT();

	if( action == 0 )
	{
		if( m_bInstructorHintActive && hintId == m_iInstructorHintId )
			HUD_ClearInstructorHint( this );
		return 1;
	}

	if( action == 2 )
	{
		const int targetIndex = READ_SHORT();
		const float x = READ_COORD();
		const float y = READ_COORD();
		const float z = READ_COORD();
		const int iconMode = READ_BYTE();
		const bool occluded = READ_BYTE() != 0;
		const bool targetVisible = READ_BYTE() != 0;
		char captionUpdate[192];
		char bindingUpdate[64];
		const char *readCaptionUpdate = READ_STRING();
		strncpy( captionUpdate, readCaptionUpdate ? readCaptionUpdate : "", sizeof( captionUpdate ) - 1 );
		captionUpdate[sizeof( captionUpdate ) - 1] = '\0';
		const char *readBindingUpdate = READ_STRING();
		strncpy( bindingUpdate, readBindingUpdate ? readBindingUpdate : "", sizeof( bindingUpdate ) - 1 );
		bindingUpdate[sizeof( bindingUpdate ) - 1] = '\0';

		if( m_bInstructorHintActive && hintId == m_iInstructorHintId )
		{
			m_iInstructorHintTarget = targetIndex;
			m_bInstructorHintHasTargetPosition =
				( targetIndex > 0 ) && !( m_iInstructorHintFlags & 0x00000100 );
			m_vecInstructorHintTargetOrigin[0] = x;
			m_vecInstructorHintTargetOrigin[1] = y;
			m_vecInstructorHintTargetOrigin[2] = z;
			m_iInstructorHintIconMode = iconMode;
			m_bInstructorHintOccluded = occluded;
			m_bInstructorHintTargetVisible = targetVisible;

			// Empty strings mean "retain the map-authored caption/binding".
			if( captionUpdate && captionUpdate[0] )
			{
				strncpy( m_szInstructorHintCaption, captionUpdate, sizeof( m_szInstructorHintCaption ) - 1 );
				m_szInstructorHintCaption[sizeof( m_szInstructorHintCaption ) - 1] = '\0';
			}
			if( bindingUpdate && bindingUpdate[0] )
			{
				strncpy( m_szInstructorHintBinding, bindingUpdate, sizeof( m_szInstructorHintBinding ) - 1 );
				m_szInstructorHintBinding[sizeof( m_szInstructorHintBinding ) - 1] = '\0';
			}
			if( hintId == -1 )
			{
				const char *caption = iconMode == 2 ?
					"Hold USE and move backward to pull" : "Move into it to push";
				const char *binding = iconMode == 2 ? "USE" : "";
				strncpy( m_szInstructorHintCaption, caption, sizeof( m_szInstructorHintCaption ) - 1 );
				m_szInstructorHintCaption[sizeof( m_szInstructorHintCaption ) - 1] = '\0';
				strncpy( m_szInstructorHintBinding, binding, sizeof( m_szInstructorHintBinding ) - 1 );
				m_szInstructorHintBinding[sizeof( m_szInstructorHintBinding ) - 1] = '\0';
			}
		}
		return 1;
	}

	if( action != 1 )
		return 0;

	const int targetIndex = READ_SHORT();
	const int timeoutTenths = READ_SHORT();
	const float x = READ_COORD();
	const float y = READ_COORD();
	const float z = READ_COORD();
	char caption[192], binding[64];
	char iconOnscreen[64], iconOffscreen[64];
	char hintName[64], replaceKey[64];
	const char *readString = READ_STRING();
	strncpy( caption, readString ? readString : "", sizeof( caption ) - 1 );
	caption[sizeof( caption ) - 1] = '\0';
	readString = READ_STRING();
	strncpy( binding, readString ? readString : "", sizeof( binding ) - 1 );
	binding[sizeof( binding ) - 1] = '\0';
	const int iconMode = READ_BYTE();
	readString = READ_STRING();
	strncpy( iconOnscreen, readString ? readString : "", sizeof( iconOnscreen ) - 1 );
	iconOnscreen[sizeof( iconOnscreen ) - 1] = '\0';
	readString = READ_STRING();
	strncpy( iconOffscreen, readString ? readString : "", sizeof( iconOffscreen ) - 1 );
	iconOffscreen[sizeof( iconOffscreen ) - 1] = '\0';
	const int colorR = READ_BYTE();
	const int colorG = READ_BYTE();
	const int colorB = READ_BYTE();
	const int flags = READ_SHORT();
	const float range = READ_COORD();
	const bool occluded = READ_BYTE() != 0;
	const bool targetVisible = READ_BYTE() != 0;
	readString = READ_STRING();
	strncpy( hintName, readString ? readString : "", sizeof( hintName ) - 1 );
	hintName[sizeof( hintName ) - 1] = '\0';
	readString = READ_STRING();
	strncpy( replaceKey, readString ? readString : "", sizeof( replaceKey ) - 1 );
	replaceKey[sizeof( replaceKey ) - 1] = '\0';

	// Server-authored hints have precedence over scripted client lessons.
	// The manager resumes once this explicit hint ends or times out.
	m_bInstructorHintManaged = false;
	m_flInstructorManagedVisibleSince = 0.0f;

	// A new Source hint replaces the currently displayed hint. This is the
	// single-panel equivalent of the game's replace-key/lesson arbitration.
	m_iInstructorHintId = hintId;
	m_iInstructorHintTarget = targetIndex;
	m_iInstructorHintFlags = flags;
	m_bInstructorHintHasTargetPosition =
		( targetIndex > 0 ) && !( flags & 0x00000100 );
	m_bInstructorHintOccluded = occluded;
	m_bInstructorHintTargetVisible = targetVisible;
	m_vecInstructorHintTargetOrigin[0] = x;
	m_vecInstructorHintTargetOrigin[1] = y;
	m_vecInstructorHintTargetOrigin[2] = z;
	m_flInstructorHintEndTime = timeoutTenths > 0 ? m_flTime + ( timeoutTenths * 0.1f ) : 0.0f;
	m_flInstructorHintRange = range;
	m_iInstructorHintIconMode = iconMode;
	m_iInstructorHintColorR = colorR;
	m_iInstructorHintColorG = colorG;
	m_iInstructorHintColorB = colorB;

	strncpy( m_szInstructorHintCaption, caption ? caption : "", sizeof( m_szInstructorHintCaption ) - 1 );
	m_szInstructorHintCaption[sizeof( m_szInstructorHintCaption ) - 1] = '\0';
	strncpy( m_szInstructorHintBinding, binding ? binding : "", sizeof( m_szInstructorHintBinding ) - 1 );
	m_szInstructorHintBinding[sizeof( m_szInstructorHintBinding ) - 1] = '\0';
	strncpy( m_szInstructorHintIconOnscreen, iconOnscreen ? iconOnscreen : "", sizeof( m_szInstructorHintIconOnscreen ) - 1 );
	m_szInstructorHintIconOnscreen[sizeof( m_szInstructorHintIconOnscreen ) - 1] = '\0';
	strncpy( m_szInstructorHintIconOffscreen, iconOffscreen ? iconOffscreen : "", sizeof( m_szInstructorHintIconOffscreen ) - 1 );
	m_szInstructorHintIconOffscreen[sizeof( m_szInstructorHintIconOffscreen ) - 1] = '\0';
	strncpy( m_szInstructorHintName, hintName ? hintName : "", sizeof( m_szInstructorHintName ) - 1 );
	m_szInstructorHintName[sizeof( m_szInstructorHintName ) - 1] = '\0';
	strncpy( m_szInstructorHintReplaceKey, replaceKey ? replaceKey : "", sizeof( m_szInstructorHintReplaceKey ) - 1 );
	m_szInstructorHintReplaceKey[sizeof( m_szInstructorHintReplaceKey ) - 1] = '\0';

	m_hInstructorHintOnscreenSprite = HUD_LoadInstructorHintSprite( m_szInstructorHintIconOnscreen );
	m_hInstructorHintOffscreenSprite = HUD_LoadInstructorHintSprite( m_szInstructorHintIconOffscreen );

	if( hintId == -1 )
	{
		const char *pushableCaption = iconMode == 2 ?
			"Hold USE and move backward to pull" : "Move into it to push";
		const char *pushableBinding = iconMode == 2 ? "USE" : "";
		strncpy( m_szInstructorHintCaption, pushableCaption, sizeof( m_szInstructorHintCaption ) - 1 );
		m_szInstructorHintCaption[sizeof( m_szInstructorHintCaption ) - 1] = '\0';
		strncpy( m_szInstructorHintBinding, pushableBinding, sizeof( m_szInstructorHintBinding ) - 1 );
		m_szInstructorHintBinding[sizeof( m_szInstructorHintBinding ) - 1] = '\0';
	}

	m_bInstructorHintActive = ( m_szInstructorHintCaption[0] != '\0' ||
		m_szInstructorHintBinding[0] != '\0' ||
		m_hInstructorHintOnscreenSprite || m_hInstructorHintOffscreenSprite );
	return 1;
}
int CHud::MsgFunc_InstructorEvent( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );

	InstructorEventData event;
	memset( &event, 0, sizeof( event ) );

	const char *readString = READ_STRING();
	strncpy( event.name, readString ? readString : "", sizeof( event.name ) - 1 );
	event.entityIndex = READ_SHORT();

	readString = READ_STRING();
	strncpy( event.className, readString ? readString : "", sizeof( event.className ) - 1 );
	readString = READ_STRING();
	strncpy( event.targetName, readString ? readString : "", sizeof( event.targetName ) - 1 );
	readString = READ_STRING();
	strncpy( event.fieldName, readString ? readString : "", sizeof( event.fieldName ) - 1 );
	readString = READ_STRING();
	strncpy( event.stringValue, readString ? readString : "", sizeof( event.stringValue ) - 1 );

	event.userId = 1; // This project is single-player; the local player is the sole recipient.
	event.integerValue = READ_LONG();
	event.floatValue = READ_FLOAT();

	GameInstructor_FireEvent( event );
	return 1;
}

// The Source manager can keep many opportunities open, but this GoldSrc port
// intentionally renders the highest-priority lesson through the existing HUD card.
void HUD_SetManagedInstructorHint( const char *lessonName, const char *caption,
	const char *binding, const char *onscreenIcon, const char *offscreenIcon,
	int targetIndex, float timeout, int flags, int colorR, int colorG, int colorB,
	float range, float iconOffset, bool forceCaption,
	bool fixedPosition, float fixedX, float fixedY )
{
	(void)timeout;

	// A map-authored env_instructor_hint currently owns the card.
	if( gHUD.m_bInstructorHintActive && !gHUD.m_bInstructorHintManaged )
		return;

	const bool lessonChanged = !gHUD.m_bInstructorHintManaged ||
		strcmp( gHUD.m_szInstructorHintName, lessonName ? lessonName : "" ) != 0;
	const bool onscreenIconChanged = strcmp( gHUD.m_szInstructorHintIconOnscreen,
		onscreenIcon ? onscreenIcon : "" ) != 0;
	const bool offscreenIconChanged = strcmp( gHUD.m_szInstructorHintIconOffscreen,
		offscreenIcon ? offscreenIcon : "" ) != 0;

	gHUD.m_bInstructorHintActive = true;
	gHUD.m_bInstructorHintManaged = true;
	gHUD.m_iInstructorHintId = 0;
	gHUD.m_iInstructorHintTarget = targetIndex;
	gHUD.m_bInstructorHintFixedPosition = fixedPosition;
	gHUD.m_flInstructorHintFixedX = fixedX;
	gHUD.m_flInstructorHintFixedY = fixedY;
	gHUD.m_bInstructorHintHasTargetPosition = targetIndex > 0 && !fixedPosition;
	gHUD.m_bInstructorHintTargetVisible = true;
	gHUD.m_bInstructorHintOccluded = false;
	gHUD.m_flInstructorHintEndTime = 0.0f;
	gHUD.m_flInstructorHintRange = range;
	gHUD.m_flInstructorHintIconOffset = iconOffset;
	gHUD.m_iInstructorHintFlags = flags | ( forceCaption ? 0x00000400 : 0 );
	gHUD.m_iInstructorHintIconMode = 0;
	gHUD.m_iInstructorHintColorR = colorR;
	gHUD.m_iInstructorHintColorG = colorG;
	gHUD.m_iInstructorHintColorB = colorB;

	if( lessonChanged )
	{
		gHUD.m_flInstructorManagedVisibleSince = 0.0f;
		gHUD.m_bInstructorManagedDisplayCounted = false;
		strncpy( gHUD.m_szInstructorHintName, lessonName ? lessonName : "",
			sizeof( gHUD.m_szInstructorHintName ) - 1 );
		gHUD.m_szInstructorHintName[sizeof( gHUD.m_szInstructorHintName ) - 1] = '\0';
	}
	strncpy( gHUD.m_szInstructorHintCaption, caption ? caption : "",
		sizeof( gHUD.m_szInstructorHintCaption ) - 1 );
	gHUD.m_szInstructorHintCaption[sizeof( gHUD.m_szInstructorHintCaption ) - 1] = '\0';
	strncpy( gHUD.m_szInstructorHintBinding, binding ? binding : "",
		sizeof( gHUD.m_szInstructorHintBinding ) - 1 );
	gHUD.m_szInstructorHintBinding[sizeof( gHUD.m_szInstructorHintBinding ) - 1] = '\0';

	if( onscreenIconChanged )
	{
		strncpy( gHUD.m_szInstructorHintIconOnscreen, onscreenIcon ? onscreenIcon : "",
			sizeof( gHUD.m_szInstructorHintIconOnscreen ) - 1 );
		gHUD.m_szInstructorHintIconOnscreen[sizeof( gHUD.m_szInstructorHintIconOnscreen ) - 1] = '\0';
		gHUD.m_hInstructorHintOnscreenSprite = HUD_LoadInstructorHintSprite( gHUD.m_szInstructorHintIconOnscreen );
	}
	if( offscreenIconChanged )
	{
		strncpy( gHUD.m_szInstructorHintIconOffscreen, offscreenIcon ? offscreenIcon : "",
			sizeof( gHUD.m_szInstructorHintIconOffscreen ) - 1 );
		gHUD.m_szInstructorHintIconOffscreen[sizeof( gHUD.m_szInstructorHintIconOffscreen ) - 1] = '\0';
		gHUD.m_hInstructorHintOffscreenSprite = HUD_LoadInstructorHintSprite( gHUD.m_szInstructorHintIconOffscreen );
	}
}

void HUD_ClearManagedInstructorHint()
{
	if( gHUD.m_bInstructorHintManaged )
		HUD_ClearInstructorHint( &gHUD );
}

void HUD_PlayInstructorSound( const char *soundName )
{
	if( soundName && soundName[0] && gEngfuncs.pfnPlaySoundByName )
		gEngfuncs.pfnPlaySoundByName( soundName, 1.0f );
}

int CHud::MsgFunc_ResetHUD( const char *pszName, int iSize, void *pbuf )
{
	ASSERT( iSize == 0 );

	// clear all hud data
	HUDLIST *pList = m_pHudList;

	while( pList )
	{
		if( pList->p )
			pList->p->Reset();
		pList = pList->pNext;
	}

	// Instructor hints are transient and must not survive a HUD reset.
	HUD_ClearInstructorHint( this );

	// reset sensitivity
	m_flMouseSensitivity = 0;

	// reset concussion effect
	m_iConcussionEffect = 0;

	// Vit_amiN: reset the FOV
	m_iFOV = 0;	// default_fov
	g_lastFOV = 0.0f;
	m_inScope = false;

	return 1;
}

void CAM_ToFirstPerson( void );

void CHud::MsgFunc_ViewMode( const char *pszName, int iSize, void *pbuf )
{
	CAM_ToFirstPerson();
}

void CHud::MsgFunc_InitHUD( const char *pszName, int iSize, void *pbuf )
{
	// prepare all hud data
	HUDLIST *pList = m_pHudList;

	while( pList )
	{
		if( pList->p )
			pList->p->InitHUDData();
		pList = pList->pNext;
	}

	//Probably not a good place to put this.
	pBeam = pBeam2 = NULL;
	pFlare = NULL;	// Vit_amiN: clear egon's beam flare
}

int CHud::MsgFunc_GameMode( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	m_Teamplay = READ_BYTE();

	if( m_Teamplay )
		ClientCmd( "richpresence_gamemode Teamplay\n" );
	else
		ClientCmd( "richpresence_gamemode\n" );
	ClientCmd( "richpresence_update\n" );
	return 1;
}

int CHud::MsgFunc_Damage( const char *pszName, int iSize, void *pbuf )
{
	int		armor, blood;
	Vector	from;
	int		i;
	float	count;

	BEGIN_READ( pbuf, iSize );
	armor = READ_BYTE();
	blood = READ_BYTE();

	for( i = 0; i < 3; i++)
		from[i] = READ_COORD();

	count = ( blood * 0.5 ) + ( armor * 0.5 );

	if( count < 10 )
		count = 10;

	// TODO: kick viewangles,  show damage visually
	return 1;
}

int CHud::MsgFunc_Concuss( const char *pszName, int iSize, void *pbuf )
{
	int r, g, b;
	BEGIN_READ( pbuf, iSize );
	m_iConcussionEffect = READ_BYTE();
	if( m_iConcussionEffect )
	{
		UnpackRGB( r, g, b, RGB_YELLOWISH );	// Vit_amiN: fixed
		this->m_StatusIcons.EnableIcon( "dmg_concuss", r, g, b );
	}
	else
		this->m_StatusIcons.DisableIcon( "dmg_concuss" );
	return 1;
}

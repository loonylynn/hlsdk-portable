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
	if( !iconName || !iconName[0] || stricmp( iconName, "use_binding" ) == 0 )
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


void HUD_SyncInstructorLocatorTarget( CHud *hud )
{
	if( !hud )
		return;

	if( !hud->m_bInstructorHintActive )
	{
		if( hud->m_iInstructorLocatorTargetHandle >= 0 )
		{
			if( hud->m_InstructorLocatorPanel.GetTargetFromHandle( hud->m_iInstructorLocatorTargetHandle ) )
				hud->m_InstructorLocatorPanel.RemoveTarget(
					hud->m_iInstructorLocatorTargetHandle, hud->m_flTime, 0.3f );
			else
				hud->m_iInstructorLocatorTargetHandle = -1;
		}
		return;
	}

	CInstructorLocatorTarget *existingTarget =
		hud->m_InstructorLocatorPanel.GetTargetFromHandle( hud->m_iInstructorLocatorTargetHandle );
	if( existingTarget && existingTarget->m_bRemoveWhenFadedOut )
	{
		existingTarget->Deactivate( true, hud->m_flTime, 0.0f );
		hud->m_iInstructorLocatorTargetHandle = -1;
		existingTarget = NULL;
	}

	if( !existingTarget )
	{
		hud->m_iInstructorLocatorTargetHandle =
			hud->m_InstructorLocatorPanel.AddTarget( hud->m_scrinfo.iWidth, hud->m_scrinfo.iHeight, hud->m_flTime );
	}

	CInstructorLocatorTarget *target =
		hud->m_InstructorLocatorPanel.GetTargetFromHandle( hud->m_iInstructorLocatorTargetHandle );
	if( !target )
		return;

	target->Update( hud->m_InstructorLocatorPanel.GetFrameCount() );
	const bool managedLessonChanged = !target->m_bManaged ||
		strcmp( target->m_szName, hud->m_szInstructorHintName ) != 0;
	target->m_iEntityIndex = hud->m_iInstructorHintTarget;
	target->m_bFixedPosition = hud->m_bInstructorHintFixedPosition;
	target->m_iHintId = hud->m_iInstructorHintId;
	target->m_iIconMode = hud->m_iInstructorHintIconMode;
	target->m_bManaged = hud->m_bInstructorHintManaged;
	if( managedLessonChanged )
	{
		target->m_flVisibleSince = 0.0f;
		target->m_bManagedDisplayCounted = false;
		target->m_iBindingChoicesCount = 0;
		target->m_iBindingTick = 0;
		target->m_flNextBindingTick = 0.0f;
		target->m_szBindingCache[0] = '\0';
	}
	target->m_flIconOffset = hud->m_flInstructorHintIconOffset;
	target->m_bOriginInScreenspace =
		hud->m_bInstructorHintFixedPosition || !hud->m_bInstructorHintHasTargetPosition;
	if( target->m_bOriginInScreenspace )
	{
		target->m_vecOrigin[0] = hud->m_bInstructorHintFixedPosition ? hud->m_flInstructorHintFixedX : 0.5f;
		target->m_vecOrigin[1] = hud->m_bInstructorHintFixedPosition ? hud->m_flInstructorHintFixedY : 0.5f;
		target->m_vecOrigin[2] = 0.0f;
	}
	else
	{
		target->m_vecOrigin[0] = hud->m_vecInstructorHintTargetOrigin[0];
		target->m_vecOrigin[1] = hud->m_vecInstructorHintTargetOrigin[1];
		target->m_vecOrigin[2] = hud->m_vecInstructorHintTargetOrigin[2];
	}
	target->m_bOccluded = hud->m_bInstructorHintOccluded;
	const bool forceCaption = ( hud->m_iInstructorHintFlags & LOCATOR_ICON_FX_FORCE_CAPTION ) != 0;
	const bool shouldBeVisible = hud->m_bInstructorHintTargetVisible &&
		( !hud->m_bInstructorHintOccluded || forceCaption );
	target->SetVisible( shouldBeVisible, hud->m_flTime, 0.3f );
	const int animationFlags = target->m_iEffectsFlags &
		( LOCATOR_ICON_FX_FADE_IN | LOCATOR_ICON_FX_FADE_OUT );
	target->m_iEffectsFlags = hud->m_iInstructorHintFlags | animationFlags;
	target->m_flRange = hud->m_flInstructorHintRange;
	target->SetCaptionColor( hud->m_iInstructorHintColorR,
		hud->m_iInstructorHintColorG, hud->m_iInstructorHintColorB );
	target->SetCaptionText( hud->m_szInstructorHintCaption );
	target->SetBinding( hud->m_szInstructorHintBinding );
	target->SetOnscreenIconName( hud->m_szInstructorHintIconOnscreen, hud->m_flTime );
	target->SetOffscreenIconName( hud->m_szInstructorHintIconOffscreen, hud->m_flTime );
	strncpy( target->m_szName, hud->m_szInstructorHintName, sizeof( target->m_szName ) - 1 );
	target->m_szName[sizeof( target->m_szName ) - 1] = '\0';
	target->m_hOnscreenSprite = (int)hud->m_hInstructorHintOnscreenSprite;
	target->m_hOffscreenSprite = (int)hud->m_hInstructorHintOffscreenSprite;
	target->m_flEndTime = 0.0f;
}

static void HUD_ClearInstructorHint( CHud *hud )
{
	if( hud->m_iInstructorLocatorTargetHandle >= 0 )
	{
		hud->m_InstructorLocatorPanel.RemoveTarget( hud->m_iInstructorLocatorTargetHandle, hud->m_flTime, 0.3f );
	}
	hud->m_bInstructorHintActive = false;
	hud->m_bInstructorHintManaged = false;
	hud->m_bInstructorHintFixedPosition = false;
	hud->m_flInstructorHintFixedX = 0.5f;
	hud->m_flInstructorHintFixedY = 0.5f;
	hud->m_flInstructorManagedVisibleSince = 0.0f;
	hud->m_bInstructorManagedDisplayCounted = false;
	hud->m_iInstructorHintId = 0;
	hud->m_iInstructorHintTarget = 0;
	hud->m_bInstructorHintHasTargetPosition = false;
	hud->m_bInstructorHintTargetVisible = false;
	hud->m_bInstructorHintOccluded = false;
	hud->m_flInstructorHintNextOcclusionTest = 0.0f;
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
	hud->m_szInstructorHintBindingCache[0] = '\0';
	hud->m_iInstructorHintBindingChoicesCount = 0;
	hud->m_iInstructorHintBindingTick = 0;
	hud->m_flInstructorHintNextBindingTick = 0.0f;
	for( int i = 0; i < MAX_INSTRUCTOR_BINDING_CHOICES; ++i )
		hud->m_szInstructorHintBindingChoices[i][0] = '\0';
}


static CInstructorLocatorTarget *HUD_FindInstructorMapTarget( CHud *hud, int hintId )
{
	if( !hud )
		return NULL;
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		CInstructorLocatorTarget *target = hud->m_InstructorLocatorPanel.GetTargetAt( i );
		if( target && target->m_isActive && !target->m_bManaged && target->m_iHintId == hintId )
			return target;
	}
	return NULL;
}

static void HUD_RemoveInstructorHintsWithReplaceKey( CHud *hud, const char *replaceKey,
	CInstructorLocatorTarget *exceptTarget )
{
	if( !hud || !replaceKey || !replaceKey[0] )
		return;
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		CInstructorLocatorTarget *target = hud->m_InstructorLocatorPanel.GetTargetAt( i );
		if( target && target != exceptTarget && target->m_isActive && !target->m_bManaged &&
			target->m_szReplaceKey[0] && stricmp( target->m_szReplaceKey, replaceKey ) == 0 )
			hud->m_InstructorLocatorPanel.RemoveTarget( target->m_serialNumber, hud->m_flTime, 0.3f );
	}
}

int CHud::MsgFunc_InstructorHint( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );
	const int action = READ_BYTE();
	const int hintId = READ_SHORT();

	if( action == 0 )
	{
		CInstructorLocatorTarget *target = HUD_FindInstructorMapTarget( this, hintId );
		if( target )
		{
			const float removalFade = hintId < 0 ? 0.08f : 0.3f;
			m_InstructorLocatorPanel.RemoveTarget( target->m_serialNumber, m_flTime, removalFade );
		}
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
		char captionUpdate[192], bindingUpdate[64];
		const char *readCaptionUpdate = READ_STRING();
		strncpy( captionUpdate, readCaptionUpdate ? readCaptionUpdate : "", sizeof( captionUpdate ) - 1 );
		captionUpdate[sizeof( captionUpdate ) - 1] = '\0';
		const char *readBindingUpdate = READ_STRING();
		strncpy( bindingUpdate, readBindingUpdate ? readBindingUpdate : "", sizeof( bindingUpdate ) - 1 );
		bindingUpdate[sizeof( bindingUpdate ) - 1] = '\0';

		CInstructorLocatorTarget *target = HUD_FindInstructorMapTarget( this, hintId );
		if( target )
		{
			target->Update( m_InstructorLocatorPanel.GetFrameCount() );
			target->m_iEntityIndex = targetIndex;
			target->m_bOriginInScreenspace =
				targetIndex <= 0 || ( target->m_iEffectsFlags & LOCATOR_ICON_FX_STATIC );
			if( target->m_bOriginInScreenspace )
				target->SetScreenOrigin( 0.5f, 0.5f );
			else
			{
				const float origin[3] = { x, y, z };
				target->SetWorldOrigin( origin, targetIndex );
			}
			target->m_iIconMode = iconMode;
			target->m_bOccluded = occluded;
			if( captionUpdate[0] )
				target->SetCaptionText( captionUpdate );
			if( bindingUpdate[0] )
				target->SetBinding( bindingUpdate );
			if( hintId == -1 )
			{
				const char *caption = iconMode == 2 ?
					"Hold USE and move backward to pull" : "Move into it to push";
				const char *binding = iconMode == 2 ? "USE" : "";
				target->SetCaptionText( caption );
				target->SetBinding( binding );
			}
			const bool forceCaption = ( target->m_iEffectsFlags & LOCATOR_ICON_FX_FORCE_CAPTION ) != 0;
			target->SetVisible( targetVisible && ( !occluded || forceCaption ), m_flTime, 0.3f );
			if( targetIndex <= 0 && !( target->m_iEffectsFlags & LOCATOR_ICON_FX_STATIC ) &&
				!targetVisible )
				m_InstructorLocatorPanel.RemoveTarget( target->m_serialNumber, m_flTime, 0.3f );
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

	// A map-authored hint takes precedence over the currently managed lesson.
	if( m_bInstructorHintActive && m_bInstructorHintManaged )
		HUD_ClearInstructorHint( this );

	CInstructorLocatorTarget *target = HUD_FindInstructorMapTarget( this, hintId );
	if( target && target->m_bRemoveWhenFadedOut )
	{
		target->Deactivate( true, m_flTime, 0.0f );
		target = NULL;
	}
	HUD_RemoveInstructorHintsWithReplaceKey( this, replaceKey, target );

	if( !target )
	{
		const int handle = m_InstructorLocatorPanel.AddTarget(
			m_scrinfo.iWidth, m_scrinfo.iHeight, m_flTime );
		target = m_InstructorLocatorPanel.GetTargetFromHandle( handle );
	}
	if( !target )
		return 1;

	const int animationFlags = target->m_iEffectsFlags &
		( LOCATOR_ICON_FX_FADE_IN | LOCATOR_ICON_FX_FADE_OUT );
	target->m_iHintId = hintId;
	target->m_iEntityIndex = targetIndex;
	target->m_iIconMode = iconMode;
	target->m_bManaged = false;
	target->m_bFixedPosition = false;
	target->m_bOriginInScreenspace =
		targetIndex <= 0 || ( flags & LOCATOR_ICON_FX_STATIC );
	if( target->m_bOriginInScreenspace )
		target->SetScreenOrigin( 0.5f, 0.5f );
	else
	{
		const float origin[3] = { x, y, z };
		target->SetWorldOrigin( origin, targetIndex );
	}
	target->m_bOccluded = occluded;
	target->m_flEndTime = timeoutTenths > 0 ? m_flTime + timeoutTenths * 0.1f : 0.0f;
	target->m_flRange = range;
	target->m_flIconOffset = 0.0f; // server coordinates already include hint_icon_offset
	target->m_iEffectsFlags = flags | animationFlags;
	target->SetCaptionColor( colorR, colorG, colorB );
	target->SetCaptionText( caption );
	target->SetBinding( binding );
	target->SetOnscreenIconName( iconOnscreen, m_flTime );
	target->SetOffscreenIconName( iconOffscreen, m_flTime );
	strncpy( target->m_szName, hintName, sizeof( target->m_szName ) - 1 );
	target->m_szName[sizeof( target->m_szName ) - 1] = '\0';
	strncpy( target->m_szReplaceKey, replaceKey, sizeof( target->m_szReplaceKey ) - 1 );
	target->m_szReplaceKey[sizeof( target->m_szReplaceKey ) - 1] = '\0';
	target->m_hOnscreenSprite = (int)HUD_LoadInstructorHintSprite( iconOnscreen );
	target->m_hOffscreenSprite = (int)HUD_LoadInstructorHintSprite( iconOffscreen );
	target->m_iBindingChoicesCount = 0;
	target->m_iBindingTick = 0;
	target->m_flNextBindingTick = 0.0f;
	target->m_szBindingCache[0] = '\0';
	target->m_bManagedDisplayCounted = false;
	target->m_flVisibleSince = 0.0f;
	target->Update( m_InstructorLocatorPanel.GetFrameCount() );

	if( hintId == -1 )
	{
		const char *pushableCaption = iconMode == 2 ?
			"Hold USE and move backward to pull" : "Move into it to push";
		const char *pushableBinding = iconMode == 2 ? "USE" : "";
		target->SetCaptionText( pushableCaption );
		target->SetBinding( pushableBinding );
	}
	const bool forceCaption = ( flags & LOCATOR_ICON_FX_FORCE_CAPTION ) != 0;
	target->SetVisible( targetVisible && ( !occluded || forceCaption ), m_flTime, 0.3f );
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

	if( CVAR_GET_FLOAT( "gameinstructor_verbose" ) != 0.0f && gEngfuncs.Con_Printf )
		gEngfuncs.Con_Printf( "[GameInstructor] Client received InstructorEvent '%s' (entity %d, class '%s').\n",
			event.name, event.entityIndex, event.className );

	GameInstructor_FireEvent( event );
	return 1;
}

// The Source manager can keep many opportunities open. Each eligible managed
// opportunity is now represented by its own target in the shared locator pool.
static CInstructorLocatorTarget *HUD_FindManagedInstructorTarget( CHud *hud,
	const char *lessonName, int instanceSerial )
{
	if( !hud || !lessonName || !lessonName[0] )
		return NULL;
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		CInstructorLocatorTarget *target = hud->m_InstructorLocatorPanel.GetTargetAt( i );
		if( target && target->m_isActive && target->m_bManaged &&
			target->m_iManagedInstanceId == instanceSerial &&
			strcmp( target->m_szName, lessonName ) == 0 )
			return target;
	}
	return NULL;
}

float HUD_GetInstructorPresentationRestTime()
{
	float restTime = CVAR_GET_FLOAT( "locator_lerp_rest" );
	return restTime > 0.0f ? restTime : 0.0f;
}

float HUD_GetInstructorPresentationLerpTime()
{
	float lerpTime = CVAR_GET_FLOAT( "locator_lerp_time" );
	return lerpTime > 0.0f ? lerpTime : 0.0f;
}

bool HUD_ApplyManagedInstructorPresentation( const char *lessonName,
	int instanceSerial, bool start )
{
	CInstructorLocatorTarget *target =
		HUD_FindManagedInstructorTarget( &gHUD, lessonName, instanceSerial );
	if( !target )
		return false;

	if( start )
		target->StartPresent( gHUD.m_flTime );
	else
		target->EndPresent( gHUD.m_flTime, HUD_GetInstructorPresentationRestTime() );
	return true;
}

bool HUD_IsManagedInstructorPresentationComplete( const char *lessonName,
	int instanceSerial )
{
	CInstructorLocatorTarget *target =
		HUD_FindManagedInstructorTarget( &gHUD, lessonName, instanceSerial );
	if( !target )
		return false;

	return !target->IsPresenting( gHUD.m_flTime,
		HUD_GetInstructorPresentationRestTime() );
}

void HUD_SetManagedInstructorHint( const char *lessonName, int instanceSerial, const char *caption,
	const char *binding, const char *onscreenIcon, const char *offscreenIcon,
	int targetIndex, int priority, float timeout, int flags, int colorR, int colorG, int colorB,
	float range, float iconOffset, bool forceCaption,
	bool fixedPosition, float fixedX, float fixedY )
{
	(void)timeout;

	if( !lessonName || !lessonName[0] )
		return;

	CInstructorLocatorTarget *target =
		HUD_FindManagedInstructorTarget( &gHUD, lessonName, instanceSerial );
	if( target && target->m_bRemoveWhenFadedOut )
	{
		target->Deactivate( true, gHUD.m_flTime, 0.0f );
		target = NULL;
	}

	bool created = false;
	if( !target )
	{
		const int handle = gHUD.m_InstructorLocatorPanel.AddTarget(
			gHUD.m_scrinfo.iWidth, gHUD.m_scrinfo.iHeight, gHUD.m_flTime );
		target = gHUD.m_InstructorLocatorPanel.GetTargetFromHandle( handle );
		created = target != NULL;
	}
	if( !target )
	{
		if( CVAR_GET_FLOAT( "gameinstructor_verbose" ) != 0.0f && gEngfuncs.Con_Printf )
			gEngfuncs.Con_Printf( "[GameInstructor] Locator pool full; could not display lesson '%s'.\n",
				lessonName );
		return;
	}

	const bool lessonChanged = created || strcmp( target->m_szName, lessonName ) != 0;
	const bool targetChanged = created || target->m_iEntityIndex != targetIndex ||
		target->m_bFixedPosition != fixedPosition;
	const bool bindingChanged = strcmp( target->m_szBinding, binding ? binding : "" ) != 0;
	const bool onscreenIconChanged = strcmp( target->m_szOnscreenIcon,
		onscreenIcon ? onscreenIcon : "" ) != 0;
	const bool offscreenIconChanged = strcmp( target->m_szOffscreenIcon,
		offscreenIcon ? offscreenIcon : "" ) != 0;

	if( lessonChanged || targetChanged )
	{
		target->m_flVisibleSince = 0.0f;
		target->m_bManagedDisplayCounted = false;
		target->m_iBindingChoicesCount = 0;
		target->m_iBindingTick = 0;
		target->m_flNextBindingTick = 0.0f;
		target->m_szBindingCache[0] = '\0';
	}
	if( bindingChanged )
	{
		target->m_iBindingChoicesCount = 0;
		target->m_iBindingTick = 0;
		target->m_flNextBindingTick = 0.0f;
		target->m_szBindingCache[0] = '\0';
	}
	if( targetChanged )
	{
		target->m_bOccluded = false;
		target->m_flNextOcclusionTest = 0.0f;
	}

	const int animationFlags = target->m_iEffectsFlags &
		( LOCATOR_ICON_FX_FADE_IN | LOCATOR_ICON_FX_FADE_OUT );
	target->m_iHintId = 0;
	target->m_iPriority = priority;
	target->m_iManagedInstanceId = instanceSerial;
	target->m_iEntityIndex = targetIndex;
	target->m_iIconMode = 0;
	target->m_bManaged = true;
	target->m_bFixedPosition = fixedPosition;
	target->m_bOriginInScreenspace = fixedPosition || targetIndex <= 0;
	if( fixedPosition )
		target->SetScreenOrigin( fixedX, fixedY );
	else if( targetIndex <= 0 )
		target->SetScreenOrigin( 0.5f, 0.5f );
	else
		target->m_bOriginInScreenspace = false;

	target->m_flIconOffset = iconOffset;
	target->m_flRange = range;
	target->m_iEffectsFlags = flags | animationFlags |
		( forceCaption ? LOCATOR_ICON_FX_FORCE_CAPTION : 0 );
	target->SetVisible( true, gHUD.m_flTime, 0.3f );
	target->SetCaptionColor( colorR, colorG, colorB );
	target->SetCaptionText( caption );
	target->SetBinding( binding );
	if( onscreenIconChanged )
	{
		target->SetOnscreenIconName( onscreenIcon, gHUD.m_flTime );
		target->m_hOnscreenSprite = (int)HUD_LoadInstructorHintSprite( target->m_szOnscreenIcon );
	}
	if( offscreenIconChanged )
	{
		target->SetOffscreenIconName( offscreenIcon, gHUD.m_flTime );
		target->m_hOffscreenSprite = (int)HUD_LoadInstructorHintSprite( target->m_szOffscreenIcon );
	}

	strncpy( target->m_szName, lessonName, sizeof( target->m_szName ) - 1 );
	target->m_szName[sizeof( target->m_szName ) - 1] = '\0';
	target->m_flEndTime = 0.0f;
	target->Update( gHUD.m_InstructorLocatorPanel.GetFrameCount() );

	if( created && CVAR_GET_FLOAT( "gameinstructor_verbose" ) != 0.0f && gEngfuncs.Con_Printf )
		gEngfuncs.Con_Printf( "[GameInstructor] Added managed locator '%s' (priority %d, target %d).\n",
			lessonName, priority, targetIndex );
}

void HUD_PruneManagedInstructorHints()
{
	const int frameNumber = gHUD.m_InstructorLocatorPanel.GetFrameCount();
	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		CInstructorLocatorTarget *target = gHUD.m_InstructorLocatorPanel.GetTargetAt( i );
		if( target && target->m_isActive && target->m_bManaged &&
			target->m_frameLastUpdated != frameNumber )
			gHUD.m_InstructorLocatorPanel.RemoveTarget(
				target->m_serialNumber, gHUD.m_flTime, 0.3f );
	}
}

void HUD_ClearManagedInstructorHint()
{
	if( gHUD.m_bInstructorHintActive && gHUD.m_bInstructorHintManaged )
		HUD_ClearInstructorHint( &gHUD );

	for( int i = 0; i < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++i )
	{
		CInstructorLocatorTarget *target = gHUD.m_InstructorLocatorPanel.GetTargetAt( i );
		if( target && target->m_isActive && target->m_bManaged )
			gHUD.m_InstructorLocatorPanel.RemoveTarget(
				target->m_serialNumber, gHUD.m_flTime, 0.3f );
	}
	gHUD.m_iInstructorLocatorTargetHandle = -1;
	gHUD.m_bInstructorHintActive = false;
	gHUD.m_bInstructorHintManaged = false;
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
	m_InstructorLocatorPanel.DeactivateAllTargets( true, m_flTime, 0.0f );

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

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
#include "parsemsg.h"
#include "r_efx.h"

#define MAX_CLIENTS 32

extern BEAM *pBeam;
extern BEAM *pBeam2;
extern TEMPENTITY *pFlare;	// Vit_amiN

extern float g_lastFOV;			// Vit_amiN

/// USER-DEFINED SERVER MESSAGE HANDLERS

int CHud::MsgFunc_InstructorHint( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );

	const int action = READ_BYTE();
	const int hintId = READ_SHORT();

	if( action == 0 )
	{
		// Only the hint that owns the active panel may dismiss it.
		if( m_bInstructorHintActive && hintId == m_iInstructorHintId )
		{
			m_bInstructorHintActive = false;
			m_iInstructorHintId = 0;
			m_iInstructorHintTarget = 0;
			m_flInstructorHintEndTime = 0.0f;
			m_szInstructorHintCaption[0] = '\\0';
			m_szInstructorHintBinding[0] = '\\0';
		}
		return 1;
	}

	m_iInstructorHintTarget = READ_SHORT();
	const int timeoutTenths = READ_SHORT();
	const char *caption = READ_STRING();
	strncpy( m_szInstructorHintCaption, caption ? caption : "", sizeof( m_szInstructorHintCaption ) - 1 );
	m_szInstructorHintCaption[sizeof( m_szInstructorHintCaption ) - 1] = '\\0';

	const char *binding = READ_STRING();
	strncpy( m_szInstructorHintBinding, binding ? binding : "", sizeof( m_szInstructorHintBinding ) - 1 );
	m_szInstructorHintBinding[sizeof( m_szInstructorHintBinding ) - 1] = '\\0';

	m_iInstructorHintId = hintId;
	m_flInstructorHintEndTime = timeoutTenths > 0 ? m_flTime + ( timeoutTenths * 0.1f ) : 0.0f;
	m_bInstructorHintActive = ( m_szInstructorHintCaption[0] != '\\0' || m_szInstructorHintBinding[0] != '\\0' );
	return 1;
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
	m_bInstructorHintActive = false;
	m_iInstructorHintId = 0;
	m_iInstructorHintTarget = 0;
	m_flInstructorHintEndTime = 0.0f;
	m_szInstructorHintCaption[0] = '\\0';
	m_szInstructorHintBinding[0] = '\\0';

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

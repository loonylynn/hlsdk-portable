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
// Health.cpp
//
// implementation of CHudHealth class
//

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "hud.h"
#include "cl_util.h"
#include "cl_entity.h"
#include "parsemsg.h"
#include "../engine/keydefs.h"
#include <string.h>

#include "mobility_int.h"

DECLARE_MESSAGE( m_Health, Health )
DECLARE_MESSAGE( m_Health, Damage )

#define PAIN_NAME "sprites/%d_pain.spr"
#define DAMAGE_NAME "sprites/%d_dmg.spr"

// Auxiliary power HUD sheet: frame and fill are cropped from one 80x64 sprite.
static HSPRITE g_hAuxHudSprite = 0;
static const wrect_t g_rcAuxHudFrame = { 9, 72, 7, 24 };
static const wrect_t g_rcAuxHudFill = { 13, 66, 43, 52 };

// Latest client-predicted aux reserve, updated by HUD_PlayerMove.
float g_flHL2AuxPower = -1.0f;
// Reserve copied from the post-prediction local_state_t result.
float g_flHL2AuxPowerPostRun = -1.0f;

static void AuxPower_EditCommand( void );
static void AuxPower_CommitCommand( void );
static void AuxPower_CancelCommand( void );
static void AuxPower_ReportCommand( void );

int giDmgHeight, giDmgWidth;

int giDmgFlags[NUM_DMG_TYPES] =
{
	DMG_POISON,
	DMG_ACID,
	DMG_FREEZE|DMG_SLOWFREEZE,
	DMG_DROWN,
	DMG_BURN|DMG_SLOWBURN,
	DMG_NERVEGAS, 
	DMG_RADIATION,
	DMG_SHOCK,
	DMG_CALTROP,
	DMG_TRANQ,
	DMG_CONCUSS,
	DMG_HALLUC
};

int CHudHealth::Init( void )
{
	HOOK_MESSAGE( Health );
	HOOK_MESSAGE( Damage );

	CVAR_CREATE( "hud_auxpower_x", "-1", FCVAR_ARCHIVE );
	CVAR_CREATE( "hud_auxpower_y", "-1", FCVAR_ARCHIVE );
	CVAR_CREATE( "hud_auxpower_grid", "8", FCVAR_ARCHIVE );
	CVAR_CREATE( "hud_auxpower_snap", "1", FCVAR_ARCHIVE );
	CVAR_CREATE( "hud_auxpower_layout_version", "0", FCVAR_ARCHIVE );

	gEngfuncs.pfnAddCommand( "hud_auxpower_edit", AuxPower_EditCommand );
	gEngfuncs.pfnAddCommand( "hud_auxpower_commit", AuxPower_CommitCommand );
	gEngfuncs.pfnAddCommand( "hud_auxpower_cancel", AuxPower_CancelCommand );
	gEngfuncs.pfnAddCommand( "hud_auxpower_report", AuxPower_ReportCommand );
	m_iHealth = 100;
	m_fFade = 0;
	m_iFlags = 0;
	m_bitsDamage = 0;
	m_fAttackFront = m_fAttackRear = m_fAttackRight = m_fAttackLeft = 0;
	giDmgHeight = 0;
	giDmgWidth = 0;

	memset( m_dmg, 0, sizeof(DAMAGE_IMAGE) * NUM_DMG_TYPES );

	gHUD.AddHudElem( this );
	return 1;
}

void CHudHealth::Reset( void )
{
	// make sure the pain compass is cleared when the player respawns
	m_fAttackFront = m_fAttackRear = m_fAttackRight = m_fAttackLeft = 0;

	// force all the flashing damage icons to expire
	m_bitsDamage = 0;
	for( int i = 0; i < NUM_DMG_TYPES; i++ )
	{
		m_dmg[i].fExpire = 0;
	}
}

int CHudHealth::VidInit( void )
{
	m_hSprite = 0;

	m_HUD_dmg_bio = gHUD.GetSpriteIndex( "dmg_bio" ) + 1;
	m_HUD_cross = gHUD.GetSpriteIndex( "cross" );

	// This is a standalone sprite sheet, not an entry in hud.txt.
	g_hAuxHudSprite = SPR_Load( "sprites/auxhud.spr" );

	giDmgHeight = gHUD.GetSpriteRect( m_HUD_dmg_bio ).right - gHUD.GetSpriteRect( m_HUD_dmg_bio ).left;
	giDmgWidth = gHUD.GetSpriteRect( m_HUD_dmg_bio ).bottom - gHUD.GetSpriteRect( m_HUD_dmg_bio ).top;

	return 1;
}

int CHudHealth::MsgFunc_Health( const char *pszName, int iSize, void *pbuf )
{
	// TODO: update local health data
	BEGIN_READ( pbuf, iSize );
	int x = READ_BYTE();

	m_iFlags |= HUD_ACTIVE;

	// Only update the fade if we've changed health
	if( x != m_iHealth )
	{
		m_fFade = FADE_TIME;
		m_iHealth = x;
	}

	return 1;
}

int CHudHealth::MsgFunc_Damage( const char *pszName, int iSize, void *pbuf )
{
	BEGIN_READ( pbuf, iSize );

	int armor = READ_BYTE();	// armor
	int damageTaken = READ_BYTE();	// health
	long bitsDamage = READ_LONG(); // damage bits

	vec3_t vecFrom;

	for( int i = 0; i < 3; i++ )
		vecFrom[i] = READ_COORD();

	UpdateTiles( gHUD.m_flTime, bitsDamage );

	// Actually took damage?
	if( damageTaken > 0 || armor > 0 )
	{
		CalcDamageDirection( vecFrom );

                if( gMobileEngfuncs && damageTaken > 0 )
                {
			float time = damageTaken * 4.0f;

			if( time > 200.0f )
				time = 200.0f;
			gMobileEngfuncs->pfnVibrate( time, 0 );
                }
	}

	return 1;
}

// Returns back a color from the
// Green <-> Yellow <-> Red ramp
void CHudHealth::GetPainColor( int &r, int &g, int &b )
{
#if 0
	int iHealth = m_iHealth;

	if( iHealth > 25 )
		iHealth -= 25;
	else if( iHealth < 0 )
		iHealth = 0;

	g = iHealth * 255 / 100;
	r = 255 - g;
	b = 0;
#else
	if( m_iHealth > 25 )
	{
		UnpackRGB( r, g, b, RGB_YELLOWISH );
	}
	else
	{
		r = 250;
		g = 0;
		b = 0;
	}
#endif
}

// AUX. POWER HUD layout/editor state. Coordinates are stored in a 640x480 reference space.
static int g_bAuxPowerEditorActive = 0;
static int g_iAuxPowerPreviewX = 0;
static int g_iAuxPowerPreviewY = 0;
static int g_iAuxPowerCursorX = 0;
static int g_iAuxPowerCursorY = 0;
static int g_iAuxPowerMouseButtons = 0;
static int g_bAuxPowerDragging = 0;
static int g_iAuxPowerDragOffsetX = 0;
static int g_iAuxPowerDragOffsetY = 0;
static int g_bAuxPowerShiftDown = 0;

static int AuxPower_RefToScreenX( int x )
{
	if( ScreenWidth <= 0 )
		return x;
	return (int)( (float)x * (float)ScreenWidth / 640.0f + 0.5f );
}

static int AuxPower_RefToScreenY( int y )
{
	if( ScreenHeight <= 0 )
		return y;
	return (int)( (float)y * (float)ScreenHeight / 480.0f + 0.5f );
}

static int AuxPower_ScreenToRefX( int x )
{
	if( ScreenWidth <= 0 )
		return x;
	return (int)( (float)x * 640.0f / (float)ScreenWidth + 0.5f );
}

static int AuxPower_ScreenToRefY( int y )
{
	if( ScreenHeight <= 0 )
		return y;
	return (int)( (float)y * 480.0f / (float)ScreenHeight + 0.5f );
}

static int AuxPower_GetGrid( void )
{
	int grid = (int)CVAR_GET_FLOAT( "hud_auxpower_grid" );
	if( grid < 1 ) grid = 1;
	if( grid > 128 ) grid = 128;
	return grid;
}

static int AuxPower_GetLabelWidth( void )
{
	return gHUD.DrawHudStringLen( (char*)"AUX. POWER" );
}

static void AuxPower_GetDefaultPosition( int *x, int *y )
{
	int healthY = ScreenHeight - gHUD.m_iFontHeight - gHUD.m_iFontHeight / 2;
	int frameWidth = g_rcAuxHudFrame.right - g_rcAuxHudFrame.left;
	int frameHeight = g_rcAuxHudFrame.bottom - g_rcAuxHudFrame.top;
	int labelWidth = AuxPower_GetLabelWidth();
	int oldFrameOffsetX = ( ( labelWidth > frameWidth ) ? labelWidth : frameWidth ) - frameWidth;
	int crossSprite = gHUD.GetSpriteIndex( "cross" );
	int crossWidth = gHUD.GetSpriteRect( crossSprite ).right -
		gHUD.GetSpriteRect( crossSprite ).left;

	oldFrameOffsetX /= 2;
	*x = AuxPower_ScreenToRefX( crossWidth / 2 + oldFrameOffsetX );
	*y = AuxPower_ScreenToRefY( healthY - frameHeight );
}

static void AuxPower_MigrateSavedPosition( void )
{
	int x, y;
	int labelWidth, frameWidth, frameOffsetX, frameOffsetY;

	if( CVAR_GET_FLOAT( "hud_auxpower_layout_version" ) >= 1.0f )
		return;

	x = (int)CVAR_GET_FLOAT( "hud_auxpower_x" );
	y = (int)CVAR_GET_FLOAT( "hud_auxpower_y" );
	if( x >= 0 && y >= 0 )
	{
		labelWidth = AuxPower_GetLabelWidth();
		frameWidth = g_rcAuxHudFrame.right - g_rcAuxHudFrame.left;
		frameOffsetX = ( ( labelWidth > frameWidth ) ? labelWidth : frameWidth ) - frameWidth;
		frameOffsetX /= 2;
		frameOffsetY = gHUD.m_iFontHeight + 3;

		x += AuxPower_ScreenToRefX( frameOffsetX );
		y += AuxPower_ScreenToRefY( frameOffsetY );
		if( gEngfuncs.Cvar_SetValue )
		{
			gEngfuncs.Cvar_SetValue( "hud_auxpower_x", (float)x );
			gEngfuncs.Cvar_SetValue( "hud_auxpower_y", (float)y );
		}
	}

	if( gEngfuncs.Cvar_SetValue )
		gEngfuncs.Cvar_SetValue( "hud_auxpower_layout_version", 1.0f );
}

static void AuxPower_GetPosition( int *x, int *y )
{
	AuxPower_MigrateSavedPosition();

	if( g_bAuxPowerEditorActive )
	{
		*x = g_iAuxPowerPreviewX;
		*y = g_iAuxPowerPreviewY;
		return;
	}

	*x = (int)CVAR_GET_FLOAT( "hud_auxpower_x" );
	*y = (int)CVAR_GET_FLOAT( "hud_auxpower_y" );
	if( *x < 0 || *y < 0 )
		AuxPower_GetDefaultPosition( x, y );
}

static void AuxPower_GetLayout( int refX, int refY, int *left, int *top,
	int *width, int *height, int *labelX, int *labelY, int *frameX, int *frameY )
{
	int frameWidth = g_rcAuxHudFrame.right - g_rcAuxHudFrame.left;
	int frameHeight = g_rcAuxHudFrame.bottom - g_rcAuxHudFrame.top;
	int x = AuxPower_RefToScreenX( refX );
	int y = AuxPower_RefToScreenY( refY );

	*left = x;
	*top = y;
	*labelX = x;
	*labelY = y;
	*frameX = x;
	*frameY = y;
	*width = frameWidth;
	*height = frameHeight;
}

static void AuxPower_ClampPosition( int *x, int *y )
{
	int left, top, width, height, labelX, labelY, frameX, frameY;
	int maxX, maxY;

	AuxPower_GetLayout( *x, *y, &left, &top, &width, &height,
		&labelX, &labelY, &frameX, &frameY );
	(void)left; (void)top; (void)labelX; (void)labelY; (void)frameX; (void)frameY;

	maxX = 640 - AuxPower_ScreenToRefX( width );
	maxY = 480 - AuxPower_ScreenToRefY( height );
	if( maxX < 0 ) maxX = 0;
	if( maxY < 0 ) maxY = 0;

	if( *x < 0 ) *x = 0;
	if( *y < 0 ) *y = 0;
	if( *x > maxX ) *x = maxX;
	if( *y > maxY ) *y = maxY;
}

static void AuxPower_SnapPosition( int *x, int *y )
{
	int grid = AuxPower_GetGrid();

	if( CVAR_GET_FLOAT( "hud_auxpower_snap" ) != 0.0f )
	{
		*x = ( ( *x + grid / 2 ) / grid ) * grid;
		*y = ( ( *y + grid / 2 ) / grid ) * grid;
	}
	AuxPower_ClampPosition( x, y );
}

static void AuxPower_SetPreviewPosition( int x, int y, int forceNoSnap = 0 )
{
	if( forceNoSnap )
		AuxPower_ClampPosition( &x, &y );
	else
		AuxPower_SnapPosition( &x, &y );
	g_iAuxPowerPreviewX = x;
	g_iAuxPowerPreviewY = y;
}

static void AuxPower_PrintReport( void )
{
	int x, y;
	int left, top, width, height, labelX, labelY, frameX, frameY;
	int grid = AuxPower_GetGrid();
	cl_entity_t *pLocalPlayer = gEngfuncs.GetLocalPlayer();
	float flNetworkPower = pLocalPlayer ? pLocalPlayer->curstate.fuser4 : -1.0f;

	AuxPower_GetPosition( &x, &y );
	AuxPower_GetLayout( x, y, &left, &top, &width, &height,
		&labelX, &labelY, &frameX, &frameY );
	(void)left; (void)top; (void)labelX; (void)labelY; (void)frameX; (void)frameY;

	gEngfuncs.Con_Printf( "\n[AUX. POWER HUD] %s\n",
		g_bAuxPowerEditorActive ? "PREVIEW (not committed)" : "CURRENT LAYOUT" );
	gEngfuncs.Con_Printf( "hud_auxpower_x %d\n", x );
	gEngfuncs.Con_Printf( "hud_auxpower_y %d\n", y );
	gEngfuncs.Con_Printf( "hud_auxpower_grid %d\n", grid );
	gEngfuncs.Con_Printf( "hud_auxpower_snap %d\n",
		( CVAR_GET_FLOAT( "hud_auxpower_snap" ) != 0.0f ) ? 1 : 0 );
	gEngfuncs.Con_Printf( "auxpower post-run %.2f | predicted %.2f | entity-state %.2f\n",
		g_flHL2AuxPowerPostRun, g_flHL2AuxPower, flNetworkPower );
	gEngfuncs.Con_Printf( "// anchor: top-left of the battery frame; reference space: 640x480\n" );
	gEngfuncs.Con_Printf( "// battery bounds: %d x %d screen pixels; scale: 1.0\n",
		width, height );
	if( g_bAuxPowerEditorActive )
		gEngfuncs.Con_Printf( "// Run hud_auxpower_commit to save, or hud_auxpower_cancel to discard.\n" );
}

static void AuxPower_CommitEditor( void )
{
	if( !g_bAuxPowerEditorActive )
	{
		gEngfuncs.Con_Printf( "[AUX. POWER HUD] No edit session is active.\n" );
		return;
	}

	if( gEngfuncs.Cvar_SetValue )
	{
		gEngfuncs.Cvar_SetValue( "hud_auxpower_x", (float)g_iAuxPowerPreviewX );
		gEngfuncs.Cvar_SetValue( "hud_auxpower_y", (float)g_iAuxPowerPreviewY );
	}

	g_bAuxPowerEditorActive = 0;
	g_bAuxPowerDragging = 0;
	gEngfuncs.Con_Printf( "[AUX. POWER HUD] Placement committed.\n" );
	AuxPower_PrintReport();
}

static void AuxPower_CancelEditor( void )
{
	if( !g_bAuxPowerEditorActive )
		return;

	g_bAuxPowerEditorActive = 0;
	g_bAuxPowerDragging = 0;
	gEngfuncs.Con_Printf( "[AUX. POWER HUD] Preview discarded; saved placement was kept.\n" );
}

static void AuxPower_StartEditor( void )
{
	int x, y, left, top, width, height, labelX, labelY, frameX, frameY;

	if( g_bAuxPowerEditorActive )
		return;

	AuxPower_GetPosition( &x, &y );
	g_iAuxPowerPreviewX = x;
	g_iAuxPowerPreviewY = y;
	AuxPower_GetLayout( x, y, &left, &top, &width, &height,
		&labelX, &labelY, &frameX, &frameY );
	(void)labelX; (void)labelY; (void)frameX; (void)frameY;

	g_iAuxPowerCursorX = left + width / 2;
	g_iAuxPowerCursorY = top + height / 2;
	g_iAuxPowerMouseButtons = 0;
	g_bAuxPowerDragging = 0;
	g_bAuxPowerShiftDown = 0;
	g_bAuxPowerEditorActive = 1;

	// Release any movement/attack buttons that were held before entering edit mode.
	if( gEngfuncs.pfnClientCmd )
		gEngfuncs.pfnClientCmd( "-forward\n-back\n-moveleft\n-moveright\n-jump\n-duck\n-speed\n-walk\n-attack\n-attack2\n" );

	gEngfuncs.Con_Printf( "\n[AUX. POWER HUD] Editor enabled.\n" );
	gEngfuncs.Con_Printf( "LMB: drag element | arrows: nudge | [ / ]: grid size | S: snap toggle\n" );
	gEngfuncs.Con_Printf( "Enter: commit | Escape: cancel | R: report | hud_auxpower_report: print values\n" );
}

static void AuxPower_EditCommand( void )
{
	int argc = gEngfuncs.Cmd_Argc ? gEngfuncs.Cmd_Argc() : 0;

	if( argc > 1 )
	{
		const char *arg = gEngfuncs.Cmd_Argv ? gEngfuncs.Cmd_Argv( 1 ) : "0";
		if( atoi( arg ) != 0 )
			AuxPower_StartEditor();
		else
			AuxPower_CancelEditor();
	}
	else if( g_bAuxPowerEditorActive )
		AuxPower_CancelEditor();
	else
		AuxPower_StartEditor();
}

static void AuxPower_CommitCommand( void )
{
	AuxPower_CommitEditor();
}

static void AuxPower_CancelCommand( void )
{
	AuxPower_CancelEditor();
}

static void AuxPower_ReportCommand( void )
{
	AuxPower_PrintReport();
}

static void AuxPower_DrawGrid( void )
{
	int grid = AuxPower_GetGrid();
	int stepX = AuxPower_RefToScreenX( grid );
	int stepY = AuxPower_RefToScreenY( grid );
	int x, y;

	if( stepX < 1 ) stepX = 1;
	if( stepY < 1 ) stepY = 1;

	for( x = 0; x < ScreenWidth; x += stepX )
		FillRGBA( x, 0, 1, ScreenHeight, 220, 220, 220, 28 );
	for( y = 0; y < ScreenHeight; y += stepY )
		FillRGBA( 0, y, ScreenWidth, 1, 220, 220, 220, 28 );

	// Stronger center guides make screen alignment easier to judge.
	FillRGBA( ScreenWidth / 2, 0, 1, ScreenHeight, 236, 9, 77, 110 );
	FillRGBA( 0, ScreenHeight / 2, ScreenWidth, 1, 236, 9, 77, 110 );
}

static void AuxPower_DrawEditorOverlay( int x, int y, int width, int height )
{
	int r = 236, g = 9, b = 77;
	int grid = AuxPower_GetGrid();
	int cursorX = g_iAuxPowerCursorX;
	int cursorY = g_iAuxPowerCursorY;
	char status[160];
	int panelWidth = ScreenWidth - 16;
	int panelHeight = gHUD.m_iFontHeight * 2 + 12;

	if( panelWidth < 200 ) panelWidth = 200;

	// Selection outline and corner handles.
	FillRGBA( x, y, width, 1, r, g, b, 255 );
	FillRGBA( x, y + height - 1, width, 1, r, g, b, 255 );
	FillRGBA( x, y, 1, height, r, g, b, 255 );
	FillRGBA( x + width - 1, y, 1, height, r, g, b, 255 );
	FillRGBA( x - 2, y - 2, 5, 5, 255, 255, 255, 255 );
	FillRGBA( x + width - 2, y - 2, 5, 5, 255, 255, 255, 255 );
	FillRGBA( x - 2, y + height - 2, 5, 5, 255, 255, 255, 255 );
	FillRGBA( x + width - 2, y + height - 2, 5, 5, 255, 255, 255, 255 );

	// In-game virtual cursor; input deltas are diverted away from player view control.
	FillRGBA( cursorX - 7, cursorY, 15, 1, 255, 255, 255, 255 );
	FillRGBA( cursorX, cursorY - 7, 1, 15, 255, 255, 255, 255 );

	FillRGBA( 8, 8, panelWidth, panelHeight, 0, 0, 0, 190 );
	gHUD.DrawHudString( 14, 12, ScreenWidth - 8,
		"AUX POWER HUD EDITOR  |  LMB drag  |  arrows nudge  |  S snap  |  Enter apply  |  Esc cancel",
		255, 255, 255 );

	sprintf( status, "X %d  Y %d  GRID %d  SNAP %s  |  R: report",
		g_iAuxPowerPreviewX, g_iAuxPowerPreviewY, grid,
		( CVAR_GET_FLOAT( "hud_auxpower_snap" ) != 0.0f ) ? "ON" : "OFF" );
	gHUD.DrawHudString( 14, 12 + gHUD.m_iFontHeight, ScreenWidth - 8,
		status, r, g, b );
}

static void DrawAuxPowerMeter( float flPower )
{
	int r, g, b;
	int x, y, left, top, width, height;
	int labelX, labelY, frameX, frameY;
	int iEmptyWidth;
	wrect_t rcFill;

	if( flPower < 0.0f )
		flPower = 0.0f;
	else if( flPower > 100.0f )
		flPower = 100.0f;

	// Use the HUD accent normally and the stock HUD red as a low-power warning.
	if( flPower <= 20.0f )
		UnpackRGB( r, g, b, RGB_REDISH );
	else
		UnpackRGB( r, g, b, RGB_YELLOWISH );

	// Match the normal dim HUD treatment used by the battery and flashlight.
	ScaleColors( r, g, b, MIN_ALPHA );

	AuxPower_GetPosition( &x, &y );
	AuxPower_GetLayout( x, y, &left, &top, &width, &height,
		&labelX, &labelY, &frameX, &frameY );

	if( g_bAuxPowerEditorActive )
		AuxPower_DrawGrid();

	if( g_hAuxHudSprite )
	{
		// Keep the fill's left edge fixed; the right edge retreats as power drains.
		iEmptyWidth = (int)( (float)( g_rcAuxHudFill.right - g_rcAuxHudFill.left ) *
			( 1.0f - flPower * 0.01f ) + 0.5f );
		if( iEmptyWidth < 0 )
			iEmptyWidth = 0;
		else if( iEmptyWidth > g_rcAuxHudFill.right - g_rcAuxHudFill.left )
			iEmptyWidth = g_rcAuxHudFill.right - g_rcAuxHudFill.left;

		rcFill = g_rcAuxHudFill;
		rcFill.right -= iEmptyWidth;
		if( rcFill.right > rcFill.left )
		{
			SPR_Set( g_hAuxHudSprite, r, g, b );
			// The artwork's fill sits 4 px right and 3 px down inside the frame.
			SPR_DrawAdditive( 0, frameX + 4, frameY + 3, &rcFill );
		}

		// Draw the frame last so its outline stays crisp over the fill.
		SPR_Set( g_hAuxHudSprite, r, g, b );
		SPR_DrawAdditive( 0, frameX, frameY, &g_rcAuxHudFrame );
	}

	if( g_bAuxPowerEditorActive )
		AuxPower_DrawEditorOverlay( left, top, width, height );
}

// Xash3D input hooks call these while the debug editor owns the mouse.
extern "C" int DLLEXPORT HUD_AuxPowerEditorIsActive( void )
{
	return g_bAuxPowerEditorActive;
}

extern "C" void DLLEXPORT HUD_AuxPowerEditorMouseMove( float deltaX, float deltaY )
{
	int maxX, maxY;

	if( !g_bAuxPowerEditorActive )
		return;

	g_iAuxPowerCursorX += (int)( deltaX + ( deltaX >= 0.0f ? 0.5f : -0.5f ) );
	g_iAuxPowerCursorY += (int)( deltaY + ( deltaY >= 0.0f ? 0.5f : -0.5f ) );

	maxX = ScreenWidth - 1;
	maxY = ScreenHeight - 1;
	if( g_iAuxPowerCursorX < 0 ) g_iAuxPowerCursorX = 0;
	if( g_iAuxPowerCursorY < 0 ) g_iAuxPowerCursorY = 0;
	if( g_iAuxPowerCursorX > maxX ) g_iAuxPowerCursorX = maxX;
	if( g_iAuxPowerCursorY > maxY ) g_iAuxPowerCursorY = maxY;

	if( g_bAuxPowerDragging )
	{
		int x = AuxPower_ScreenToRefX( g_iAuxPowerCursorX - g_iAuxPowerDragOffsetX );
		int y = AuxPower_ScreenToRefY( g_iAuxPowerCursorY - g_iAuxPowerDragOffsetY );
		AuxPower_SetPreviewPosition( x, y );
	}
}

extern "C" void DLLEXPORT HUD_AuxPowerEditorMouseButtons( int buttons )
{
	int left, top, width, height, labelX, labelY, frameX, frameY;
	int leftDown = ( buttons & 1 ) != 0;
	int wasLeftDown = ( g_iAuxPowerMouseButtons & 1 ) != 0;

	if( !g_bAuxPowerEditorActive )
		return;

	if( leftDown && !wasLeftDown )
	{
		AuxPower_GetLayout( g_iAuxPowerPreviewX, g_iAuxPowerPreviewY,
			&left, &top, &width, &height, &labelX, &labelY, &frameX, &frameY );
		if( g_iAuxPowerCursorX >= left && g_iAuxPowerCursorX <= left + width &&
			g_iAuxPowerCursorY >= top && g_iAuxPowerCursorY <= top + height )
		{
			g_bAuxPowerDragging = 1;
			g_iAuxPowerDragOffsetX = g_iAuxPowerCursorX - left;
			g_iAuxPowerDragOffsetY = g_iAuxPowerCursorY - top;
		}
	}
	else if( !leftDown && wasLeftDown )
		g_bAuxPowerDragging = 0;

	g_iAuxPowerMouseButtons = buttons;
}

extern "C" void DLLEXPORT HUD_AuxPowerEditorKeyEvent( int down, int keynum )
{
	int step;

	if( !g_bAuxPowerEditorActive )
		return;

	if( keynum == K_SHIFT )
	{
		g_bAuxPowerShiftDown = down;
		return;
	}
	if( !down )
		return;

	switch( keynum )
	{
	case K_ENTER:
	case K_KP_ENTER:
		AuxPower_CommitEditor();
		return;
	case K_ESCAPE:
		AuxPower_CancelEditor();
		return;
	case K_LEFTARROW:
	case K_RIGHTARROW:
	case K_UPARROW:
	case K_DOWNARROW:
		step = g_bAuxPowerShiftDown ? 1 : AuxPower_GetGrid();
		if( keynum == K_LEFTARROW )
			AuxPower_SetPreviewPosition( g_iAuxPowerPreviewX - step, g_iAuxPowerPreviewY, g_bAuxPowerShiftDown );
		else if( keynum == K_RIGHTARROW )
			AuxPower_SetPreviewPosition( g_iAuxPowerPreviewX + step, g_iAuxPowerPreviewY, g_bAuxPowerShiftDown );
		else if( keynum == K_UPARROW )
			AuxPower_SetPreviewPosition( g_iAuxPowerPreviewX, g_iAuxPowerPreviewY - step, g_bAuxPowerShiftDown );
		else
			AuxPower_SetPreviewPosition( g_iAuxPowerPreviewX, g_iAuxPowerPreviewY + step, g_bAuxPowerShiftDown );
		return;
	case '[':
		if( gEngfuncs.Cvar_SetValue )
			gEngfuncs.Cvar_SetValue( "hud_auxpower_grid", (float)( AuxPower_GetGrid() - 1 ) );
		return;
	case ']':
		if( gEngfuncs.Cvar_SetValue )
			gEngfuncs.Cvar_SetValue( "hud_auxpower_grid", (float)( AuxPower_GetGrid() + 1 ) );
		return;
	case 's':
		if( gEngfuncs.Cvar_SetValue )
			gEngfuncs.Cvar_SetValue( "hud_auxpower_snap",
				( CVAR_GET_FLOAT( "hud_auxpower_snap" ) != 0.0f ) ? 0.0f : 1.0f );
		return;
	case 'r':
		AuxPower_PrintReport();
		return;
	}
}

// Returns back a color from the Green <-> Yellow <-> Red ramp.
int CHudHealth::Draw( float flTime )
{
	int r, g, b;
	int a = 0, x, y;
	int HealthWidth;

	if( ( gHUD.m_iHideHUDDisplay & HIDEHUD_HEALTH ) || gEngfuncs.IsSpectateOnly() )
		return 1;

	if( !m_hSprite )
		m_hSprite = LoadSprite( PAIN_NAME );

	// Has health changed? Flash the health #
	if( m_fFade )
	{
		m_fFade -= ( (float)gHUD.m_flTimeDelta * 20.0f );
		if( m_fFade <= 0 )
		{
			a = MIN_ALPHA;
			m_fFade = 0;
		}

		// Fade the health number back to dim
		a = MIN_ALPHA + ( m_fFade / FADE_TIME ) * 128;
	}
	else
		a = MIN_ALPHA;

	// If health is getting low, make it bright red
	if( m_iHealth <= 15 )
		a = 255;

	GetPainColor( r, g, b );
	ScaleColors( r, g, b, a );

	// Only draw health if we have the suit.
	if( gHUD.m_iWeaponBits & ( 1 << ( WEAPON_SUIT ) ) )
	{
		HealthWidth = gHUD.GetSpriteRect( gHUD.m_HUD_number_0 ).right - gHUD.GetSpriteRect( gHUD.m_HUD_number_0 ).left;
		int CrossWidth = gHUD.GetSpriteRect( m_HUD_cross ).right - gHUD.GetSpriteRect( m_HUD_cross ).left;

		y = ScreenHeight - gHUD.m_iFontHeight - gHUD.m_iFontHeight / 2;
		x = CrossWidth / 2;

		SPR_Set( gHUD.GetSprite( m_HUD_cross ), r, g, b );
		SPR_DrawAdditive( 0, x, y, &gHUD.GetSpriteRect( m_HUD_cross ) );

		x = CrossWidth + HealthWidth / 2;

		x = gHUD.DrawHudNumber( x, y + gHUD.m_iHudNumbersYOffset, DHN_3DIGITS | DHN_DRAWZERO, m_iHealth, r, g, b );

		x += HealthWidth / 2;

		int iHeight = gHUD.m_iFontHeight;
		int iWidth = HealthWidth / 10;
		UnpackRGB( r, g, b, RGB_YELLOWISH );
		FillRGBA( x, y + gHUD.m_iHudNumbersYOffset, iWidth, iHeight, r, g, b, a );

		// Use the post-prediction local state, which contains the
		// movement DLL's final fuser4 value for this user command.
		float flAuxPower = ( g_flHL2AuxPowerPostRun >= 0.0f ) ?
			g_flHL2AuxPowerPostRun : g_flHL2AuxPower;
		DrawAuxPowerMeter( flAuxPower );
	}

	DrawDamage( flTime );
	return DrawPain( flTime );
}

void CHudHealth::CalcDamageDirection( vec3_t vecFrom )
{
	vec3_t forward, right, up;
	float side, front;
	vec3_t vecOrigin, vecAngles;

	if( !vecFrom[0] && !vecFrom[1] && !vecFrom[2] )
	{
		m_fAttackFront = m_fAttackRear = m_fAttackRight = m_fAttackLeft = 0;
		return;
	}

	memcpy( vecOrigin, gHUD.m_vecOrigin, sizeof(vec3_t) );
	memcpy( vecAngles, gHUD.m_vecAngles, sizeof(vec3_t) );

	VectorSubtract( vecFrom, vecOrigin, vecFrom );

	float flDistToTarget = vecFrom.Length();

	vecFrom = vecFrom.Normalize();
	AngleVectors( vecAngles, forward, right, up );

	front = DotProduct( vecFrom, right );
	side = DotProduct( vecFrom, forward );

	if( flDistToTarget <= 50 )
	{
		m_fAttackFront = m_fAttackRear = m_fAttackRight = m_fAttackLeft = 1;
	}
	else 
	{
		if( side > 0.0f )
		{
			if( side > 0.3f )
				m_fAttackFront = Q_max( m_fAttackFront, side );
		}
		else
		{
			float f = fabs( side );
			if( f > 0.3f )
				m_fAttackRear = Q_max( m_fAttackRear, f );
		}

		if( front > 0.0f )
		{
			if( front > 0.3f )
				m_fAttackRight = Q_max( m_fAttackRight, front );
		}
		else
		{
			float f = fabs( front );
			if( f > 0.3f )
				m_fAttackLeft = Q_max( m_fAttackLeft, f );
		}
	}
}

int CHudHealth::DrawPain( float flTime )
{
	if( !( m_fAttackFront || m_fAttackRear || m_fAttackLeft || m_fAttackRight) )
		return 1;

	int r, g, b;
	int x, y, a, shade;

	// TODO:  get the shift value of the health
	a = 255;	// max brightness until then

	float fFade = gHUD.m_flTimeDelta * 2;

	// SPR_Draw top
	if( m_fAttackFront > 0.4f )
	{
		GetPainColor( r, g, b );
		shade = a * Q_max( m_fAttackFront, 0.5f );
		ScaleColors( r, g, b, shade );
		SPR_Set( m_hSprite, r, g, b );

		x = ScreenWidth / 2 - SPR_Width( m_hSprite, 0 ) / 2;
		y = ScreenHeight / 2 - SPR_Height( m_hSprite, 0 ) * 3;
		SPR_DrawAdditive( 0, x, y, NULL );
		m_fAttackFront = Q_max( 0, m_fAttackFront - fFade );
	} else
		m_fAttackFront = 0;

	if( m_fAttackRight > 0.4f )
	{
		GetPainColor( r, g, b );
		shade = a * Q_max( m_fAttackRight, 0.5f );
		ScaleColors( r, g, b, shade );
		SPR_Set( m_hSprite, r, g, b );

		x = ScreenWidth / 2 + SPR_Width( m_hSprite, 1 ) * 2;
		y = ScreenHeight / 2 - SPR_Height( m_hSprite,1 ) / 2;
		SPR_DrawAdditive( 1, x, y, NULL );
		m_fAttackRight = Q_max( 0, m_fAttackRight - fFade );
	}
	else
		m_fAttackRight = 0;

	if( m_fAttackRear > 0.4f )
	{
		GetPainColor( r, g, b );
		shade = a * Q_max( m_fAttackRear, 0.5f );
		ScaleColors( r, g, b, shade );
		SPR_Set( m_hSprite, r, g, b );

		x = ScreenWidth / 2 - SPR_Width( m_hSprite, 2 ) / 2;
		y = ScreenHeight / 2 + SPR_Height( m_hSprite, 2 ) * 2;
		SPR_DrawAdditive( 2, x, y, NULL );
		m_fAttackRear = Q_max( 0, m_fAttackRear - fFade );
	}
	else
		m_fAttackRear = 0;

	if( m_fAttackLeft > 0.4f )
	{
		GetPainColor( r, g, b );
		shade = a * Q_max( m_fAttackLeft, 0.5f );
		ScaleColors( r, g, b, shade );
		SPR_Set( m_hSprite, r, g, b );

		x = ScreenWidth / 2 - SPR_Width( m_hSprite, 3 ) * 3;
		y = ScreenHeight / 2 - SPR_Height( m_hSprite,3 ) / 2;
		SPR_DrawAdditive( 3, x, y, NULL );

		m_fAttackLeft = Q_max( 0, m_fAttackLeft - fFade );
	} else
		m_fAttackLeft = 0;

	return 1;
}

int CHudHealth::DrawDamage( float flTime )
{
	int i, r, g, b, a;
	DAMAGE_IMAGE *pdmg;

	if( !m_bitsDamage )
		return 1;

	UnpackRGB( r, g, b, RGB_YELLOWISH );

	a = (int)( fabs( sin( flTime * 2.0f ) ) * 256.0f );

	ScaleColors( r, g, b, a );

	// check for bits that should be expired
	for( i = 0; i < NUM_DMG_TYPES; i++ )
	{
		if( m_bitsDamage & giDmgFlags[i] )
		{
			pdmg = &m_dmg[i];

			// Draw all the items
			SPR_Set( gHUD.GetSprite( m_HUD_dmg_bio + i ), r, g, b );
			SPR_DrawAdditive( 0, pdmg->x, pdmg->y, &gHUD.GetSpriteRect( m_HUD_dmg_bio + i ) );

			pdmg->fExpire = Q_min( flTime + DMG_IMAGE_LIFE, pdmg->fExpire );

			if( pdmg->fExpire <= flTime		// when the time has expired
				&& a < 40 )						// and the flash is at the low point of the cycle
			{
				pdmg->fExpire = 0;

				int y = pdmg->y;
				pdmg->x = pdmg->y = 0;

				// move everyone above down
				for( int j = 0; j < NUM_DMG_TYPES; j++ )
				{
					pdmg = &m_dmg[j];
					if( ( pdmg->y ) && ( pdmg->y < y ) )
						pdmg->y += giDmgHeight;

				}

				m_bitsDamage &= ~giDmgFlags[i];  // clear the bits
			}
		}
	}

	return 1;
}

void CHudHealth::UpdateTiles( float flTime, long bitsDamage )
{	
	DAMAGE_IMAGE *pdmg;

	// Which types are new?
	long bitsOn = ~m_bitsDamage & bitsDamage;

	for( int i = 0; i < NUM_DMG_TYPES; i++ )
	{
		pdmg = &m_dmg[i];

		// Is this one already on?
		if( m_bitsDamage & giDmgFlags[i] )
		{
			pdmg->fExpire = flTime + DMG_IMAGE_LIFE; // extend the duration
			if( !pdmg->fBaseline )
				pdmg->fBaseline = flTime;
		}

		// Are we just turning it on?
		if( bitsOn & giDmgFlags[i] )
		{
			// put this one at the bottom
			pdmg->x = giDmgWidth / 8;
			pdmg->y = ScreenHeight - giDmgHeight * 2;
			pdmg->fExpire=flTime + DMG_IMAGE_LIFE;

			// move everyone else up
			for( int j = 0; j < NUM_DMG_TYPES; j++ )
			{
				if( j == i )
					continue;

				pdmg = &m_dmg[j];
				if( pdmg->y )
					pdmg->y -= giDmgHeight;
			}
			// pdmg = &m_dmg[i];
		}
	}

	// damage bits are only turned on here;  they are turned off when the draw time has expired (in DrawDamage())
	m_bitsDamage |= bitsDamage;
}

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
// hud_redraw.cpp
//
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hud.h"
#include "game_instructor.h"
#include "cl_util.h"
#include "cl_entity.h"
#include "../common/event_api.h"
#include "../pm_shared/pm_defs.h"
#include "triangleapi.h"

#include "vgui_TeamFortressViewport.h"

#define MAX_LOGO_FRAMES 56

int grgLogoFrame[MAX_LOGO_FRAMES] =
{
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 13, 13, 13, 13, 13, 12, 11, 10, 9, 8, 14, 15,
	16, 17, 18, 19, 20, 20, 20, 20, 20, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 
	29, 29, 29, 29, 29, 28, 27, 26, 25, 24, 30, 31 
};

extern int g_iVisibleMouse;

float HUD_GetFOV( void );

extern cvar_t *sensitivity;

// Updated by V_CalcRefdef; this is the actual camera origin, matching
// Source's MainViewOrigin() rather than the player's feet position.
extern vec3_t v_origin;

// Source hud_locator_target.cpp: ICON_DIST_TOO_FAR = 60 feet * 12 inches.
#define INSTRUCTOR_LOCATOR_CAPTION_MAX_DISTANCE (60.0f * 12.0f)
// All Instructor sprites share the same master multiplier. Do not add a
// second base multiplier here: that made locator icons twice the size of
// equal-sized key badges at the same hud_instructor_scale value.
#define INSTRUCTOR_LOCATOR_ICON_BASE_SCALE 1.0f

static float HUD_GetInstructorCaptionScale( void );
static float HUD_GetInstructorScale( void );
static int HUD_ScaleInstructorPixel( int value );
static void HUD_GetInstructorSpriteSize( HSPRITE sprite, float baseScale,
	int *width, int *height );
static int HUD_GetInstructorKeyTextScale( void );
static int HUD_GetInstructorCaptionWidth( const char *text );
static int HUD_GetInstructorCaptionHeight( void );
static int HUD_DrawInstructorCaption( int x, int y, int maxX,
	const char *text, int r, int g, int b );

// Source AnimateIconSize keeps the same absolute height increase for wide icons.
static void HUD_AnimateInstructorIconSize( const CInstructorLocatorTarget *target,
	int *wide, int *tall )
{
	if( !target || !wide || !tall || *wide <= 0 || *tall <= 0 ) return;
	float scale = target->m_flPulseScale;
	if( *wide > *tall )
	{
		const int heightDelta = (int)( (float)( *tall ) * scale - (float)( *tall ) );
		scale = (float)( *wide + heightDelta ) / (float)( *wide );
	}
	*wide = (int)( (float)( *wide ) * scale );
	*tall = (int)( (float)( *tall ) * scale );
}

// Draw a thin HUD-space tether between the real entity anchor and the
// optional icon-height offset. The short Bresenham walk avoids introducing
// a VGUI dependency into the GoldSrc client HUD.
static void HUD_DrawInstructorLocatorTether( int x0, int y0, int x1, int y1,
	int r, int g, int b, int alpha )
{
	int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
	int sx = x0 < x1 ? 1 : -1;
	int dy = y1 >= y0 ? -( y1 - y0 ) : -( y0 - y1 );
	int sy = y0 < y1 ? 1 : -1;
	int error = dx + dy;

	// A very large projected offset should not become a per-frame fill storm.
	int steps = dx > -dy ? dx : -dy;
	if( steps > 256 )
		return;

	for( ;; )
	{
		FillRGBA( x0, y0, HUD_ScaleInstructorPixel( 2 ),
			HUD_ScaleInstructorPixel( 2 ), 0, 0, 0, alpha / 2 );
		FillRGBA( x0, y0, HUD_ScaleInstructorPixel( 1 ),
			HUD_ScaleInstructorPixel( 1 ), r, g, b, alpha );
		if( x0 == x1 && y0 == y1 )
			break;

		const int doubledError = 2 * error;
		if( doubledError >= dy )
		{
			error += dy;
			x0 += sx;
		}
		if( doubledError <= dx )
		{
			error += dx;
			y0 += sy;
		}
	}
}

// Resolve a world point to HUD viewport coordinates. TriAPI's WorldToScreen
// returns normalized device coordinates and reports points behind the view
// separately; convert to pixels and mirror behind-camera directions so the
// offscreen ray points toward the target rather than through the camera.
static bool HUD_GetInstructorHintScreenPosition( const float *worldOrigin, float *screenX, float *screenY, bool *behind )
{
	if( !worldOrigin || !screenX || !screenY || !behind || !gEngfuncs.pTriAPI ||
		!gEngfuncs.pTriAPI->WorldToScreen )
		return false;

	float world[3] =
	{
		worldOrigin[0],
		worldOrigin[1],
		worldOrigin[2]
	};
	float projected[3] = { 0.0f, 0.0f, 0.0f };
	const int zClipped = gEngfuncs.pTriAPI->WorldToScreen( world, projected );

	// Xash3D's TriAPI WorldToScreen returns normalized device coordinates
	// (-1..+1), not HUD pixels. Convert NDC to the top-left-origin pixel
	// coordinates used by SPR_Draw/FillRGBA and the rest of this HUD.
	*screenX = ( 1.0f + projected[0] ) * 0.5f * (float)ScreenWidth;
	*screenY = ( 1.0f - projected[1] ) * 0.5f * (float)ScreenHeight;
	*behind = ( zClipped != 0 );

	if( *behind )
	{
		// WorldToScreen cannot divide by a negative clip-space W. Its raw
		// NDC direction is reflected around the viewport center for edge placement.
		*screenX = (float)ScreenWidth - *screenX;
		*screenY = (float)ScreenHeight - *screenY;

		// A target directly behind the player can project to the exact center.
		// Recover a useful direction from the camera basis in that edge case.
		if( fabsf( *screenX - ScreenWidth * 0.5f ) < 2.0f &&
			fabsf( *screenY - ScreenHeight * 0.5f ) < 2.0f &&
			gEngfuncs.GetViewAngles && gEngfuncs.pfnAngleVectors )
		{
			cl_entity_t *local = gEngfuncs.GetLocalPlayer ? gEngfuncs.GetLocalPlayer() : NULL;
			if( local )
			{
				float angles[3] = { 0.0f, 0.0f, 0.0f };
				float forward[3], right[3], up[3];
				gEngfuncs.GetViewAngles( angles );
				gEngfuncs.pfnAngleVectors( angles, forward, right, up );

				float delta[3] =
				{
					worldOrigin[0] - local->origin[0],
					worldOrigin[1] - local->origin[1],
					worldOrigin[2] - local->origin[2]
				};
				const float side = delta[0] * right[0] + delta[1] * right[1] + delta[2] * right[2];
				const float vertical = delta[0] * up[0] + delta[1] * up[1] + delta[2] * up[2];

				*screenX = ScreenWidth * 0.5f - side;
				*screenY = ScreenHeight * 0.5f + vertical;
			}
		}
	}

	// Reject invalid or absurd coordinates before integer conversion/clamping.
	if( *screenX != *screenX || *screenY != *screenY ||
		fabsf( *screenX ) > 1000000.0f || fabsf( *screenY ) > 1000000.0f )
		return false;

	return true;
}

// Reproduce Source GetTargetPosition's horizontal camera-relative bearing.
// The locator compass is intentionally independent of the target's pitch/elevation.
static bool HUD_GetInstructorCompassDirection( const float *worldOrigin,
	float *sinYaw, float *cosYaw )
{
	if( !worldOrigin || !sinYaw || !cosYaw ||
		!gEngfuncs.GetViewAngles || !gEngfuncs.pfnAngleVectors )
		return false;

	float angles[3] = { 0.0f, 0.0f, 0.0f };
	float forward[3], right[3], up[3];
	gEngfuncs.GetViewAngles( angles );
	gEngfuncs.pfnAngleVectors( angles, forward, right, up );

	forward[2] = 0.0f;
	const float forwardLength = sqrtf( forward[0] * forward[0] +
		forward[1] * forward[1] );
	if( forwardLength < 0.0001f )
		return false;
	forward[0] /= forwardLength;
	forward[1] /= forwardLength;

	// Source constructs right = CrossProduct( worldUp, flattenedForward ).
	right[0] = -forward[1];
	right[1] = forward[0];
	right[2] = 0.0f;

	const float delta[3] =
	{
		worldOrigin[0] - v_origin[0],
		worldOrigin[1] - v_origin[1],
		worldOrigin[2] - v_origin[2]
	};
	const float front = delta[0] * forward[0] +
		delta[1] * forward[1];
	const float side = delta[0] * right[0] +
		delta[1] * right[1];

	const float rotation = atan2f( -side, -front ) +
		3.14159265358979323846f;
	const float yawRadians = -rotation;
	*sinYaw = sinf( yawRadians );
	*cosYaw = cosf( yawRadians );
	return true;
}

// Source-style client locator visibility trace. Map-authored env_instructor_hint
// already receives this result from the server; this fills the gap for lessons
// scheduled entirely by the client-side GameInstructor manager.
static bool HUD_IsInstructorHintOccluded( const float *targetOrigin, int targetEntity )
{
	if( !targetOrigin || targetEntity <= 0 || !gEngfuncs.pEventAPI ||
		!gEngfuncs.GetLocalPlayer )
		return false;

	event_api_t *eventApi = gEngfuncs.pEventAPI;
	if( !eventApi->EV_LocalPlayerViewheight || !eventApi->EV_SetUpPlayerPrediction ||
		!eventApi->EV_PushPMStates || !eventApi->EV_PopPMStates ||
		!eventApi->EV_SetSolidPlayers || !eventApi->EV_SetTraceHull ||
		!eventApi->EV_PlayerTrace || !eventApi->EV_IndexFromTrace )
		return false;

	cl_entity_t *localPlayer = gEngfuncs.GetLocalPlayer();
	if( !localPlayer || localPlayer->index <= 0 || localPlayer->index == targetEntity )
		return false;

	float viewHeight[3] = { 0.0f, 0.0f, 0.0f };
	eventApi->EV_LocalPlayerViewheight( viewHeight );

	float traceStart[3] =
	{
		localPlayer->origin[0] + viewHeight[0],
		localPlayer->origin[1] + viewHeight[1],
		localPlayer->origin[2] + viewHeight[2]
	};
	float traceEnd[3] =
	{
		targetOrigin[0], targetOrigin[1], targetOrigin[2]
	};

	pmtrace_t trace = {};
	eventApi->EV_SetUpPlayerPrediction( 0, 1 );
	eventApi->EV_PushPMStates();
	eventApi->EV_SetSolidPlayers( localPlayer->index - 1 );
	eventApi->EV_SetTraceHull( 0 );
	eventApi->EV_PlayerTrace( traceStart, traceEnd, PM_STUDIO_BOX, -1, &trace );
	eventApi->EV_PopPMStates();

	const int hitEntity = eventApi->EV_IndexFromTrace( &trace );
	return trace.startsolid ||
		( trace.fraction < 1.0f && hitEntity != targetEntity );
}

// Set sprite modulation and (when supported) retain full RGB so the engine
// can apply the requested opacity through its actual sprite alpha channel.
static void HUD_SetInstructorLocatorSpriteColor( HSPRITE sprite,
	int r, int g, int b, int alpha )
{
	if( !sprite )
		return;

	if( gEngfuncs.pfnSPR_SetAlpha )
		SPR_Set( sprite, r, g, b );
	else
		SPR_Set( sprite, r * alpha / 255, g * alpha / 255, b * alpha / 255 );
}

// Draw a HUD locator sprite at a requested size. The Xash extension sets the
// per-sprite alpha used by SPR_DrawGeneric, so its fade matches Source VGUI's
// Color( r, g, b, alpha ) instead of simulating opacity by darkening RGB.
static bool HUD_DrawInstructorLocatorSprite( HSPRITE sprite, int x, int y,
	int nativeWidth, int nativeHeight, int width, int height,
	int renderMode, bool additiveFallback, bool flipX = false, bool flipY = false,
	int alpha = 255 )
{
	if( !sprite )
		return false;

	if( gEngfuncs.pfnSPR_DrawGeneric && gEngfuncs.pTriAPI &&
		gEngfuncs.pTriAPI->RenderMode )
	{
		int drawX = HUD_AdjustX( x );
		int drawY = HUD_AdjustY( y );
		int drawWidth = width;
		int drawHeight = height;

		// Xash3D's generic sprite path forwards signed dimensions to its
		// textured-quad renderer. Moving the origin to the opposite edge and
		// negating one dimension mirrors the texture without another .spr.
		if( flipX )
		{
			drawX += drawWidth;
			drawWidth = -drawWidth;
		}
		if( flipY )
		{
			drawY += drawHeight;
			drawHeight = -drawHeight;
		}

		gEngfuncs.pTriAPI->RenderMode( renderMode );
		// SPR_SetAlpha controls the alpha component of the current sprite color.
		// Set it after SPR_Set/color modulation and immediately before drawing.
		if( gEngfuncs.pfnSPR_SetAlpha )
			gEngfuncs.pfnSPR_SetAlpha( alpha );
		gEngfuncs.pfnSPR_DrawGeneric( 0, drawX, drawY,
			NULL, 0, 0, drawWidth, drawHeight );
		gEngfuncs.pTriAPI->RenderMode( kRenderNormal );
		if( gEngfuncs.pfnSPR_SetAlpha )
			gEngfuncs.pfnSPR_SetAlpha( 255 );
		return true;
	}

	// Older engines cannot stretch or mirror through the generic sprite API.
	// Keep the native-size fallback for ordinary sprites; mirrored directions
	// are skipped so that a backwards-facing arrow is never shown by mistake.
	if( flipX || flipY )
		return false;

	x += ( width - nativeWidth ) / 2;
	y += ( height - nativeHeight ) / 2;
	if( gEngfuncs.pfnSPR_SetAlpha )
		gEngfuncs.pfnSPR_SetAlpha( alpha );
	if( additiveFallback )
		SPR_DrawAdditive( 0, x, y, NULL );
	else
		SPR_DrawHoles( 0, x, y, NULL );
	if( gEngfuncs.pfnSPR_SetAlpha )
		gEngfuncs.pfnSPR_SetAlpha( 255 );
	return true;
}

// Return whether a lesson binding is an engine command token rather than a
// literal display label. Source lesson files normally use values such as
// "+use" and "+attack"; plain text such as "E" remains an explicit override.
static bool HUD_IsInstructorBindingCommand( const char *binding, const char **command )
{
	if( !binding || !binding[0] || !command )
		return false;

	if( binding[0] == '+' )
	{
		*command = binding + 1;
		return true;
	}

	static const char *knownCommands[] =
	{
		"use", "jump", "attack", "attack2", "reload", "forward", "back",
		"moveleft", "moveright", "left", "right", "duck", "speed", "walk",
		"strafe", "showscores", "mlook", "jlook"
	};

	for( size_t i = 0; i < sizeof( knownCommands ) / sizeof( knownCommands[0] ); ++i )
	{
		const char *a = binding;
		const char *b = knownCommands[i];
		while( *a && *b && toupper( (unsigned char)*a ) == toupper( (unsigned char)*b ) )
		{
			++a;
			++b;
		}
		if( !*a && !*b )
		{
			*command = binding;
			return true;
		}
	}

	return false;
}

// Convert engine key names into compact, readable labels. One- and two-key
// labels can use icon_key_generic; longer names retain the drawn key-chip.
static void HUD_FormatInstructorKeyName( const char *keyName, char *output, size_t outputSize )
{
	if( !output || outputSize == 0 )
		return;
	output[0] = '\0';
	if( !keyName || !keyName[0] )
		return;

	char upper[32];
	size_t length = 0;
	while( keyName[length] && length < sizeof( upper ) - 1 )
	{
		upper[length] = (char)toupper( (unsigned char)keyName[length] );
		++length;
	}
	upper[length] = '\0';

	struct KeyLabel
	{
		const char *engineName;
		const char *displayName;
	};
	static const KeyLabel labels[] =
	{
		{ "SPACE", "SPACE" },
		{ "SEMICOLON", ";" },
		{ "ENTER", "ENTER" },
		{ "ESCAPE", "ESC" },
		{ "BACKSPACE", "BKSP" },
		{ "TAB", "TAB" },
		{ "UPARROW", "UP" },
		{ "DOWNARROW", "DN" },
		{ "LEFTARROW", "LT" },
		{ "RIGHTARROW", "RT" },
		{ "MOUSE1", "M1" },
		{ "MOUSE2", "M2" },
		{ "MOUSE3", "M3" },
		{ "MOUSE4", "M4" },
		{ "MOUSE5", "M5" },
		{ "MWHEELUP", "MW+" },
		{ "MWHEELDOWN", "MW-" },
		{ "CTRL", "CTRL" },
		{ "SHIFT", "SHIFT" },
		{ "ALT", "ALT" },
		{ "CAPSLOCK", "CAPS" },
		{ "SCROLLOCK", "SCRL" },
		{ "INS", "INS" },
		{ "DEL", "DEL" },
		{ "PGUP", "PGUP" },
		{ "PGDN", "PGDN" },
		{ "KP_HOME", "KP7" },
		{ "KP_UPARROW", "KP8" },
		{ "KP_PGUP", "KP9" },
		{ "KP_LEFTARROW", "KP4" },
		{ "KP_5", "KP5" },
		{ "KP_RIGHTARROW", "KP6" },
		{ "KP_END", "KP1" },
		{ "KP_DOWNARROW", "KP2" },
		{ "KP_PGDN", "KP3" },
		{ "KP_ENTER", "KP ENT" },
		{ "KP_INS", "KP0" },
		{ "KP_DEL", "KP." },
		{ "KP_SLASH", "KP/" },
		{ "KP_MINUS", "KP-" },
		{ "KP_PLUS", "KP+" },
		// Xash3D gamepad button names returned by Key_LookupBinding.
		{ "A_BUTTON", "A" },
		{ "B_BUTTON", "B" },
		{ "X_BUTTON", "X" },
		{ "Y_BUTTON", "Y" },
		{ "BACK", "BACK" },
		{ "MODE", "MODE" },
		{ "START", "START" },
		{ "STICK1", "L3" },
		{ "STICK2", "R3" },
		{ "L1_BUTTON", "LB" },
		{ "R1_BUTTON", "RB" },
		{ "L2_BUTTON", "LT" },
		{ "R2_BUTTON", "RT" },
		{ "LTRIGGER", "LT" },
		{ "RTRIGGER", "RT" },
		{ "DPAD_UP", "DU" },
		{ "DPAD_DOWN", "DD" },
		{ "DPAD_LEFT", "DL" },
		{ "DPAD_RIGHT", "DR" }
	};

	for( size_t i = 0; i < sizeof( labels ) / sizeof( labels[0] ); ++i )
	{
		if( strcmp( upper, labels[i].engineName ) == 0 )
		{
			snprintf( output, outputSize, "%s", labels[i].displayName );
			return;
		}
	}

	// Printable letter keys are returned in lower case by the engine; show
	// them in upper case to match the generic keycap artwork.
	snprintf( output, outputSize, "%s", upper );
}

// Resolve "+use" / "+jump" every frame so changed player key bindings are
// reflected immediately. The engine API returns the key name, not the command.
// The multi-choice locator path uses Key_LookupBindingEx when available;
// older engines retain the first-key fallback and literal labels stay intact.
static void HUD_ResolveInstructorBinding( const char *binding, char *output, size_t outputSize )
{
	if( !output || outputSize == 0 )
		return;
	output[0] = '\0';
	if( !binding || !binding[0] )
		return;

	const char *command = NULL;
	if( !HUD_IsInstructorBindingCommand( binding, &command ) )
	{
		snprintf( output, outputSize, "%s", binding );
		return;
	}

	char commandName[64];
	size_t commandLength = 0;
	while( command[commandLength] &&
		( isalnum( (unsigned char)command[commandLength] ) ||
		  command[commandLength] == '_' ) &&
		commandLength < sizeof( commandName ) - 1 )
	{
		commandName[commandLength] = command[commandLength];
		++commandLength;
	}
	commandName[commandLength] = '\0';

	if( commandName[0] && gEngfuncs.Key_LookupBinding )
	{
		const char *keyName = gEngfuncs.Key_LookupBinding( commandName );
		if( keyName && keyName[0] &&
			strncmp( keyName, "<KEY", 4 ) != 0 &&
			strncmp( keyName, "<OUT", 4 ) != 0 )
		{
			HUD_FormatInstructorKeyName( keyName, output, outputSize );
			if( output[0] )
				return;
		}
	}

	// Keep an intelligible prompt when an action is currently unbound instead
	// of showing a stale token such as "+use".
	struct KeyLabelFallback
	{
		const char *command;
		const char *displayName;
	};
	static const KeyLabelFallback labels[] =
	{
		{ "use", "USE" },
		{ "jump", "JUMP" },
		{ "attack", "ATTACK" },
		{ "attack2", "ALT FIRE" },
		{ "reload", "RELOAD" },
		{ "forward", "FORWARD" },
		{ "back", "BACK" },
		{ "moveleft", "LEFT" },
		{ "moveright", "RIGHT" },
		{ "left", "LEFT" },
		{ "right", "RIGHT" },
		{ "duck", "CROUCH" },
		{ "speed", "RUN" },
		{ "walk", "WALK" },
		{ "showscores", "SCORES" }
	};
	for( size_t i = 0; i < sizeof( labels ) / sizeof( labels[0] ); ++i )
	{
		const char *a = commandName;
		const char *b = labels[i].command;
		while( *a && *b && toupper( (unsigned char)*a ) == toupper( (unsigned char)*b ) )
		{
			++a;
			++b;
		}
		if( !*a && !*b )
		{
			snprintf( output, outputSize, "%s", labels[i].displayName );
			return;
		}
	}

	HUD_FormatInstructorKeyName( commandName, output, outputSize );
}


// Source CLocatorTarget::SetBinding / UseBindingImage / DrawBindingName port.
// Enumerate all bound keys, preserve Source's 0.75-second cycling interval,
// and use the matching key art rather than a small inline badge.
static void HUD_AddInstructorBindingChoice( CInstructorLocatorTarget *target, const char *keyName )
{
	if( !target || !keyName || !keyName[0] ||
		target->m_iBindingChoicesCount >= MAX_INSTRUCTOR_LOCATOR_BINDINGS )
		return;
	for( int i = 0; i < target->m_iBindingChoicesCount; ++i )
		if( stricmp( target->m_szBindingChoices[i], keyName ) == 0 )
			return;
	char *choice = target->m_szBindingChoices[target->m_iBindingChoicesCount++];
	strncpy( choice, keyName, MAX_INSTRUCTOR_LOCATOR_BINDING - 1 );
	choice[MAX_INSTRUCTOR_LOCATOR_BINDING - 1] = '\0';
}

static void HUD_UpdateInstructorBindingChoices( CInstructorLocatorTarget *target, float flTime )
{
	if( !target ) return;
	char oldChoices[MAX_INSTRUCTOR_LOCATOR_BINDINGS][MAX_INSTRUCTOR_LOCATOR_BINDING];
	memset( oldChoices, 0, sizeof( oldChoices ) );
	const int oldCount = target->m_iBindingChoicesCount;
	const int oldTick = target->m_iBindingTick;
	for( int i = 0; i < oldCount && i < MAX_INSTRUCTOR_LOCATOR_BINDINGS; ++i )
		strncpy( oldChoices[i], target->m_szBindingChoices[i], MAX_INSTRUCTOR_LOCATOR_BINDING - 1 );

	const bool bindingChanged = stricmp( target->m_szBindingCache, target->m_szBinding ) != 0;
	strncpy( target->m_szBindingCache, target->m_szBinding, sizeof( target->m_szBindingCache ) - 1 );
	target->m_szBindingCache[sizeof( target->m_szBindingCache ) - 1] = '\0';
	target->m_iBindingChoicesCount = 0;
	memset( target->m_szBindingChoices, 0, sizeof( target->m_szBindingChoices ) );

	const char *cursor = target->m_szBinding;
	while( cursor && *cursor && target->m_iBindingChoicesCount < MAX_INSTRUCTOR_LOCATOR_BINDINGS )
	{
		while( *cursor == ';' || *cursor == ' ' || *cursor == '\t' ) ++cursor;
		if( !*cursor ) break;
		const char *tokenStart = cursor;
		while( *cursor && *cursor != ';' ) ++cursor;
		size_t tokenLength = (size_t)( cursor - tokenStart );
		while( tokenLength > 0 && ( tokenStart[tokenLength - 1] == ' ' || tokenStart[tokenLength - 1] == '\t' ) ) --tokenLength;
		if( tokenLength == 0 )
		{
			if( *cursor == ';' ) ++cursor;
			continue;
		}
		char token[64];
		if( tokenLength >= sizeof( token ) ) tokenLength = sizeof( token ) - 1;
		memcpy( token, tokenStart, tokenLength );
		token[tokenLength] = '\0';

		const char *command = NULL;
		if( HUD_IsInstructorBindingCommand( token, &command ) )
		{
			char commandName[64];
			size_t commandLength = 0;
			while( command[commandLength] &&
				( isalnum( (unsigned char)command[commandLength] ) || command[commandLength] == '_' ) &&
				commandLength < sizeof( commandName ) - 1 )
			{
				commandName[commandLength] = command[commandLength];
				++commandLength;
			}
			commandName[commandLength] = '\0';
			if( commandName[0] && gEngfuncs.Key_LookupBindingEx )
			{
				for( int ordinal = 0; ordinal < MAX_INSTRUCTOR_LOCATOR_BINDINGS &&
					target->m_iBindingChoicesCount < MAX_INSTRUCTOR_LOCATOR_BINDINGS; ++ordinal )
				{
					const char *keyName = gEngfuncs.Key_LookupBindingEx( commandName, ordinal );
					if( !keyName || !keyName[0] ) break;
					HUD_AddInstructorBindingChoice( target, keyName );
				}
			}
			else if( commandName[0] && gEngfuncs.Key_LookupBinding )
				HUD_AddInstructorBindingChoice( target, gEngfuncs.Key_LookupBinding( commandName ) );
		}
		else
			HUD_AddInstructorBindingChoice( target, token );

		if( *cursor == ';' ) ++cursor;
	}

	bool choicesChanged = oldCount != target->m_iBindingChoicesCount;
	if( !choicesChanged )
		for( int i = 0; i < oldCount; ++i )
			if( stricmp( oldChoices[i], target->m_szBindingChoices[i] ) != 0 )
			{
				choicesChanged = true;
				break;
			}

	if( bindingChanged || choicesChanged )
		{
			target->m_iBindingTick = 0;
			target->m_flNextBindingTick = flTime + 0.75f;
		}
	else if( target->m_iBindingChoicesCount > 0 )
		target->m_iBindingTick = oldTick % target->m_iBindingChoicesCount;
	else
		target->m_iBindingTick = 0;

	if( flTime >= target->m_flNextBindingTick )
	{
		if( target->m_iBindingChoicesCount > 1 )
			target->m_iBindingTick = ( target->m_iBindingTick + 1 ) % target->m_iBindingChoicesCount;
		target->m_flNextBindingTick = flTime + 0.75f;
	}
}

static bool HUD_IsInstructorControllerBinding( const char *keyName )
{
	if( !keyName ) return false;
	static const char *keys[] = {
		"A_BUTTON", "B_BUTTON", "X_BUTTON", "Y_BUTTON", "BACK", "MODE", "START",
		"STICK1", "STICK2", "L1_BUTTON", "R1_BUTTON", "L2_BUTTON", "R2_BUTTON",
		"LTRIGGER", "RTRIGGER", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"
	};
	for( size_t i = 0; i < sizeof( keys ) / sizeof( keys[0] ); ++i )
		if( stricmp( keyName, keys[i] ) == 0 ) return true;
	return false;
}

static HSPRITE HUD_GetInstructorBindingImage( CHud *hud, const char *keyName,
	char *displayName, size_t displayNameSize )
{
	if( !displayName || displayNameSize == 0 ) return 0;
	displayName[0] = '\0';
	if( !hud || !keyName || !keyName[0] )
	{
		snprintf( displayName, displayNameSize, "NONE" );
		return hud ? hud->m_hInstructorHintKeyWideSprite : 0;
	}
	if( stricmp( keyName, "MOUSE1" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyMouseLeftSprite ) return hud->m_hInstructorHintKeyMouseLeftSprite;
		snprintf( displayName, displayNameSize, "M1" ); return 0;
	}
	if( stricmp( keyName, "MOUSE2" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyMouseRightSprite ) return hud->m_hInstructorHintKeyMouseRightSprite;
		snprintf( displayName, displayNameSize, "M2" ); return 0;
	}
	if( stricmp( keyName, "MOUSE3" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyMouseThreeSprite ) return hud->m_hInstructorHintKeyMouseThreeSprite;
		snprintf( displayName, displayNameSize, "M3" ); return 0;
	}
	if( stricmp( keyName, "MWHEELUP" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyMouseWheelUpSprite ) return hud->m_hInstructorHintKeyMouseWheelUpSprite;
		snprintf( displayName, displayNameSize, "MW+" ); return 0;
	}
	if( stricmp( keyName, "MWHEELDOWN" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyMouseWheelDownSprite ) return hud->m_hInstructorHintKeyMouseWheelDownSprite;
		snprintf( displayName, displayNameSize, "MW-" ); return 0;
	}
	if( stricmp( keyName, "UPARROW" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyUpSprite ) return hud->m_hInstructorHintKeyUpSprite;
		snprintf( displayName, displayNameSize, "UP" ); return 0;
	}
	if( stricmp( keyName, "DOWNARROW" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyDownSprite ) return hud->m_hInstructorHintKeyDownSprite;
		snprintf( displayName, displayNameSize, "DN" ); return 0;
	}
	if( stricmp( keyName, "LEFTARROW" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyLeftSprite ) return hud->m_hInstructorHintKeyLeftSprite;
		snprintf( displayName, displayNameSize, "LT" ); return 0;
	}
	if( stricmp( keyName, "RIGHTARROW" ) == 0 )
	{
		if( hud->m_hInstructorHintKeyRightSprite ) return hud->m_hInstructorHintKeyRightSprite;
		snprintf( displayName, displayNameSize, "RT" ); return 0;
	}
	// Source uses a dedicated controller-glyph font that GoldSrc lacks.
	if( HUD_IsInstructorControllerBinding( keyName ) )
	{
		HUD_FormatInstructorKeyName( keyName, displayName, displayNameSize );
		return 0;
	}
	char upper[32];
	size_t length = 0;
	while( keyName[length] && length < sizeof( upper ) - 1 )
	{
		upper[length] = (char)toupper( (unsigned char)keyName[length] );
		++length;
	}
	upper[length] = '\0';
	const bool useGeneric = length <= 2 ||
		stricmp( upper, "SEMICOLON" ) == 0 || stricmp( upper, "INS" ) == 0 ||
		stricmp( upper, "DEL" ) == 0 || stricmp( upper, "HOME" ) == 0 ||
		stricmp( upper, "END" ) == 0 || stricmp( upper, "PGUP" ) == 0 ||
		stricmp( upper, "PGDN" ) == 0 || stricmp( upper, "PAUSE" ) == 0 ||
		stricmp( upper, "F10" ) == 0 || stricmp( upper, "F11" ) == 0 ||
		stricmp( upper, "F12" ) == 0;
	HUD_FormatInstructorKeyName( keyName, displayName, displayNameSize );
	return useGeneric ? hud->m_hInstructorHintKeyGenericSprite : hud->m_hInstructorHintKeyWideSprite;
}

// Compact 5x7 glyphs for black keycap labels. These are blended over the
// existing .spr artwork, because the GoldSrc HUD font is additive and cannot
// render true black. FillRGBABlend preserves the locator's shared fade.
static unsigned char HUD_InstructorKeyGlyphRow( char ch, int row )
{
	static const unsigned char glyphs[36][7] =
	{
		{ 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 }, // A
		{ 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E }, // B
		{ 0x0F,0x10,0x10,0x10,0x10,0x10,0x0F }, // C
		{ 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E }, // D
		{ 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F }, // E
		{ 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 }, // F
		{ 0x0F,0x10,0x10,0x17,0x11,0x11,0x0F }, // G
		{ 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 }, // H
		{ 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E }, // I
		{ 0x07,0x02,0x02,0x02,0x12,0x12,0x0C }, // J
		{ 0x11,0x12,0x14,0x18,0x14,0x12,0x11 }, // K
		{ 0x10,0x10,0x10,0x10,0x10,0x10,0x1F }, // L
		{ 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 }, // M
		{ 0x11,0x19,0x15,0x13,0x11,0x11,0x11 }, // N
		{ 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E }, // O
		{ 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 }, // P
		{ 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D }, // Q
		{ 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 }, // R
		{ 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E }, // S
		{ 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 }, // T
		{ 0x11,0x11,0x11,0x11,0x11,0x11,0x0E }, // U
		{ 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 }, // V
		{ 0x11,0x11,0x11,0x15,0x15,0x15,0x0A }, // W
		{ 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 }, // X
		{ 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 }, // Y
		{ 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F }, // Z
		{ 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E }, // 0
		{ 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E }, // 1
		{ 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F }, // 2
		{ 0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E }, // 3
		{ 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 }, // 4
		{ 0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E }, // 5
		{ 0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E }, // 6
		{ 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 }, // 7
		{ 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E }, // 8
		{ 0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E }  // 9
	};

	if( ch >= 'a' && ch <= 'z' )
		ch = (char)( ch - 'a' + 'A' );
	int index = -1;
	if( ch >= 'A' && ch <= 'Z' )
		index = ch - 'A';
	else if( ch >= '0' && ch <= '9' )
		index = 26 + ch - '0';
	if( index >= 0 && index < 36 && row >= 0 && row < 7 )
		return glyphs[index][row];
	switch( ch )
	{
	case '-': return row == 3 ? 0x1F : 0x00;
	case '+': return row == 3 ? 0x1F : ( row == 1 || row == 2 || row == 4 || row == 5 ? 0x04 : 0x00 );
	case '/': return row == 0 ? 0x01 : ( row == 1 || row == 2 ? 0x02 :
		( row == 3 ? 0x04 : ( row == 4 || row == 5 ? 0x08 : 0x10 ) ) );
	case '.': return row >= 5 ? 0x0C : 0x00;
	default: return 0x00;
	}
}

// Alpha-blended 5x7 text for the small instructor labels. GoldSrc's
// VGUI2DrawCharacterAdditive API accepts RGB but no alpha, so DrawHudString
// cannot fade in lockstep with an RGBA locator sprite. FillRGBA can.
static void HUD_DrawInstructorAlphaText( int x, int y, int maxX,
	const char *text, int r, int g, int b, int alpha, int scale )
{
	if( !text || !text[0] || alpha <= 0 || scale <= 0 || maxX <= x )
		return;

	const int advance = 6 * scale;
	for( int i = 0; text[i] && text[i] != '\n'; ++i )
	{
		const int glyphX = x + i * advance;
		if( glyphX >= maxX )
			break;

		for( int row = 0; row < 7; ++row )
		{
			const unsigned char bits = HUD_InstructorKeyGlyphRow( text[i], row );
			int col = 0;
			while( col < 5 )
			{
				if( !( bits & ( 1 << ( 4 - col ) ) ) )
				{
					++col;
					continue;
				}

				const int first = col;
				while( col < 5 && ( bits & ( 1 << ( 4 - col ) ) ) )
					++col;

				const int drawX = glyphX + first * scale;
				int drawWidth = ( col - first ) * scale;
				if( drawX + drawWidth > maxX )
					drawWidth = maxX - drawX;
				if( drawWidth > 0 )
					FillRGBABlend( drawX, y + row * scale, drawWidth, scale,
						r, g, b, alpha );
			}
		}
	}
}

static void HUD_DrawInstructorBindingName( int centerX, int centerY,
	const char *bindingName, int alpha )
{
	if( !bindingName || !bindingName[0] || alpha <= 0 )
		return;

	int length = (int)strlen( bindingName );
	if( length > 12 )
		length = 12;

	// Generic key labels are drawn from a compact 5x7 bitmap. The scale is
	// adjustable so the letter can be tuned independently from the key art.
	const int scale = Q_max( 1, (int)( HUD_GetInstructorKeyTextScale() *
		HUD_GetInstructorScale() + 0.5f ) );
	const int totalWidth = length * 6 * scale - scale;
	const int glyphHeight = 7 * scale;
	const int startX = centerX - totalWidth / 2;
	const int startY = centerY - glyphHeight / 2;
	HUD_DrawInstructorAlphaText( startX, startY, startX + totalWidth,
		bindingName, 0, 0, 0, alpha, scale );
}

// Think
void CHud::Think( void )
{
	m_scrinfo.iSize = sizeof(m_scrinfo);
	GetScreenInfo(&m_scrinfo);

	int newfov;
	HUDLIST *pList = m_pHudList;

	while( pList )
	{
		if( pList->p->m_iFlags & HUD_ACTIVE )
			pList->p->Think();
		pList = pList->pNext;
	}

	newfov = HUD_GetFOV();
	if( newfov == 0 )
	{
		m_iFOV = default_fov->value;
	}
	else
	{
		m_iFOV = newfov;
	}

	// the clients fov is actually set in the client data update section of the hud
	// Set a new sensitivity
	if( m_iFOV == default_fov->value )
	{
		// reset to saved sensitivity
		m_flMouseSensitivity = 0;
	}
	else
	{
		// set a new sensitivity that is proportional to the change from the FOV default
		m_flMouseSensitivity = sensitivity->value * ((float)newfov / Q_max( default_fov->value, 90 )) * CVAR_GET_FLOAT("zoom_sensitivity_ratio");
	}

	// think about default fov
	if( m_iFOV == 0 )
	{
		// only let players adjust up in fov,  and only if they are not overriden by something else
		m_iFOV = Q_max( default_fov->value, 90 );  
	}

	if( gEngfuncs.IsSpectateOnly() )
	{
		m_iFOV = gHUD.m_Spectator.GetFOV(); // default_fov->value;
	}
}

// Redraw
// step through the local data,  placing the appropriate graphics & text as appropriate
// returns 1 if they've changed, 0 otherwise
int CHud::Redraw( float flTime, int intermission )
{
	m_fOldTime = m_flTime;	// save time of previous redraw
	m_flTime = flTime;
	m_InstructorLocatorPanel.BeginFrame( m_flTime );
	GameInstructor_Update( flTime );

	// Refresh the managed lesson adapter; map-authored hints already own
	// independent target entries in the locator panel.
	HUD_SyncInstructorLocatorTarget( this );

	for( int locatorIndex = 0; locatorIndex < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++locatorIndex )
	{
		CInstructorLocatorTarget *target = m_InstructorLocatorPanel.GetTargetAt( locatorIndex );
		if( !target || !target->m_isActive )
			continue;

		if( target->m_bManaged && !target->m_bOriginInScreenspace )
		{
			bool targetAllowed = true;
			cl_entity_t *managedTarget = gEngfuncs.GetEntityByIndex ?
				gEngfuncs.GetEntityByIndex( target->m_iEntityIndex ) : NULL;
			if( !managedTarget )
				targetAllowed = false;
			else
			{
				target->m_vecOrigin[0] = managedTarget->origin[0];
				target->m_vecOrigin[1] = managedTarget->origin[1];
				target->m_vecOrigin[2] = managedTarget->origin[2];
				if( managedTarget->curstate.solid == SOLID_BSP )
				{
					target->m_vecOrigin[0] +=
						( managedTarget->curstate.mins[0] + managedTarget->curstate.maxs[0] ) * 0.5f;
					target->m_vecOrigin[1] +=
						( managedTarget->curstate.mins[1] + managedTarget->curstate.maxs[1] ) * 0.5f;
					target->m_vecOrigin[2] +=
						( managedTarget->curstate.mins[2] + managedTarget->curstate.maxs[2] ) * 0.5f;
				}

				// Source's icon_relative_offset follows the main view-up axis;
				// keep icon_offset as the separate world-Z adjustment.
				if( target->m_flRelativeIconOffset != 0.0f &&
					gEngfuncs.GetViewAngles && gEngfuncs.pfnAngleVectors )
				{
					float viewAngles[3], forward[3], right[3], up[3];
					gEngfuncs.GetViewAngles( viewAngles );
					gEngfuncs.pfnAngleVectors( viewAngles, forward, right, up );
					target->m_vecOrigin[0] += up[0] * target->m_flRelativeIconOffset;
					target->m_vecOrigin[1] += up[1] * target->m_flRelativeIconOffset;
					target->m_vecOrigin[2] += up[2] * target->m_flRelativeIconOffset;
				}
				if( ( managedTarget->curstate.effects & EF_NODRAW ) &&
					!( target->m_iEffectsFlags & 0x00010000 ) )
					targetAllowed = false;
				if( targetAllowed && target->m_flRange > 0.0f && gEngfuncs.GetLocalPlayer )
				{
					cl_entity_t *localPlayer = gEngfuncs.GetLocalPlayer();
					if( localPlayer )
					{
						const float dx = target->m_vecOrigin[0] - localPlayer->origin[0];
						const float dy = target->m_vecOrigin[1] - localPlayer->origin[1];
						const float dz = target->m_vecOrigin[2] - localPlayer->origin[2];
						if( sqrtf( dx * dx + dy * dy + dz * dz ) > target->m_flRange )
							targetAllowed = false;
					}
				}
			}
			if( targetAllowed && flTime >= target->m_flNextOcclusionTest )
			{
				target->m_bOccluded = HUD_IsInstructorHintOccluded(
					target->m_vecOrigin, target->m_iEntityIndex );
				target->m_flNextOcclusionTest = flTime + 0.25f;
			}
			const bool forceCaption = ( target->m_iEffectsFlags & LOCATOR_ICON_FX_FORCE_CAPTION ) != 0;
			const bool showWhenOccluded =
				( target->m_iEffectsFlags & LOCATOR_ICON_FX_SHOW_WHEN_OCCLUDED ) != 0;
			target->SetVisible( targetAllowed &&
				( !target->m_bOccluded || forceCaption || showWhenOccluded ), flTime, 0.3f );
		}

		// Source CLocatorPanel::ComputeTargetIconPosition measures from
		// MainViewOrigin(). GoldSrc's v_origin is the finalized refdef camera
		// position, including view height, chase-camera offsets, and view effects.
		if( target->m_bOriginInScreenspace )
		{
			target->m_flDistFromPlayer = 0.0f;
		}
		else
		{
			const float dx = target->m_vecOrigin[0] - v_origin[0];
			const float dy = target->m_vecOrigin[1] - v_origin[1];
			const float dz = target->m_vecOrigin[2] - v_origin[2];
			target->m_flDistFromPlayer = sqrtf( dx * dx + dy * dy + dz * dz );
		}

		if( target->m_flEndTime > 0.0f && flTime >= target->m_flEndTime )
		{
			m_InstructorLocatorPanel.RemoveTarget( target->m_serialNumber, flTime, 0.3f );
			target->m_flEndTime = 0.0f;
		}
	}

	// Advance animations once, size every visible target, then resolve
	// collisions across world markers and screen-space lesson cards together.
	m_InstructorLocatorPanel.UpdatePresentation( m_flTime );
	for( int locatorIndex = 0; locatorIndex < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++locatorIndex )
	{
		CInstructorLocatorTarget *target = m_InstructorLocatorPanel.GetTargetAt( locatorIndex );
		if( !target || !target->m_isActive || !target->ShouldDraw() )
			continue;

		if( target->m_bOriginInScreenspace )
		{
			char resolvedBinding[64];
			HUD_ResolveInstructorBinding( target->m_szBinding,
				resolvedBinding, sizeof( resolvedBinding ) );
			const int captionWidth = HUD_GetInstructorCaptionWidth( target->m_szCaption );
			const int bindingWidth = DrawHudStringLen( resolvedBinding );
			const bool managedCard = target->m_bManaged;
			const bool genericKey = m_hInstructorHintKeyGenericSprite &&
				resolvedBinding[0] && resolvedBinding[1] && !resolvedBinding[2] ? true :
				( m_hInstructorHintKeyGenericSprite && resolvedBinding[0] && !resolvedBinding[1] );
			int keyChipWidth = 0;
			int keyChipHeight = 0;
			if( managedCard && resolvedBinding[0] )
			{
				if( genericKey )
				{
					HUD_GetInstructorSpriteSize( m_hInstructorHintKeyGenericSprite, 1.0f,
						&keyChipWidth, &keyChipHeight );
				}
				else
				{
					HUD_GetInstructorSpriteSize( 0, 1.0f, NULL, &keyChipHeight );
					const int fallbackPadding = HUD_ScaleInstructorPixel( 8 );
					keyChipWidth = bindingWidth + fallbackPadding * 2;
				}
				if( !genericKey && keyChipWidth > 64 ) keyChipWidth = 64;
			}

			HSPRITE icon = (HSPRITE)target->m_hOnscreenSprite;
			if( target->m_iHintId == -1 )
				icon = target->m_iIconMode == 2 ? m_hInstructorHintHoldSprite :
					( target->m_iIconMode == 1 ? m_hInstructorHintMoveSprite : 0 );
			int iconWidth = 0;
			int iconHeight = 0;
			if( icon )
				HUD_GetInstructorSpriteSize( icon, INSTRUCTOR_LOCATOR_ICON_BASE_SCALE,
					&iconWidth, &iconHeight );

			const int textWidth = managedCard ?
				captionWidth + ( keyChipWidth ? keyChipWidth + HUD_ScaleInstructorPixel( 12 ) : 0 ) :
				Q_max( captionWidth, bindingWidth );
			int panelWidth = textWidth +
				( iconWidth ? iconWidth + HUD_ScaleInstructorPixel( 12 ) : 0 ) +
				HUD_ScaleInstructorPixel( managedCard ? 44 : 36 );
			const int maxPanelWidth = Q_max( 1, ScreenWidth - HUD_ScaleInstructorPixel( 24 ) );
			if( panelWidth > maxPanelWidth ) panelWidth = maxPanelWidth;
			const int minPanelWidth = HUD_ScaleInstructorPixel( managedCard ? 224 : 180 );
			if( panelWidth < minPanelWidth && maxPanelWidth >= minPanelWidth )
				panelWidth = minPanelWidth;

			const int basePanelHeight = HUD_ScaleInstructorPixel(
				managedCard ? 62 : ( resolvedBinding[0] ? 58 : 42 ) );
			const int panelHeight = Q_max( basePanelHeight,
				Q_max( iconHeight + HUD_ScaleInstructorPixel( managedCard ? 24 : 16 ),
					keyChipHeight > 0 ? keyChipHeight + HUD_ScaleInstructorPixel( 24 ) : 0 ) );
			int panelX = ( ScreenWidth - panelWidth ) / 2;
			int panelY = ScreenHeight - panelHeight - HUD_ScaleInstructorPixel( 30 );
			if( target->m_bFixedPosition )
			{
				panelX = (int)( target->m_vecOrigin[0] * ScreenWidth - panelWidth * 0.5f );
				panelY = (int)( target->m_vecOrigin[1] * ScreenHeight - panelHeight * 0.5f );
			}

			target->m_wide = panelWidth;
			target->m_tall = panelHeight;
			target->m_iconX = panelX + panelWidth / 2;
			target->m_iconY = panelY + panelHeight / 2;
			target->m_bOnscreen = true;
			target->m_bIsDrawing = true;
			continue;
		}

		float screenX = 0.0f, screenY = 0.0f;
		bool behind = false;
		float worldOrigin[3] =
		{
			target->m_vecOrigin[0],
			target->m_vecOrigin[1],
			target->m_vecOrigin[2] + target->m_flIconOffset
		};
		target->m_wide = HUD_ScaleInstructorPixel( 40 );
		target->m_tall = HUD_ScaleInstructorPixel( 40 );
		if( HUD_GetInstructorHintScreenPosition( worldOrigin, &screenX, &screenY, &behind ) )
		{
			target->UpdateProjection( screenX, screenY, behind,
				ScreenWidth, ScreenHeight, ScreenWidth * 0.075f, m_flTime );
			float compassSin = 0.0f, compassCos = 1.0f;
			const bool hasCompassDirection =
				HUD_GetInstructorCompassDirection( worldOrigin, &compassSin, &compassCos );
			target->SetCompassDirection( compassSin, compassCos, hasCompassDirection );
			if( !target->m_bOnscreen && ( target->m_iEffectsFlags & LOCATOR_ICON_FX_NO_OFFSCREEN ) )
				continue;
			// Use the same target-specific safe edge margin during layout and
			// final drawing. ResolveDecluttering then offsets from this exact
			// base position instead of a subtly different second projection.
			const bool targetOffscreen = !target->m_bOnscreen;
			const bool targetUsesOffscreenIcon = targetOffscreen;
			const char *requestedIcon = targetUsesOffscreenIcon ?
				target->m_szOffscreenIcon : target->m_szOnscreenIcon;
			const bool sourceBindingIcon = requestedIcon &&
				stricmp( requestedIcon, "use_binding" ) == 0;
			const bool hasDirectionalLocatorSprites =
				m_hInstructorHintArrowRightSprite && m_hInstructorHintArrowUpSprite;
			int sourceBindingIconWidth = 0;
			int sourceBindingIconHeight = 0;
			if( sourceBindingIcon )
			{
				HUD_UpdateInstructorBindingChoices( target, m_flTime );
				const char *keyName = target->m_iBindingChoicesCount > 0 ?
					target->m_szBindingChoices[target->m_iBindingTick % target->m_iBindingChoicesCount] : NULL;
				char ignoredGlyph[32];
				HSPRITE keySprite = HUD_GetInstructorBindingImage( this, keyName,
					ignoredGlyph, sizeof( ignoredGlyph ) );
				if( keySprite )
				{
					HUD_GetInstructorSpriteSize( keySprite, 1.0f,
						&sourceBindingIconWidth, &sourceBindingIconHeight );
				}
			}
			target->m_iEdgeMargin = hasDirectionalLocatorSprites ?
				( sourceBindingIcon ?
					sourceBindingIconHeight / 2 + HUD_ScaleInstructorPixel( 3 + 24 + 4 ) :
					HUD_ScaleInstructorPixel( 22 + 24 + 4 ) ) :
				HUD_ScaleInstructorPixel( 22 );
			target->CalculateEdgePosition( ScreenWidth, ScreenHeight, target->m_iEdgeMargin );

			HSPRITE icon = targetUsesOffscreenIcon ?
				(HSPRITE)target->m_hOffscreenSprite : (HSPRITE)target->m_hOnscreenSprite;
			if( target->m_iHintId == -1 )
				icon = target->m_iIconMode == 2 ? m_hInstructorHintHoldSprite :
					( target->m_iIconMode == 1 ? m_hInstructorHintMoveSprite : 0 );
			if( icon )
			{
				const int iconWidth = SPR_Width( icon, 0 );
				const int iconHeight = SPR_Height( icon, 0 );
				if( iconWidth > 0 && iconHeight > 0 && iconWidth <= 64 && iconHeight <= 64 )
				{
					const float iconScale = INSTRUCTOR_LOCATOR_ICON_BASE_SCALE * HUD_GetInstructorScale();
					target->m_wide = (int)( iconWidth * iconScale + 0.5f );
					target->m_tall = (int)( iconHeight * iconScale + 0.5f );
				}
			}
			if( sourceBindingIcon && sourceBindingIconWidth > 0 && sourceBindingIconHeight > 0 )
			{
				target->m_wide = sourceBindingIconWidth;
				target->m_tall = sourceBindingIconHeight;
			}
			// Source's dynamic locator only reserves caption width while the
			// target is on-screen and within ICON_DIST_TOO_FAR (720 units),
			// unless FORCE_CAPTION is set. Off-screen and distant markers use
			// the icon footprint only.
			const bool forceCaption = ( target->m_iEffectsFlags & LOCATOR_ICON_FX_FORCE_CAPTION ) != 0;
			if( target->m_bOnscreen &&
				( forceCaption || target->m_flDistFromPlayer <= INSTRUCTOR_LOCATOR_CAPTION_MAX_DISTANCE ) )
			{
				const int captionFootprint = HUD_GetInstructorCaptionWidth( target->m_szCaption ) +
					HUD_ScaleInstructorPixel( 24 );
				if( target->m_wide < captionFootprint )
					target->m_wide = Q_min( Q_max( HUD_ScaleInstructorPixel( 40 ), captionFootprint ),
					Q_max( HUD_ScaleInstructorPixel( 40 ), ScreenWidth / 2 ) );
			}
			if( target->m_tall < HUD_ScaleInstructorPixel( 48 ) )
				target->m_tall = HUD_ScaleInstructorPixel( 48 );
		}
	}
	m_InstructorLocatorPanel.ResolveDecluttering( ScreenWidth, ScreenHeight,
		HUD_ScaleInstructorPixel( 56 ), HUD_ScaleInstructorPixel( 8 ) );

	m_flTimeDelta = (double)( m_flTime - m_fOldTime );
	static float m_flShotTime = 0;

	// Clock was reset, reset delta
	if( m_flTimeDelta < 0 )
		m_flTimeDelta = 0;

	// Bring up the scoreboard during intermission
	if (gViewPort)
	{
		if( m_iIntermission && !intermission )
		{
			// Have to do this here so the scoreboard goes away
			m_iIntermission = intermission;
			gViewPort->HideCommandMenu();
			gViewPort->HideScoreBoard();
			gViewPort->UpdateSpectatorPanel();
		}
		else if( !m_iIntermission && intermission )
		{
			m_iIntermission = intermission;
			gViewPort->HideCommandMenu();
			gViewPort->HideVGUIMenu();
			if (UseVguiScoreBoard())
				gViewPort->ShowScoreBoard();
			gViewPort->UpdateSpectatorPanel();
			// Take a screenshot if the client's got the cvar set
			if( CVAR_GET_FLOAT( "hud_takesshots" ) != 0 )
				m_flShotTime = flTime + 1.0;	// Take a screenshot in a second
		}
	}
	if( m_flShotTime && m_flShotTime < flTime )
	{
		gEngfuncs.pfnClientCmd( "snapshot\n" );
		m_flShotTime = 0;
	}

	m_iIntermission = intermission;

	// if no redrawing is necessary
	// return 0;

	m_iHudNumbersYOffset = IsHL25() ? m_iFontHeight * 0.2 : 0;

	if( m_pCvarDraw->value )
	{
		HUDLIST *pList = m_pHudList;

		while( pList )
		{
			if( !intermission )
			{
				if ( ( pList->p->m_iFlags & HUD_ACTIVE ) && !( m_iHideHUDDisplay & HIDEHUD_ALL ) )
					pList->p->Draw( flTime );
			}
			else
			{
				// it's an intermission,  so only draw hud elements that are set to draw during intermissions
				if( pList->p->m_iFlags & HUD_INTERMISSION )
					pList->p->Draw( flTime );
			}

			pList = pList->pNext;
		}
	}

	// Source-style instructor hint presentation now consumes the locator model.
	// The PS2 world-space bracket remains a separate, unchanged HUD element.
	for( int locatorIndex = 0; locatorIndex < MAX_INSTRUCTOR_LOCATOR_TARGETS; ++locatorIndex )
	{
	CInstructorLocatorTarget *locatorTarget = m_InstructorLocatorPanel.GetTargetAt( locatorIndex );
	if( locatorTarget && locatorTarget->ShouldDraw() &&
		( !locatorTarget->m_bOccluded ||
			( locatorTarget->m_iEffectsFlags & LOCATOR_ICON_FX_FORCE_CAPTION ) ||
			locatorTarget->IsFadingOut() ) &&
		!intermission && m_pCvarDraw && m_pCvarDraw->value &&
		!( m_iHideHUDDisplay & HIDEHUD_ALL ) )
	{
		if( locatorTarget->m_flEndTime > 0.0f && flTime >= locatorTarget->m_flEndTime )
		{
			m_InstructorLocatorPanel.RemoveTarget( locatorTarget->m_serialNumber, flTime, 0.3f );
			locatorTarget->m_flEndTime = 0.0f;
		}
		else
		{
			char resolvedBinding[64];
			HUD_ResolveInstructorBinding( locatorTarget->m_szBinding,
				resolvedBinding, sizeof( resolvedBinding ) );

			char bindingText[80];
			bindingText[0] = '\0';
			if( resolvedBinding[0] )
				snprintf( bindingText, sizeof( bindingText ),
					locatorTarget->m_bManaged ? "%s" : "[ %s ]", resolvedBinding );

			int captionWidth = HUD_GetInstructorCaptionWidth( locatorTarget->m_szCaption );
			int bindingWidth = DrawHudStringLen( bindingText );

			// Project the current world target before selecting its on/off-screen icon.
			float targetX = 0.0f, targetY = 0.0f;
			float entityAnchorX = 0.0f, entityAnchorY = 0.0f;
			bool targetBehind = false;
			bool entityAnchorBehind = false;
			float locatorWorldOrigin[3] =
			{
				locatorTarget->m_vecOrigin[0],
				locatorTarget->m_vecOrigin[1],
				locatorTarget->m_vecOrigin[2] + locatorTarget->m_flIconOffset
			};
			const bool hasTarget = (!locatorTarget->m_bOriginInScreenspace) &&
				HUD_GetInstructorHintScreenPosition( locatorWorldOrigin,
					&targetX, &targetY, &targetBehind );
			const bool hasEntityAnchor = (!locatorTarget->m_bOriginInScreenspace) &&
				HUD_GetInstructorHintScreenPosition( locatorTarget->m_vecOrigin,
					&entityAnchorX, &entityAnchorY, &entityAnchorBehind );
			// Source ComputeTargetIconPosition uses a 7.5%-of-screen-width
			// title-safe inset. Crossing that inset selects the offscreen icon
			// and direction arrow, including targets near the screen corners.
			const float edgeMargin = ScreenWidth * 0.075f;
			if( hasTarget )
			{
				locatorTarget->UpdateProjection( targetX, targetY, targetBehind,
					ScreenWidth, ScreenHeight, edgeMargin, flTime );
				float compassSin = 0.0f, compassCos = 1.0f;
				const bool hasCompassDirection =
					HUD_GetInstructorCompassDirection( locatorWorldOrigin, &compassSin, &compassCos );
				locatorTarget->SetCompassDirection( compassSin, compassCos, hasCompassDirection );
			}
			// Source treats an actively presenting locator as a static icon:
			// it stays centered until locator_lerp_rest has elapsed.
			const float presentationRestTime = HUD_GetInstructorPresentationRestTime();
			const float presentationElapsed = flTime - locatorTarget->m_lerpStart;
			const float presentationLerpTime = HUD_GetInstructorPresentationLerpTime();
			const bool locatorPresenting = hasTarget &&
				locatorTarget->IsPresenting( flTime, presentationRestTime );
			const bool locatorStatic = hasTarget &&
				( locatorPresenting ||
					( locatorTarget->m_iEffectsFlags & LOCATOR_ICON_FX_STATIC ) );
			const bool locatorLerping = hasTarget && !locatorStatic &&
				presentationLerpTime > 0.0f &&
				presentationElapsed < presentationRestTime + presentationLerpTime;
			float locatorLerpFraction = 1.0f;
			if( locatorLerping )
			{
				locatorLerpFraction = ( presentationElapsed - presentationRestTime ) /
					presentationLerpTime;
				if( locatorLerpFraction < 0.0f ) locatorLerpFraction = 0.0f;
				if( locatorLerpFraction > 1.0f ) locatorLerpFraction = 1.0f;
			}
			const bool targetOffscreen = hasTarget && !locatorStatic &&
				!locatorTarget->m_bOnscreen;
			const bool targetUsesOffscreenIcon = targetOffscreen;

			// "use_binding" is a virtual icon: the resolved key art becomes the locator.
			char instructorBindingGlyph[32];
			instructorBindingGlyph[0] = '\0';
			HSPRITE instructorBindingSprite = 0;
			const char *requestedBindingIcon = targetUsesOffscreenIcon ?
				locatorTarget->m_szOffscreenIcon : locatorTarget->m_szOnscreenIcon;
			const bool sourceBindingIcon = requestedBindingIcon &&
				stricmp( requestedBindingIcon, "use_binding" ) == 0;
			int sourceBindingIconHeight = 0;
			int sourceBindingIconWidth = 0;
			if( sourceBindingIcon )
			{
				HUD_UpdateInstructorBindingChoices( locatorTarget, flTime );
				const char *keyName = locatorTarget->m_iBindingChoicesCount > 0 ?
					locatorTarget->m_szBindingChoices[locatorTarget->m_iBindingTick % locatorTarget->m_iBindingChoicesCount] : NULL;
				instructorBindingSprite = HUD_GetInstructorBindingImage( this, keyName,
					instructorBindingGlyph, sizeof( instructorBindingGlyph ) );
				if( instructorBindingSprite )
				{
					HUD_GetInstructorSpriteSize( instructorBindingSprite, 1.0f,
						&sourceBindingIconWidth, &sourceBindingIconHeight );
				}
			}

			// Source counts a lesson only after its icon has remained on-screen
			// for at least 1.5 seconds, rather than when it is merely opened.
			if( locatorTarget->m_bManaged && !locatorTarget->m_bManagedDisplayCounted )
			{
				const bool countsAsVisible = locatorTarget->m_bOriginInScreenspace ||
					( hasTarget && !targetOffscreen &&
						( !locatorPresenting ||
							( locatorTarget->m_iEffectsFlags & LOCATOR_ICON_FX_STATIC ) ) );
				if( countsAsVisible )
				{
					if( locatorTarget->m_flVisibleSince <= 0.0f )
						locatorTarget->m_flVisibleSince = flTime;
					else if( flTime - locatorTarget->m_flVisibleSince >= 1.5f )
					{
						GameInstructor_MarkDisplayed( locatorTarget->m_szName );
						locatorTarget->m_bManagedDisplayCounted = true;
					}
				}
				else
					locatorTarget->m_flVisibleSince = 0.0f;
			}


			// A target-backed hint is a world-space locator, not a tutorial card.
			// Keep its icon attached to the projected target and place only a compact
			// caption tag beside it. Fixed/no-target lessons retain the HUD card path.
			const bool worldLocator = hasTarget && !locatorTarget->m_bFixedPosition;
			if( worldLocator )
			{
				if( !( targetOffscreen && ( locatorTarget->m_iEffectsFlags & 0x00000200 ) ) )
				{
					const bool hasDirectionalLocatorSprites =
						m_hInstructorHintArrowRightSprite && m_hInstructorHintArrowUpSprite;
					const int locatorArrowDrawSize = HUD_ScaleInstructorPixel( 24 );
					const int locatorArrowGap = HUD_ScaleInstructorPixel( 4 );
					const int locatorEdgeMargin = locatorTarget->m_iEdgeMargin > 0 ?
						locatorTarget->m_iEdgeMargin : HUD_ScaleInstructorPixel( 22 );

					// Projection and edge placement belong to the target model.
					// The renderer adds only transient pulse/shake offsets.
					if( !locatorStatic )
						locatorTarget->CalculateEdgePosition( ScreenWidth, ScreenHeight, locatorEdgeMargin );
					const float screenCenterX = ScreenWidth * 0.5f;
					const float screenCenterY = ScreenHeight * 0.5f;
					const float desiredLocatorX =
						(float)( locatorTarget->m_iconX + locatorTarget->m_offsetX ) +
							locatorTarget->m_flShakeX;
					const float desiredLocatorY =
						(float)( locatorTarget->m_iconY + locatorTarget->m_offsetY ) +
							locatorTarget->m_flShakeY;
					float locatorX = locatorStatic ? screenCenterX :
						locatorLerping ? screenCenterX +
							( desiredLocatorX - screenCenterX ) * locatorLerpFraction :
							desiredLocatorX;
					float locatorY = locatorStatic ? screenCenterY :
						locatorLerping ? screenCenterY +
							( desiredLocatorY - screenCenterY ) * locatorLerpFraction :
							desiredLocatorY;

					if( locatorX < locatorEdgeMargin ) locatorX = (float)locatorEdgeMargin;
					if( locatorX > ScreenWidth - locatorEdgeMargin ) locatorX = (float)( ScreenWidth - locatorEdgeMargin );
					if( locatorY < locatorEdgeMargin ) locatorY = (float)locatorEdgeMargin;
					if( locatorY > ScreenHeight - locatorEdgeMargin ) locatorY = (float)( ScreenHeight - locatorEdgeMargin );

					const int markerX = (int)locatorX;
					const int markerY = (int)locatorY;

					// The icon_offset raises the marker for readability, but its
					// tether must still originate at the actual entity anchor.
					// This also connects brush-model locators from their computed
					// world-space center instead of the brush's often-zero origin.
					if( !locatorStatic && !targetOffscreen && hasEntityAnchor && !entityAnchorBehind &&
						entityAnchorX >= 0.0f && entityAnchorX < ScreenWidth &&
						entityAnchorY >= 0.0f && entityAnchorY < ScreenHeight )
					{
						const int anchorX = (int)entityAnchorX;
						const int anchorY = (int)entityAnchorY;
						if( markerX - anchorX > 1 || anchorX - markerX > 1 ||
							markerY - anchorY > 1 || anchorY - markerY > 1 )
							HUD_DrawInstructorLocatorTether( anchorX, anchorY,
								markerX, markerY, locatorTarget->m_iCaptionColorR,
								locatorTarget->m_iCaptionColorG, locatorTarget->m_iCaptionColorB,
								locatorTarget->GetRenderAlpha( 150 ) );
					}

					HSPRITE locatorSprite = sourceBindingIcon ? instructorBindingSprite :
						( targetUsesOffscreenIcon ? (HSPRITE)locatorTarget->m_hOffscreenSprite :
							(HSPRITE)locatorTarget->m_hOnscreenSprite );
					if( !sourceBindingIcon && locatorTarget->m_iHintId == -1 )
					{
						locatorSprite = 0;
						if( locatorTarget->m_iIconMode == 2 )
							locatorSprite = m_hInstructorHintHoldSprite;
						else if( locatorTarget->m_iIconMode == 1 )
							locatorSprite = m_hInstructorHintMoveSprite;
					}

					int locatorIconWidth = locatorSprite ? SPR_Width( locatorSprite, 0 ) : 0;
					int locatorIconHeight = locatorSprite ? SPR_Height( locatorSprite, 0 ) : 0;
					if( locatorIconWidth <= 0 || locatorIconHeight <= 0 ||
						locatorIconWidth > 64 || locatorIconHeight > 64 )
					{
						locatorSprite = 0;
						locatorIconWidth = 0;
						locatorIconHeight = 0;
					}

					int locatorR = locatorTarget->m_iCaptionColorR;
					int locatorG = locatorTarget->m_iCaptionColorG;
					int locatorB = locatorTarget->m_iCaptionColorB;

					int locatorAlpha = locatorTarget->GetRenderAlpha( 255 );
					const int locatorTextR = locatorR * locatorAlpha / 255;
					const int locatorTextG = locatorG * locatorAlpha / 255;
					const int locatorTextB = locatorB * locatorAlpha / 255;

					// Source halves locator icons beyond ICON_DIST_TOO_FAR (720 units).
					const float distanceScale =
						locatorTarget->m_flDistFromPlayer > INSTRUCTOR_LOCATOR_CAPTION_MAX_DISTANCE ?
						0.5f : 1.0f;

					// Double the 32x32 locator art to 64x64 at normal gameplay distance.
					if( sourceBindingIcon && locatorIconHeight > 0 && locatorIconWidth > 0 )
						sourceBindingIconWidth = Q_max( 1, (int)( sourceBindingIconHeight *
							( (float)locatorIconWidth / (float)locatorIconHeight ) + 0.5f ) );
					const float locatorIconScale = INSTRUCTOR_LOCATOR_ICON_BASE_SCALE * HUD_GetInstructorScale();
					const int locatorBaseWidth = sourceBindingIcon ? sourceBindingIconWidth :
						( locatorSprite ? (int)( locatorIconWidth * locatorIconScale + 0.5f ) : 0 );
					const int locatorBaseHeight = sourceBindingIcon ? sourceBindingIconHeight :
						( locatorSprite ? (int)( locatorIconHeight * locatorIconScale + 0.5f ) : 0 );
					int animatedIconWidth = locatorBaseWidth;
					int animatedIconHeight = locatorBaseHeight;
					HUD_AnimateInstructorIconSize( locatorTarget, &animatedIconWidth, &animatedIconHeight );
					const int locatorDrawWidth = locatorSprite ?
						Q_max( 1, (int)( animatedIconWidth * distanceScale + 0.5f ) ) : 0;
					const int locatorDrawHeight = locatorSprite ?
						Q_max( 1, (int)( animatedIconHeight * distanceScale + 0.5f ) ) : 0;
					const int markerHalfWidth = sourceBindingIcon ? locatorDrawWidth / 2 :
						( locatorSprite ? locatorDrawWidth / 2 : 10 );
					const int markerHalfHeight = sourceBindingIcon ? locatorDrawHeight / 2 :
						( locatorSprite ? locatorDrawHeight / 2 : 10 );

					if( locatorSprite )
					{
						// Keep the sprite's transparent palette pixels fully clear.
						// Do not draw a rectangular backing/halo behind the locator.

						HUD_SetInstructorLocatorSpriteColor( locatorSprite,
							locatorR, locatorG, locatorB, locatorAlpha );
						if( locatorTarget->m_iHintId == -1 )
						{
							HUD_DrawInstructorLocatorSprite( locatorSprite,
								markerX - locatorDrawWidth / 2,
								markerY - locatorDrawHeight / 2,
								locatorIconWidth, locatorIconHeight,
								locatorDrawWidth, locatorDrawHeight,
								kRenderTransAdd, true, false, false, locatorAlpha );
						}
						else
						{
							// Use the sprite's per-pixel alpha so filtered samples at
							// the enlarged edge can blend smoothly with the scene.
							HUD_DrawInstructorLocatorSprite( locatorSprite,
								markerX - locatorDrawWidth / 2,
								markerY - locatorDrawHeight / 2,
								locatorIconWidth, locatorIconHeight,
								locatorDrawWidth, locatorDrawHeight,
								kRenderTransAlpha, false, false, false, locatorAlpha );
						}
					}
					else if( targetOffscreen && !sourceBindingIcon )
					{
						// If the requested Source icon has no GoldSrc .spr
						// counterpart, fall back to a direction marker.
						const int direction = locatorTarget->m_drawArrowDirection;
						const char *arrow = direction == INSTRUCTOR_LOCATOR_ARROW_LEFT ? "<" :
							( direction == INSTRUCTOR_LOCATOR_ARROW_RIGHT ? ">" :
							( direction == INSTRUCTOR_LOCATOR_ARROW_UP ? "^" : "v" ) );
						const int fallbackMarkerSize = HUD_ScaleInstructorPixel( 20 );
						FillRGBA( markerX - fallbackMarkerSize / 2,
							markerY - fallbackMarkerSize / 2, fallbackMarkerSize,
							fallbackMarkerSize, 5, 7, 10, locatorAlpha );
						if( !hasDirectionalLocatorSprites )
							DrawHudString( markerX - HUD_ScaleInstructorPixel( 4 ),
								markerY - HUD_ScaleInstructorPixel( 8 ),
								markerX + HUD_ScaleInstructorPixel( 10 ),
								arrow, locatorTextR, locatorTextG, locatorTextB );
					}
					else if( !sourceBindingIcon )
					{
						// Sprite-free on-screen fallback: a small reticle, not
						// the PS2 interaction brackets and not a tutorial card.
						const int reticleOuter = HUD_ScaleInstructorPixel( 18 );
						const int reticleInner = HUD_ScaleInstructorPixel( 16 );
						const int reticleThickness = HUD_ScaleInstructorPixel( 1 );
						const int reticleHalfOuter = reticleOuter / 2;
						const int reticleHalfInner = reticleInner / 2;
						const int reticleDot = HUD_ScaleInstructorPixel( 5 );
						FillRGBA( markerX - reticleHalfOuter, markerY - reticleHalfOuter,
							reticleOuter, reticleOuter, 5, 7, 10, locatorAlpha / 2 );
						FillRGBA( markerX - reticleHalfInner, markerY - reticleHalfInner,
							reticleInner, reticleThickness, locatorR, locatorG, locatorB, locatorAlpha );
						FillRGBA( markerX - reticleHalfInner, markerY + reticleHalfInner - reticleThickness,
							reticleInner, reticleThickness, locatorR, locatorG, locatorB, locatorAlpha );
						FillRGBA( markerX - reticleHalfInner, markerY - reticleHalfInner,
							reticleThickness, reticleInner, locatorR, locatorG, locatorB, locatorAlpha );
						FillRGBA( markerX + reticleHalfInner - reticleThickness, markerY - reticleHalfInner,
							reticleThickness, reticleInner, locatorR, locatorG, locatorB, locatorAlpha );
						FillRGBA( markerX - reticleDot / 2, markerY - reticleDot / 2,
							reticleDot, reticleDot, locatorR, locatorG, locatorB, locatorAlpha );
					}

					// Source's locator draws a separate direction indicator beside
					// the offscreen target icon. Reuse the two supplied originals:
				// left = horizontally flipped right; down = vertically flipped up.
					if( targetOffscreen && hasDirectionalLocatorSprites &&
						locatorTarget->m_drawArrowDirection != INSTRUCTOR_LOCATOR_ARROW_NONE )
					{
						HSPRITE arrowSprite = 0;
						bool flipArrowX = false;
						bool flipArrowY = false;
						int arrowX = markerX;
						int arrowY = markerY;
						const int arrowWidth = locatorArrowDrawSize;
					const int arrowHeight = locatorArrowDrawSize;

						switch( locatorTarget->m_drawArrowDirection )
						{
						case INSTRUCTOR_LOCATOR_ARROW_LEFT:
							arrowSprite = m_hInstructorHintArrowRightSprite;
							flipArrowX = true;
							arrowX = markerX - markerHalfWidth - locatorArrowGap - arrowWidth;
							arrowY = markerY - arrowHeight / 2;
							break;
						case INSTRUCTOR_LOCATOR_ARROW_RIGHT:
							arrowSprite = m_hInstructorHintArrowRightSprite;
							arrowX = markerX + markerHalfWidth + locatorArrowGap;
							arrowY = markerY - arrowHeight / 2;
							break;
						case INSTRUCTOR_LOCATOR_ARROW_UP:
							arrowSprite = m_hInstructorHintArrowUpSprite;
							arrowX = markerX - arrowWidth / 2;
							arrowY = markerY - markerHalfHeight - locatorArrowGap - arrowHeight;
							break;
						case INSTRUCTOR_LOCATOR_ARROW_DOWN:
							arrowSprite = m_hInstructorHintArrowUpSprite;
							flipArrowY = true;
							arrowX = markerX - arrowWidth / 2;
							arrowY = markerY + markerHalfHeight + locatorArrowGap;
							break;
						}
						if( arrowSprite )
						{
							const int arrowNativeWidth = SPR_Width( arrowSprite, 0 );
						const int arrowNativeHeight = SPR_Height( arrowSprite, 0 );
						HUD_SetInstructorLocatorSpriteColor( arrowSprite,
							locatorR, locatorG, locatorB, locatorAlpha );
						const bool arrowDrawn = HUD_DrawInstructorLocatorSprite(
							arrowSprite, arrowX, arrowY,
							arrowNativeWidth, arrowNativeHeight,
							arrowWidth, arrowHeight, kRenderTransAlpha, false,
							flipArrowX, flipArrowY, locatorAlpha );
						if( !arrowDrawn )
						{
							const char *arrow = locatorTarget->m_drawArrowDirection == INSTRUCTOR_LOCATOR_ARROW_LEFT ? "<" :
								( locatorTarget->m_drawArrowDirection == INSTRUCTOR_LOCATOR_ARROW_RIGHT ? ">" :
								( locatorTarget->m_drawArrowDirection == INSTRUCTOR_LOCATOR_ARROW_UP ? "^" : "v" ) );
							DrawHudString( markerX - HUD_ScaleInstructorPixel( 4 ),
								markerY - HUD_ScaleInstructorPixel( 8 ),
								markerX + HUD_ScaleInstructorPixel( 10 ),
								arrow, locatorTextR, locatorTextG, locatorTextB );
						}
						}
					}

					// The caption follows the icon as a compact locator label.
					// Binding text is shown in a small key chip on the same line.
					char locatorBinding[80];
					locatorBinding[0] = '\0';
					if( resolvedBinding[0] )
						snprintf( locatorBinding, sizeof( locatorBinding ), "%s", resolvedBinding );

					const int locatorCaptionWidth = HUD_GetInstructorCaptionWidth( locatorTarget->m_szCaption );
					const int locatorBindingWidth = DrawHudStringLen( locatorBinding );
					const bool locatorUsesGenericKey = m_hInstructorHintKeyGenericSprite &&
						locatorBinding[0] && locatorBinding[1] && !locatorBinding[2] ? true :
						( m_hInstructorHintKeyGenericSprite && locatorBinding[0] && !locatorBinding[1] );
					const bool hasLocatorText = locatorCaptionWidth > 0 || locatorBindingWidth > 0;

					// Match CLocatorPanel::DrawDynamicIcon: captions and ordinary
					// inline binding labels are limited by ICON_DIST_TOO_FAR.
					// FORCE_CAPTION bypasses both distance and occlusion limits.
					const bool forceCaption =
						( locatorTarget->m_iEffectsFlags & LOCATOR_ICON_FX_FORCE_CAPTION ) != 0;
					const bool captionWithinSourceRange = locatorStatic || forceCaption ||
						locatorTarget->m_flDistFromPlayer <= INSTRUCTOR_LOCATOR_CAPTION_MAX_DISTANCE;
					if( hasLocatorText && !sourceBindingIcon && !targetOffscreen &&
						captionWithinSourceRange )
					{
						int locatorKeyWidth = 0;
						int locatorKeyHeight = 0;
						if( locatorBindingWidth > 0 )
						{
							if( locatorUsesGenericKey )
							{
								HUD_GetInstructorSpriteSize( m_hInstructorHintKeyGenericSprite, 1.0f,
									&locatorKeyWidth, &locatorKeyHeight );
							}
							else
							{
								HUD_GetInstructorSpriteSize( 0, 1.0f, NULL, &locatorKeyHeight );
								const int fallbackPadding = HUD_ScaleInstructorPixel( 5 );
								locatorKeyWidth = locatorBindingWidth + fallbackPadding * 2;
							}
							if( !locatorUsesGenericKey && locatorKeyWidth > 64 )
								locatorKeyWidth = 64;
						}
						const int maxLabelWidth = Q_max( HUD_ScaleInstructorPixel( 32 ),
							ScreenWidth - HUD_ScaleInstructorPixel( 24 ) );
						int labelWidth = locatorCaptionWidth + locatorKeyWidth +
							HUD_ScaleInstructorPixel( 24 );
						if( labelWidth < HUD_ScaleInstructorPixel( 44 ) )
							labelWidth = HUD_ScaleInstructorPixel( 44 );
						if( labelWidth > maxLabelWidth ) labelWidth = maxLabelWidth;
						const int labelHeight = Q_max( locatorKeyHeight + HUD_ScaleInstructorPixel( 4 ),
							Q_max( HUD_ScaleInstructorPixel( 20 ),
								HUD_GetInstructorCaptionHeight() + HUD_ScaleInstructorPixel( 6 ) ) );
						int labelX = markerX - labelWidth / 2;
						int labelY = markerY + markerHalfHeight + HUD_ScaleInstructorPixel( 7 );

						if( targetOffscreen )
						{
							if( markerX <= locatorEdgeMargin + HUD_ScaleInstructorPixel( 2 ) )
							{
								labelX = markerX + markerHalfWidth + HUD_ScaleInstructorPixel( 6 );
								labelY = markerY - labelHeight / 2;
							}
							else if( markerX >= ScreenWidth - locatorEdgeMargin - HUD_ScaleInstructorPixel( 2 ) )
							{
								labelX = markerX - markerHalfWidth - labelWidth - HUD_ScaleInstructorPixel( 6 );
								labelY = markerY - labelHeight / 2;
							}
							else if( markerY <= locatorEdgeMargin + HUD_ScaleInstructorPixel( 2 ) )
							{
								labelY = markerY + markerHalfHeight + HUD_ScaleInstructorPixel( 6 );
							}
							else
							{
								labelY = markerY - markerHalfHeight - labelHeight - HUD_ScaleInstructorPixel( 6 );
							}
						}
						else if( labelY + labelHeight > ScreenHeight - HUD_ScaleInstructorPixel( 8 ) )
						{
							labelY = markerY - markerHalfHeight - labelHeight - HUD_ScaleInstructorPixel( 7 );
						}

						const int minLabelX = HUD_ScaleInstructorPixel( 8 );
						const int minLabelY = HUD_ScaleInstructorPixel( 8 );
						const int maxLabelX = Q_max( minLabelX,
							ScreenWidth - labelWidth - HUD_ScaleInstructorPixel( 8 ) );
						const int maxLabelY = Q_max( minLabelY,
							ScreenHeight - labelHeight - HUD_ScaleInstructorPixel( 8 ) );
						if( labelX < minLabelX ) labelX = minLabelX;
						if( labelX > maxLabelX ) labelX = maxLabelX;
						if( labelY < minLabelY ) labelY = minLabelY;
						if( labelY > maxLabelY ) labelY = maxLabelY;

						if( !targetOffscreen )
						{
							if( labelY < markerY )
								FillRGBA( markerX - 1, labelY + labelHeight,
									2, Q_max( 1, markerY - markerHalfHeight - labelY - labelHeight ),
									locatorR, locatorG, locatorB, locatorAlpha / 2 );
							else
								FillRGBA( markerX - 1, markerY + markerHalfHeight + 1,
									2, Q_max( 1, labelY - markerY - markerHalfHeight - 1 ),
									locatorR, locatorG, locatorB, locatorAlpha / 2 );
						}

						FillRGBA( labelX, labelY, labelWidth, labelHeight, 5, 7, 10, locatorAlpha * 3 / 4 );
						FillRGBA( labelX, labelY, labelWidth, HUD_ScaleInstructorPixel( 1 ),
							locatorR, locatorG, locatorB, locatorAlpha * 3 / 4 );
						FillRGBA( labelX, labelY + labelHeight - HUD_ScaleInstructorPixel( 1 ),
							labelWidth, HUD_ScaleInstructorPixel( 1 ), 0, 0, 0, locatorAlpha );

						const int labelTextY = labelY + HUD_ScaleInstructorPixel( 5 );
						const int labelTextX = labelX + HUD_ScaleInstructorPixel( 8 );
						const int keyX = labelX + labelWidth - locatorKeyWidth - HUD_ScaleInstructorPixel( 5 );
						const int captionRight = locatorKeyWidth > 0 ?
							keyX - HUD_ScaleInstructorPixel( 6 ) :
							labelX + labelWidth - HUD_ScaleInstructorPixel( 6 );
						if( locatorCaptionWidth > 0 )
							HUD_DrawInstructorCaption( labelTextX, labelTextY, captionRight,
								locatorTarget->m_szCaption, locatorTextR, locatorTextG, locatorTextB );

						if( locatorKeyWidth > 0 )
						{
							const int keyHeight = locatorKeyHeight;
							const int keyY = labelY + ( labelHeight - keyHeight ) / 2;
						bool keySpriteDrawn = false;
						if( locatorUsesGenericKey )
						{
							const int keyNativeWidth = SPR_Width( m_hInstructorHintKeyGenericSprite, 0 );
						const int keyNativeHeight = SPR_Height( m_hInstructorHintKeyGenericSprite, 0 );
						if( keyNativeWidth > 0 && keyNativeHeight > 0 &&
							keyNativeWidth <= 64 && keyNativeHeight <= 64 )
						{
							HUD_SetInstructorLocatorSpriteColor( m_hInstructorHintKeyGenericSprite,
								locatorR, locatorG, locatorB, locatorAlpha );
							keySpriteDrawn = HUD_DrawInstructorLocatorSprite(
								m_hInstructorHintKeyGenericSprite, keyX, keyY,
								keyNativeWidth, keyNativeHeight,
								locatorKeyWidth, keyHeight, kRenderTransAlpha, false,
								false, false, locatorAlpha );
						}
						}
						if( !keySpriteDrawn )
						{
							// Keep the old drawn key chip if the optional .spr is absent.
							FillRGBA( keyX, keyY, locatorKeyWidth, keyHeight, 12, 15, 19, locatorAlpha );
							FillRGBA( keyX, keyY, locatorKeyWidth, 1, locatorR, locatorG, locatorB, locatorAlpha / 2 );
						}
						if( keySpriteDrawn )
						HUD_DrawInstructorBindingName( keyX + locatorKeyWidth / 2,
							keyY + keyHeight / 2, locatorBinding, locatorAlpha );
					else
					{
						int keyTextX = keyX + ( locatorKeyWidth - locatorBindingWidth ) / 2;
						if( keyTextX < keyX + 3 )
							keyTextX = keyX + 3;
						DrawHudString( keyTextX, keyY + HUD_ScaleInstructorPixel( 4 ),
						keyX + locatorKeyWidth - HUD_ScaleInstructorPixel( 3 ),
							locatorBinding, locatorTextR, locatorTextG, locatorTextB );
					}
						}
					}

					if( sourceBindingIcon )
					{
						// Source's on-screen DrawBindingName() is inside
						// bShouldDrawCaption; its off-screen binding image is
						// drawn separately and remains available at any distance.
						if( targetOffscreen || captionWithinSourceRange )
							HUD_DrawInstructorBindingName( markerX, markerY,
								instructorBindingGlyph, locatorAlpha );
						const int sourceCaptionWidth = HUD_GetInstructorCaptionWidth( locatorTarget->m_szCaption );
						if( !targetOffscreen && captionWithinSourceRange && sourceCaptionWidth > 0 )
						{
							const int fontHeight = HUD_GetInstructorCaptionHeight();
							const int safeLeft = 8;
							const int safeRight = Q_max( safeLeft + 1, ScreenWidth - 8 );
							const int rightCaptionX = markerX + markerHalfWidth + 5;
							const int leftCaptionX = markerX - markerHalfWidth - 5 - sourceCaptionWidth;
							const int rightSpace = safeRight - rightCaptionX;
							const int leftSpace = markerX - markerHalfWidth - 5 - safeLeft;
							int captionX = rightSpace >= sourceCaptionWidth ? rightCaptionX :
								( leftSpace >= sourceCaptionWidth ? leftCaptionX :
								( rightSpace >= leftSpace ? rightCaptionX : leftCaptionX ) );
							const int maxCaptionX = Q_max( safeLeft, safeRight - sourceCaptionWidth );
							captionX = Q_max( safeLeft, Q_min( captionX, maxCaptionX ) );

							// Use the caption-specific scalable font and let it use the
							// full remaining safe width. This prevents the last letters
							// from being clipped when the icon is near either screen edge.
							HUD_DrawInstructorCaption( captionX, markerY - fontHeight / 2 - 1,
								safeRight, locatorTarget->m_szCaption,
								locatorTextR, locatorTextG, locatorTextB );
						}
					}
				}
			}
			else
			{
			// LOCATOR_ICON_FX_NO_OFFSCREEN deactivates the locator when it leaves
			// the viewport; don't leave a floating caption behind.
			if( !( targetOffscreen && ( locatorTarget->m_iEffectsFlags & 0x00000200 ) ) )
			{
			// Source has distinct icon_onscreen and icon_offscreen resources.
			// Automatic pushable guidance retains its own move/hold fallback sprites.
			HSPRITE hintIcon = targetOffscreen ?
				(HSPRITE)locatorTarget->m_hOffscreenSprite : (HSPRITE)locatorTarget->m_hOnscreenSprite;
			if( locatorTarget->m_iHintId == -1 )
			{
				hintIcon = 0;
				if( locatorTarget->m_iIconMode == 2 )
					hintIcon = m_hInstructorHintHoldSprite;
				else if( locatorTarget->m_iIconMode == 1 )
					hintIcon = m_hInstructorHintMoveSprite;
			}
			if( targetOffscreen && ( locatorTarget->m_iEffectsFlags & 0x00000200 ) )
				hintIcon = 0;

			int iconNativeWidth = hintIcon ? SPR_Width( hintIcon, 0 ) : 0;
			int iconNativeHeight = hintIcon ? SPR_Height( hintIcon, 0 ) : 0;
			int iconWidth = 0;
			int iconHeight = 0;
			if( !hintIcon || iconNativeWidth <= 0 || iconNativeHeight <= 0 ||
				iconNativeWidth > 64 || iconNativeHeight > 64 )
			{
				hintIcon = 0;
				iconNativeWidth = iconNativeHeight = 0;
			}
			else
			{
				HUD_GetInstructorSpriteSize( hintIcon, INSTRUCTOR_LOCATOR_ICON_BASE_SCALE,
					&iconWidth, &iconHeight );
			}

			const bool managedCard = locatorTarget->m_bManaged;
			const bool cardUsesGenericKey = m_hInstructorHintKeyGenericSprite &&
				bindingText[0] && bindingText[1] && !bindingText[2] ? true :
				( m_hInstructorHintKeyGenericSprite && bindingText[0] && !bindingText[1] );
			int keyChipWidth = 0;
			int keyChipHeight = 0;
			if( managedCard && bindingText[0] )
			{
				if( cardUsesGenericKey )
				{
					HUD_GetInstructorSpriteSize( m_hInstructorHintKeyGenericSprite, 1.0f,
						&keyChipWidth, &keyChipHeight );
				}
				else
				{
					HUD_GetInstructorSpriteSize( 0, 1.0f, NULL, &keyChipHeight );
					const int fallbackPadding = HUD_ScaleInstructorPixel( 8 );
					keyChipWidth = bindingWidth + fallbackPadding * 2;
				}
				if( !cardUsesGenericKey && keyChipWidth > 64 )
					keyChipWidth = 64;
			}
			const int contentTextWidth = managedCard ?
				captionWidth + ( keyChipWidth ? keyChipWidth + HUD_ScaleInstructorPixel( 12 ) : 0 ) :
				Q_max( captionWidth, bindingWidth );
			const int contentWidth = contentTextWidth +
				( hintIcon ? iconWidth + HUD_ScaleInstructorPixel( 12 ) : 0 );
			int panelWidth = contentWidth + HUD_ScaleInstructorPixel( managedCard ? 44 : 36 );
			const int maxPanelWidth = Q_max( 1, ScreenWidth - HUD_ScaleInstructorPixel( 24 ) );
			if( panelWidth > maxPanelWidth )
				panelWidth = maxPanelWidth;
			const int minPanelWidth = HUD_ScaleInstructorPixel( managedCard ? 224 : 180 );
			if( panelWidth < minPanelWidth && maxPanelWidth >= minPanelWidth )
				panelWidth = minPanelWidth;

			const int basePanelHeight = HUD_ScaleInstructorPixel(
				managedCard ? 62 : ( bindingText[0] ? 58 : 42 ) );
			const int panelHeight = Q_max( basePanelHeight,
				Q_max( iconHeight + HUD_ScaleInstructorPixel( managedCard ? 24 : 16 ),
					keyChipHeight > 0 ? keyChipHeight + HUD_ScaleInstructorPixel( 24 ) : 0 ) );
			int panelX = ( ScreenWidth - panelWidth ) / 2;
			int panelY = ScreenHeight - panelHeight - HUD_ScaleInstructorPixel( 30 );

			if( locatorTarget->m_bFixedPosition )
			{
				panelX = (int)( locatorTarget->m_vecOrigin[0] * ScreenWidth - panelWidth * 0.5f );
				panelY = (int)( locatorTarget->m_vecOrigin[1] * ScreenHeight - panelHeight * 0.5f );
			}
			else if( !hasTarget )
			{
				// Screen-space/no-target cards are positioned by the shared
				// locator declutter pass below.
			}
			else if( hasTarget )
			{
				const float centerX = ScreenWidth * 0.5f;
				const float centerY = ScreenHeight * 0.5f;
				const int edgeMargin = HUD_ScaleInstructorPixel( 24 );
				const bool offscreen = targetBehind ||
					targetX < edgeMargin || targetX > ScreenWidth - edgeMargin ||
					targetY < edgeMargin || targetY > ScreenHeight - edgeMargin;

				if( !offscreen )
				{
					// Keep the contextual caption close to the object, but never
					// allow the panel to spill outside the HUD safe area.
					panelX = (int)( targetX - panelWidth * 0.5f );
					panelY = (int)( targetY + 28.0f );
					if( panelY + panelHeight > ScreenHeight - 12 )
						panelY = (int)( targetY - panelHeight - 24.0f );
				}
				else
				{
					// Clamp a ray from screen center to the viewport edge. The
					// arrow glyph points toward the target; its caption card sits
					// just inside the same edge.
					float dx = targetX - centerX;
					float dy = targetY - centerY;
					if( fabsf( dx ) < 1.0f && fabsf( dy ) < 1.0f )
					dy = 1.0f; // directly behind: use the lower edge as a stable fallback

					const float left = (float)edgeMargin;
					const float right = (float)( ScreenWidth - edgeMargin );
					const float top = (float)edgeMargin;
					const float bottom = (float)( ScreenHeight - edgeMargin );
					float tx = 1000000.0f;
				float ty = 1000000.0f;

					if( dx > 0.001f )
					tx = ( right - centerX ) / dx;
					else if( dx < -0.001f )
					tx = ( left - centerX ) / dx;

					if( dy > 0.001f )
					ty = ( bottom - centerY ) / dy;
					else if( dy < -0.001f )
					ty = ( top - centerY ) / dy;

					float t = tx < ty ? tx : ty;
					if( t < 0.0f || t > 1000000.0f )
					t = 0.0f;

					const int arrowX = (int)( centerX + dx * t );
					const int arrowY = (int)( centerY + dy * t );
					const char *arrow = fabsf( dx ) > fabsf( dy ) ?
						( dx < 0.0f ? "<" : ">" ) :
						( dy < 0.0f ? "^" : "v" );

					const int cardArrowSize = HUD_ScaleInstructorPixel( 24 );
					FillRGBA( arrowX - cardArrowSize / 2, arrowY - cardArrowSize / 2,
						cardArrowSize, cardArrowSize, 8, 10, 14, 225 );
					DrawHudString( arrowX - HUD_ScaleInstructorPixel( 4 ),
						arrowY - HUD_ScaleInstructorPixel( 7 ),
						arrowX + HUD_ScaleInstructorPixel( 12 ), arrow,
						locatorTarget->m_iCaptionColorR, locatorTarget->m_iCaptionColorG,
						locatorTarget->m_iCaptionColorB );

					if( arrowX <= edgeMargin + 2 )
				{
					panelX = edgeMargin + HUD_ScaleInstructorPixel( 22 );
					panelY = arrowY - panelHeight / 2;
				}
				else if( arrowX >= ScreenWidth - edgeMargin - 2 )
				{
					panelX = ScreenWidth - edgeMargin - panelWidth - HUD_ScaleInstructorPixel( 22 );
					panelY = arrowY - panelHeight / 2;
				}
				else if( arrowY <= edgeMargin + 2 )
				{
					panelX = arrowX - panelWidth / 2;
					panelY = edgeMargin + HUD_ScaleInstructorPixel( 22 );
				}
				else
				{
					panelX = arrowX - panelWidth / 2;
					panelY = ScreenHeight - edgeMargin - panelHeight - HUD_ScaleInstructorPixel( 22 );
				}
				}
			}

			// Apply the panel-wide collision offset for fixed, no-target, and
			// world-projected cards before clamping to the safe viewport.
			panelX += locatorTarget->m_offsetX;
			panelY += locatorTarget->m_offsetY;

			// Clamp the card after choosing its target-relative or edge-relative
			// placement. Keep a small inset even with unusual screen sizes.
			const int maxX = Q_max( 0, ScreenWidth - panelWidth );
			const int maxY = Q_max( 0, ScreenHeight - panelHeight );
			if( panelX < 12 ) panelX = 12;
			if( panelX > maxX - 12 ) panelX = Q_max( 0, maxX - 12 );
			if( panelY < 12 ) panelY = 12;
			if( panelY > maxY - 12 ) panelY = Q_max( 0, maxY - 12 );

			// Approximate Source locator FX flags using the GoldSrc HUD primitives.
			int panelAlpha = locatorTarget->GetRenderAlpha( 205 );
			panelX += (int)( locatorTarget->m_flShakeX * 1.5f );
			panelY += (int)( locatorTarget->m_flShakeY );

			int hintR = locatorTarget->m_iCaptionColorR;
			int hintG = locatorTarget->m_iCaptionColorG;
			int hintB = locatorTarget->m_iCaptionColorB;
			int hintTextR = hintR * panelAlpha / 255;
			int hintTextG = hintG * panelAlpha / 255;
			int hintTextB = hintB * panelAlpha / 255;
			const int whiteText = 245 * panelAlpha / 255;

			if( managedCard )
			{
				// A Source-inspired layered card: subtle drop shadow, inset face,
				// crisp edge and a restrained locator-color accent.
				if( hintR == 255 && hintG == 255 && hintB == 255 )
				{
					hintR = 112;
					hintG = 180;
					hintB = 255;
					hintTextR = hintR * panelAlpha / 255;
					hintTextG = hintG * panelAlpha / 255;
					hintTextB = hintB * panelAlpha / 255;
				}

				const int cardBorder = HUD_ScaleInstructorPixel( 1 );
				const int cardShadowX = HUD_ScaleInstructorPixel( 2 );
				const int cardShadowY = HUD_ScaleInstructorPixel( 3 );
				const int cardSideAccent = HUD_ScaleInstructorPixel( 3 );
				FillRGBA( panelX + cardShadowX, panelY + cardShadowY, panelWidth, panelHeight,
					0, 0, 0, panelAlpha / 2 );
				FillRGBA( panelX, panelY, panelWidth, panelHeight, 4, 6, 10, panelAlpha );
				FillRGBA( panelX + cardBorder, panelY + cardBorder,
					panelWidth - 2 * cardBorder, panelHeight - 2 * cardBorder,
					19, 24, 33, panelAlpha );
				FillRGBA( panelX + cardBorder, panelY + cardBorder,
					panelWidth - 2 * cardBorder, cardBorder, 74, 86, 104, panelAlpha );
				FillRGBA( panelX + cardBorder, panelY + panelHeight - 2 * cardBorder,
					panelWidth - 2 * cardBorder, cardBorder, 5, 7, 11, panelAlpha );
				FillRGBA( panelX + cardBorder, panelY + cardBorder,
					cardSideAccent, panelHeight - 2 * cardBorder, hintR, hintG, hintB, panelAlpha );
				FillRGBA( panelX + HUD_ScaleInstructorPixel( 5 ),
					panelY + HUD_ScaleInstructorPixel( 5 ), cardBorder,
					panelHeight - HUD_ScaleInstructorPixel( 10 ), hintR, hintG, hintB, panelAlpha / 3 );
			}
			else
			{
				// Preserve the established env_instructor_hint / pushable styling.
				FillRGBA( panelX, panelY, panelWidth, panelHeight, 8, 10, 14, panelAlpha );
				FillRGBA( panelX, panelY, HUD_ScaleInstructorPixel( 3 ),
					panelHeight, hintR, hintG, hintB, panelAlpha );
			}

			int textAreaX = panelX + HUD_ScaleInstructorPixel( managedCard ? 16 : 14 );
			if( hintIcon )
			{
				const int iconX = panelX + HUD_ScaleInstructorPixel( 12 );
				const int iconY = panelY + ( panelHeight - iconHeight ) / 2;
				if( managedCard )
				{
					const int iconFrameInset = HUD_ScaleInstructorPixel( 5 );
					const int iconFrameExtra = HUD_ScaleInstructorPixel( 10 );
					FillRGBA( iconX - iconFrameInset, iconY - iconFrameInset,
						iconWidth + iconFrameExtra, iconHeight + iconFrameExtra,
						31, 39, 52, panelAlpha );
					FillRGBA( iconX - iconFrameInset, iconY - iconFrameInset,
						iconWidth + iconFrameExtra, HUD_ScaleInstructorPixel( 1 ),
						hintR, hintG, hintB, panelAlpha );
				}
				HUD_SetInstructorLocatorSpriteColor( hintIcon,
					hintR, hintG, hintB, panelAlpha );
				HUD_DrawInstructorLocatorSprite( hintIcon, iconX, iconY,
					iconNativeWidth, iconNativeHeight, iconWidth, iconHeight,
					kRenderTransAdd, true, false, false, panelAlpha );
				textAreaX = iconX + iconWidth + HUD_ScaleInstructorPixel( 12 );
			}

			const int keyX = panelX + panelWidth - keyChipWidth - HUD_ScaleInstructorPixel( 12 );
			const int textAreaRight = ( managedCard && keyChipWidth ) ?
				keyX - HUD_ScaleInstructorPixel( 10 ) :
				panelX + panelWidth - HUD_ScaleInstructorPixel( managedCard ? 14 : 10 );
			const int textAreaWidth = Q_max( 1, textAreaRight - textAreaX );

			if( managedCard )
			{
				DrawHudString( textAreaX, panelY + HUD_ScaleInstructorPixel( 9 ), textAreaRight,
					"INSTRUCTION", hintTextR, hintTextG, hintTextB );
				HUD_DrawInstructorCaption( textAreaX, panelY + HUD_ScaleInstructorPixel( 31 ), textAreaRight,
					locatorTarget->m_szCaption, whiteText, whiteText, whiteText );

				if( bindingText[0] )
				{
					const int cardKeyHeight = keyChipHeight;
					const int keyY = panelY + ( panelHeight - cardKeyHeight ) / 2 +
						HUD_ScaleInstructorPixel( 2 );
					bool keySpriteDrawn = false;
					if( cardUsesGenericKey )
					{
						const int keyNativeWidth = SPR_Width( m_hInstructorHintKeyGenericSprite, 0 );
						const int keyNativeHeight = SPR_Height( m_hInstructorHintKeyGenericSprite, 0 );
						if( keyNativeWidth > 0 && keyNativeHeight > 0 &&
							keyNativeWidth <= 64 && keyNativeHeight <= 64 )
						{
							HUD_SetInstructorLocatorSpriteColor( m_hInstructorHintKeyGenericSprite,
								hintR, hintG, hintB, panelAlpha );
							keySpriteDrawn = HUD_DrawInstructorLocatorSprite(
								m_hInstructorHintKeyGenericSprite, keyX, keyY,
								keyNativeWidth, keyNativeHeight,
								keyChipWidth, cardKeyHeight, kRenderTransAlpha, false,
								false, false, panelAlpha );
						}
					}
					if( !keySpriteDrawn )
					{
						FillRGBA( keyX, keyY, keyChipWidth, cardKeyHeight, 10, 14, 21, panelAlpha );
						FillRGBA( keyX, keyY, keyChipWidth, 1, hintR, hintG, hintB, panelAlpha );
						FillRGBA( keyX, keyY + cardKeyHeight - 1, keyChipWidth, 1, 63, 73, 89, panelAlpha );
					}
					if( keySpriteDrawn )
						HUD_DrawInstructorBindingName( keyX + keyChipWidth / 2,
							keyY + cardKeyHeight / 2, bindingText, panelAlpha );
					else
					{
						int keyTextX = keyX + ( keyChipWidth - bindingWidth ) / 2;
						if( keyTextX < keyX + HUD_ScaleInstructorPixel( 3 ) )
							keyTextX = keyX + HUD_ScaleInstructorPixel( 3 );
						DrawHudString( keyTextX, keyY + HUD_ScaleInstructorPixel( 6 ),
							keyX + keyChipWidth - HUD_ScaleInstructorPixel( 5 ),
							bindingText, hintTextR, hintTextG, hintTextB );
					}
				}
			}
			else
			{
				int textX = textAreaX + ( textAreaWidth - captionWidth ) / 2;
				if( textX < textAreaX )
					textX = textAreaX;
				const int textOffsetY = ( panelHeight - basePanelHeight ) / 2;
				HUD_DrawInstructorCaption( textX,
					panelY + textOffsetY + HUD_ScaleInstructorPixel( 9 ), textAreaRight,
					locatorTarget->m_szCaption, whiteText, whiteText, whiteText );

				if( bindingText[0] )
				{
					int keyTextX = textAreaX + ( textAreaWidth - bindingWidth ) / 2;
					if( keyTextX < textAreaX )
						keyTextX = textAreaX;
					DrawHudString( keyTextX,
						panelY + textOffsetY + HUD_ScaleInstructorPixel( 32 ), textAreaRight,
						bindingText, hintTextR, hintTextG, hintTextB );
				}
			}
			}
			}
		}
	}

	}

	// are we in demo mode? do we need to draw the logo in the top corner?
	if( m_iLogo )
	{
		int x, y, i;

		if( m_hsprLogo == 0 )
			m_hsprLogo = LoadSprite( "sprites/%d_logo.spr" );

		SPR_Set( m_hsprLogo, 250, 250, 250 );

		x = SPR_Width( m_hsprLogo, 0 );
		x = ScreenWidth - x;
		y = SPR_Height( m_hsprLogo, 0 ) / 2;

		// Draw the logo at 20 fps
		int iFrame = (int)( flTime * 20 ) % MAX_LOGO_FRAMES;
		i = grgLogoFrame[iFrame] - 1;

		SPR_DrawAdditive( i, x, y, NULL );
	}

	/*
	if( g_iVisibleMouse )
	{
		void IN_GetMousePos( int *mx, int *my );
		int mx, my;

		IN_GetMousePos( &mx, &my );

		if( m_hsprCursor == 0 )
		{
			m_hsprCursor = SPR_Load( "sprites/cursor.spr" );
		}

		SPR_Set( m_hsprCursor, 250, 250, 250 );

		// Draw the logo at 20 fps
		SPR_DrawAdditive( 0, mx, my, NULL );
	}
	*/

	return 1;
}

void ScaleColors( int &r, int &g, int &b, int a )
{
	float x = (float)a / 255;
	r = (int)( r * x );
	g = (int)( g * x );
	b = (int)( b * x );
}

const unsigned char colors[8][3] =
{
{127, 127, 127}, // additive cannot be black
{255,   0,   0},
{  0, 255,   0},
{255, 255,   0},
{  0,   0, 255},
{  0, 255, 255},
{255,   0, 255},
{240, 180,  24}
};

// Draw a HUD string with independent logical and physical start positions.
// The entire string follows the overscan transform, but glyphs keep their native
// width and advance so they cannot collide as the safe area gets narrower.
static int HUD_DrawHudStringAt( int xpos, int drawXpos, int ypos, int iMaxX, const char *szIt, int r, int g, int b )
{
	HUD_ApplyTextColor( r, g, b );

	const int logicalStartX = xpos;
	const int drawYpos = HUD_AdjustY( ypos );
	const int drawMaxX = HUD_AdjustX( iMaxX );

	if( hud_textmode->value == 2 )
	{
		gEngfuncs.pfnDrawSetTextColor( r / 255.0, g / 255.0, b / 255.0 );
		return gEngfuncs.pfnDrawConsoleString( drawXpos, drawYpos, (char*) szIt );
	}

	// xash3d: reset unicode state
	TextMessageDrawChar( 0, 0, 0, 0, 0, 0 );

	// draw the string until we hit the null character or a newline character
	for( ; *szIt != 0 && *szIt != '\n'; szIt++ )
	{
		int w = gHUD.m_scrinfo.charWidths['M'];
		int glyphX = drawXpos + ( xpos - logicalStartX );
		if( glyphX + w > drawMaxX )
			return xpos;
		if( ( *szIt == '^' ) && ( *( szIt + 1 ) >= '0') && ( *( szIt + 1 ) <= '9') )
		{
			szIt++;
			int index = (*szIt - '0') & 7;
			r = colors[index][0];
			g = colors[index][1];
			b = colors[index][2];
			if( !*(++szIt) )
				return xpos;
		}
		int c = (unsigned int)(unsigned char)*szIt;

		xpos += TextMessageDrawCharAt( glyphX, drawYpos, c, r, g, b );
	}

	return xpos;
}


static bool HUD_HasInstructorScalableFont( void )
{
	return gEngfuncs.pfnDrawConsoleString &&
		gEngfuncs.pfnDrawConsoleStringLen &&
		gEngfuncs.pfnDrawSetTextColor;
}

// Source's InstructorTitle is a bold condensed face. GoldSrc does not
// expose Source's VGUI HFont handle, so scale the active Xash font locally.
// Caption scale is read from a live cvar; measuring and drawing use matching
// scaled-font metrics for stable layout.
static int HUD_GetInstructorGlyphBytes( const char *text )
{
	const unsigned char lead = (unsigned char)text[0];
	int charBytes = 1;

	if( ( lead & 0xE0 ) == 0xC0 ) charBytes = 2;
	else if( ( lead & 0xF0 ) == 0xE0 ) charBytes = 3;
	else if( ( lead & 0xF8 ) == 0xF0 ) charBytes = 4;

	for( int i = 1; i < charBytes; ++i )
	{
		if( !text[i] || ( ( (unsigned char)text[i] & 0xC0 ) != 0x80 ) )
			return 1;
	}

	return charBytes;
}

static float HUD_GetInstructorCaptionScale( void )
{
	float scale = CVAR_GET_FLOAT( "hud_instructor_caption_scale" );
	if( scale < 0.5f ) scale = 0.5f;
	if( scale > 2.0f ) scale = 2.0f;
	return scale;
}

static float HUD_GetInstructorScale( void )
{
	float scale = CVAR_GET_FLOAT( "hud_instructor_scale" );
	if( scale < 0.5f ) scale = 0.5f;
	if( scale > 2.0f ) scale = 2.0f;
	return scale;
}

static int HUD_ScaleInstructorPixel( int value )
{
	if( value <= 0 )
		return 0;
	return Q_max( 1, (int)( (float)value * HUD_GetInstructorScale() + 0.5f ) );
}

static void HUD_GetInstructorSpriteSize( HSPRITE sprite, float baseScale,
	int *width, int *height )
{
	int nativeWidth = sprite ? SPR_Width( sprite, 0 ) : 32;
	int nativeHeight = sprite ? SPR_Height( sprite, 0 ) : 32;

	// Missing/invalid optional artwork uses a square 32px fallback badge.
	if( nativeWidth <= 0 || nativeHeight <= 0 ||
		nativeWidth > 64 || nativeHeight > 64 )
		nativeWidth = nativeHeight = 32;

	float scale = HUD_GetInstructorScale() * baseScale;
	// Bound the largest rendered side while preserving the sprite's native aspect ratio.
	const int largestNativeDimension = Q_max( nativeWidth, nativeHeight );
	if( largestNativeDimension * scale > 256.0f )
		scale = 256.0f / (float)largestNativeDimension;
	if( width )
		*width = Q_max( 1, (int)( nativeWidth * scale + 0.5f ) );
	if( height )
		*height = Q_max( 1, (int)( nativeHeight * scale + 0.5f ) );
}

static int HUD_GetInstructorKeyTextScale( void )
{
	int scale = (int)( CVAR_GET_FLOAT( "hud_instructor_key_text_scale" ) + 0.5f );
	if( scale < 1 ) scale = 1;
	if( scale > 4 ) scale = 4;
	return scale;
}

static int HUD_GetInstructorGlyphAdvance( const char *glyph )
{
	int width = 0;
	int height = 0;

	if( gEngfuncs.pfnDrawConsoleStringLenScaled )
		gEngfuncs.pfnDrawConsoleStringLenScaled( glyph, &width, &height,
			HUD_GetInstructorCaptionScale() );
	else
		gEngfuncs.pfnDrawConsoleStringLen( glyph, &width, &height );
	if( width <= 1 )
		return 1;

	// Keep the measured advance identical to Xash's scaled string renderer.
	// Extra condensation here made adjacent glyph quads overlap at enlarged sizes.
	return width;
}

static int HUD_GetInstructorCaptionWidth( const char *text )
{
	if( !text || !text[0] )
		return 0;

	if( HUD_HasInstructorScalableFont() )
	{
		int width = 0;
		bool hasGlyph = false;

		while( text[0] && text[0] != '\n' )
		{
			char glyph[5];
			const int charBytes = HUD_GetInstructorGlyphBytes( text );
			memcpy( glyph, text, (size_t)charBytes );
			glyph[charBytes] = '\0';

			width += HUD_GetInstructorGlyphAdvance( glyph );
			hasGlyph = true;
			text += charBytes;
		}

		// Keep a small allowance for the final glyph's edge pixels in locator layout.
		return hasGlyph ? width + 2 : 0;
	}

	return gHUD.DrawHudStringLen( text );
}

static void HUD_DrawInstructorCaptionText( int x, int y, const char *text )
{
	if( !text || !text[0] )
		return;

	// Submit the complete caption in one pass. This lets Xash apply its normal
	// glyph advances without per-character integer rounding or overlap.
	if( gEngfuncs.pfnDrawConsoleStringScaled )
		gEngfuncs.pfnDrawConsoleStringScaled( x, y, text,
			HUD_GetInstructorCaptionScale() );
	else
		gEngfuncs.pfnDrawConsoleString( x, y, (char *)text );
}

static int HUD_GetInstructorCaptionHeight( void )
{
	if( HUD_HasInstructorScalableFont() )
	{
		int width = 0;
		int height = 0;
		if( gEngfuncs.pfnDrawConsoleStringLenScaled )
			gEngfuncs.pfnDrawConsoleStringLenScaled( "M", &width, &height,
				HUD_GetInstructorCaptionScale() );
		else
			gEngfuncs.pfnDrawConsoleStringLen( "M", &width, &height );
		if( height > 0 )
			return height;
	}

	if( gHUD.m_scrinfo.iCharHeight > 0 )
		return gHUD.m_scrinfo.iCharHeight;
	return gHUD.m_iFontHeight > 0 ? gHUD.m_iFontHeight : 12;
}

// Build the longest complete UTF-8 prefix that fits the caption's available
// width. Xash's console-font renderer has accurate variable-width metrics, but
// unlike DrawHudString it does not accept a right-edge clipping coordinate.
static void HUD_CopyInstructorCaptionFit( const char *source, int maxWidth,
	char *output, size_t outputSize, int *outputWidth )
{
	size_t used = 0;
	int acceptedAdvance = 0;
	int acceptedWidth = 0;

	if( outputWidth )
		*outputWidth = 0;
	if( !output || outputSize == 0 )
		return;
	output[0] = '\0';

	if( !source || maxWidth <= 0 )
		return;

	while( source[0] && source[0] != '\n' )
	{
		const int charBytes = HUD_GetInstructorGlyphBytes( source );

		if( used + (size_t)charBytes >= outputSize )
			break;

		memcpy( output + used, source, (size_t)charBytes );
		used += (size_t)charBytes;
		output[used] = '\0';

		char glyph[5];
		memcpy( glyph, source, (size_t)charBytes );
		glyph[charBytes] = '\0';
		const int candidateAdvance = acceptedAdvance + HUD_GetInstructorGlyphAdvance( glyph );
		const int candidateWidth = candidateAdvance + 2;
		if( candidateWidth > maxWidth )
		{
			used -= (size_t)charBytes;
			output[used] = '\0';
			break;
		}

		acceptedAdvance = candidateAdvance;
		acceptedWidth = candidateWidth;
		source += charBytes;
	}

	if( outputWidth )
		*outputWidth = acceptedWidth;
}

// GoldSrc has no Source VGUI HFont handles. Xash's scaled console-font
// callback enlarges these captions locally without changing console text;
// non-Xash engines retain the native HUD-font fallback.
static int HUD_DrawInstructorCaption( int x, int y, int maxX,
	const char *text, int r, int g, int b )
{
	if( !text || !text[0] || maxX <= x )
		return x;

	if( HUD_HasInstructorScalableFont() )
	{
		const int drawX = HUD_AdjustX( x );
		const int drawY = HUD_AdjustY( y );
		const int drawMaxX = HUD_AdjustX( maxX );
		char visibleText[256];
		int visibleWidth = 0;

		HUD_CopyInstructorCaptionFit( text, drawMaxX - drawX,
			visibleText, sizeof( visibleText ), &visibleWidth );
		if( !visibleText[0] )
			return x;

		gEngfuncs.pfnDrawSetTextColor( r / 255.0f, g / 255.0f, b / 255.0f );
		HUD_DrawInstructorCaptionText( drawX, drawY, visibleText );
		return x + visibleWidth;
	}

	// The legacy fallback also receives the Source-style offset shadow.
	if( r != 0 || g != 0 || b != 0 )
		HUD_DrawHudStringAt( x + 1, HUD_AdjustX( x ) + 1,
			y + 1, maxX + 1, text, 0, 0, 0 );
	return HUD_DrawHudStringAt( x, HUD_AdjustX( x ),
		y, maxX, text, r, g, b );
}

int CHud::DrawHudString( int xpos, int ypos, int iMaxX, const char *szIt, int r, int g, int b )
{
	return HUD_DrawHudStringAt( xpos, HUD_AdjustX( xpos ), ypos, iMaxX, szIt, r, g, b );
}

int DrawUtfString( int xpos, int ypos, int iMaxX, const char *szIt, int r, int g, int b )
{
	HUD_ApplyTextColor( r, g, b );

	if (IsXashFWGS())
	{
		const int logicalStartX = xpos;
		const int drawStartX = HUD_AdjustX( xpos );
		const int drawYpos = HUD_AdjustY( ypos );
		const int drawMaxX = HUD_AdjustX( iMaxX );

		// xash3d: reset unicode state
		gEngfuncs.pfnVGUI2DrawCharacterAdditive( 0, 0, 0, 0, 0, 0, 0 );

		// Keep native glyph spacing and move the complete text run to the safe area.
		for( ; *szIt != 0 && *szIt != '\n'; szIt++ )
		{
			int w = gHUD.m_scrinfo.charWidths['M'];
			int glyphX = drawStartX + ( xpos - logicalStartX );
			if( glyphX + w > drawMaxX )
				return xpos;
			if( ( *szIt == '^' ) && ( *( szIt + 1 ) >= '0') && ( *( szIt + 1 ) <= '9') )
			{
				szIt++;
				int index = (*szIt - '0') & 7;
				r = colors[index][0];
				g = colors[index][1];
				b = colors[index][2];
				if( !*(++szIt) )
					return xpos;
			}
			int c = (unsigned int)(unsigned char)*szIt;
			xpos += gEngfuncs.pfnVGUI2DrawCharacterAdditive( glyphX, drawYpos, c, r, g, b, 0 );
		}
		return xpos;
	}
	else
	{
		return gHUD.DrawHudString(xpos, ypos, iMaxX, szIt, r, g, b);
	}
}

int CHud::DrawHudStringLen( const char *szIt )
{
	int l = 0;
	for( ; *szIt != 0 && *szIt != '\n'; szIt++ )
	{
		l += gHUD.m_scrinfo.charWidths[(unsigned char)*szIt];
	}
	return l;
}

int CHud::DrawHudNumberString( int xpos, int ypos, int iMinX, int iNumber, int r, int g, int b )
{
	char szString[32];
	sprintf( szString, "%d", iNumber );
	return DrawHudStringReverse( xpos, ypos, iMinX, szString, r, g, b );
}

// draws a string from right to left (right-aligned)
int CHud::DrawHudStringReverse( int xpos, int ypos, int iMinX, const char *szString, int r, int g, int b )
{
	const int originalRightX = xpos;
	int stringWidth = 0;

	// Keep the string's native width and align its right edge to the safe-area anchor.
	for( const char *szIt = szString; *szIt != 0; szIt++ )
		stringWidth += gHUD.m_scrinfo.charWidths[(unsigned char)*szIt];

	xpos -= stringWidth;
	if( xpos < iMinX )
	{
		xpos = iMinX;
		HUD_DrawHudStringAt( xpos, HUD_AdjustX( xpos ), ypos, gHUD.m_scrinfo.iWidth, szString, r, g, b );
	}
	else
	{
		HUD_DrawHudStringAt( xpos, HUD_AdjustX( originalRightX ) - stringWidth,
			ypos, gHUD.m_scrinfo.iWidth, szString, r, g, b );
	}
	return xpos;
}

int CHud::DrawHudNumber( int x, int y, int iFlags, int iNumber, int r, int g, int b )
{
	int iWidth = GetSpriteRect( m_HUD_number_0 ).right - GetSpriteRect( m_HUD_number_0 ).left;
	int k;
	
	if( iNumber > 0 )
	{
		// SPR_Draw 100's
		if( iNumber >= 100 )
		{
			k = iNumber / 100;
			SPR_Set( GetSprite( m_HUD_number_0 + k ), r, g, b );
			SPR_DrawAdditive( 0, x, y, &GetSpriteRect( m_HUD_number_0 + k ) );
			x += iWidth;
		}
		else if( iFlags & ( DHN_3DIGITS ) )
		{
			//SPR_DrawAdditive( 0, x, y, &rc );
			x += iWidth;
		}

		// SPR_Draw 10's
		if( iNumber >= 10 )
		{
			k = ( iNumber % 100 ) / 10;
			SPR_Set( GetSprite( m_HUD_number_0 + k ), r, g, b );
			SPR_DrawAdditive( 0, x, y, &GetSpriteRect( m_HUD_number_0 + k ) );
			x += iWidth;
		}
		else if( iFlags & ( DHN_3DIGITS | DHN_2DIGITS ) )
		{
			//SPR_DrawAdditive( 0, x, y, &rc );
			x += iWidth;
		}

		// SPR_Draw ones
		k = iNumber % 10;
		SPR_Set( GetSprite( m_HUD_number_0 + k ), r, g, b );
		SPR_DrawAdditive( 0,  x, y, &GetSpriteRect( m_HUD_number_0 + k ) );
		x += iWidth;
	}
	else if( iFlags & DHN_DRAWZERO )
	{
		SPR_Set( GetSprite( m_HUD_number_0 ), r, g, b );

		// SPR_Draw 100's
		if( iFlags & ( DHN_3DIGITS ) )
		{
			//SPR_DrawAdditive( 0, x, y, &rc );
			x += iWidth;
		}

		if( iFlags & ( DHN_3DIGITS | DHN_2DIGITS ) )
		{
			//SPR_DrawAdditive( 0, x, y, &rc );
			x += iWidth;
		}

		// SPR_Draw ones
		SPR_DrawAdditive( 0,  x, y, &GetSpriteRect( m_HUD_number_0 ) );
		x += iWidth;
	}

	return x;
}

int CHud::DrawHudNumberSm( int x, int y, int iFlags, int iNumber, int r, int g, int b )
{
	const int numberBase = ( m_HUD_number_sm_0 >= 0 ) ? m_HUD_number_sm_0 : m_HUD_number_0;
	const int digitWidth = GetSpriteRect( numberBase ).right - GetSpriteRect( numberBase ).left;
	int digit;

	if( digitWidth <= 0 )
		return x;

	if( iNumber < 0 )
		iNumber = 0;

	if( iNumber > 0 )
	{
		if( iNumber >= 100 )
		{
			digit = iNumber / 100;
			SPR_Set( GetSprite( numberBase + digit ), r, g, b );
			SPR_DrawAdditive( 0, x, y, &GetSpriteRect( numberBase + digit ) );
			x += digitWidth;
		}
		else if( iFlags & DHN_3DIGITS_SM )
			x += digitWidth;

		if( iNumber >= 10 )
		{
			digit = ( iNumber % 100 ) / 10;
			SPR_Set( GetSprite( numberBase + digit ), r, g, b );
			SPR_DrawAdditive( 0, x, y, &GetSpriteRect( numberBase + digit ) );
			x += digitWidth;
		}
		else if( iFlags & ( DHN_3DIGITS_SM | DHN_2DIGITS_SM ) )
			x += digitWidth;

		digit = iNumber % 10;
		SPR_Set( GetSprite( numberBase + digit ), r, g, b );
		SPR_DrawAdditive( 0, x, y, &GetSpriteRect( numberBase + digit ) );
		x += digitWidth;
	}
	else if( iFlags & DHN_DRAWZERO_SM )
	{
		SPR_Set( GetSprite( numberBase ), r, g, b );
		if( iFlags & DHN_3DIGITS_SM )
			x += digitWidth;
		if( iFlags & ( DHN_3DIGITS_SM | DHN_2DIGITS_SM ) )
			x += digitWidth;
		SPR_DrawAdditive( 0, x, y, &GetSpriteRect( numberBase ) );
		x += digitWidth;
	}

	return x;
}

int CHud::GetNumWidth( int iNumber, int iFlags )
{
	if( iFlags & ( DHN_3DIGITS | DHN_3DIGITS_SM ) )
		return 3;

	if( iFlags & ( DHN_2DIGITS | DHN_2DIGITS_SM ) )
		return 2;

	if( iNumber <= 0 )
	{
		if( iFlags & ( DHN_DRAWZERO | DHN_DRAWZERO_SM ) )
			return 1;
		else
			return 0;
	}

	if( iNumber < 10 )
		return 1;

	if( iNumber < 100 )
		return 2;

	return 3;
}	

void CHud::DrawDarkRectangle( int x, int y, int wide, int tall )
{
	//gEngfuncs.pTriAPI->RenderMode( kRenderTransTexture );
	FillRGBABlend( x, y, wide, tall, 0, 0, 0, 255 * 0.6 );
	FillRGBA( x + 1, y, wide - 1, 1, 255, 140, 0, 255 );
	FillRGBA( x, y, 1, tall - 1, 255, 140, 0, 255 );
	FillRGBA( x + wide - 1, y + 1, 1, tall - 1, 255, 140, 0, 255 );
	FillRGBA( x, y + tall - 1, wide - 1, 1, 255, 140, 0, 255 );
}

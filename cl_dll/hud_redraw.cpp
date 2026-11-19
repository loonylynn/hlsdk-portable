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
		FillRGBA( x0, y0, 2, 2, 0, 0, 0, alpha / 2 );
		FillRGBA( x0, y0, 1, 1, r, g, b, alpha );
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

// Draw a HUD locator sprite at a requested size. Xash3D's generic sprite
// entry point stretches the source texture; texture-alpha blending allows the
// renderer to interpolate edge coverage when linear texture filtering is active.
static bool HUD_DrawInstructorLocatorSprite( HSPRITE sprite, int x, int y,
	int nativeWidth, int nativeHeight, int width, int height,
	int renderMode, bool additiveFallback, bool flipX = false, bool flipY = false )
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
		gEngfuncs.pfnSPR_DrawGeneric( 0, drawX, drawY,
			NULL, 0, 0, drawWidth, drawHeight );
		gEngfuncs.pTriAPI->RenderMode( kRenderNormal );
		return true;
	}

	// Older engines cannot stretch or mirror through the generic sprite API.
	// Keep the native-size fallback for ordinary sprites; mirrored directions
	// are skipped so that a backwards-facing arrow is never shown by mistake.
	if( flipX || flipY )
		return false;

	x += ( width - nativeWidth ) / 2;
	y += ( height - nativeHeight ) / 2;
	if( additiveFallback )
		SPR_DrawAdditive( 0, x, y, NULL );
	else
		SPR_DrawHoles( 0, x, y, NULL );
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
// It currently returns one matching key; literal lesson labels are preserved.
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

static void HUD_DrawInstructorBindingName( int centerX, int centerY, const char *bindingName, int alpha )
{
	if( !bindingName || !bindingName[0] ) return;
	const int width = gHUD.DrawHudStringLen( bindingName );
	const int height = gHUD.m_scrinfo.iCharHeight > 0 ? gHUD.m_scrinfo.iCharHeight :
		( gHUD.m_iFontHeight > 0 ? gHUD.m_iFontHeight : 12 );
	const int x = centerX - width / 2 - 1;
	const int y = centerY - height / 2 - 1;
	int ink = ( 255 - alpha ) * 3 / 4;
	if( ink > 190 ) ink = 190;
	gHUD.DrawHudString( x, y, centerX + width / 2 + 4, bindingName, ink, ink, ink );
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
			target->SetVisible( targetAllowed &&
				( !target->m_bOccluded || forceCaption ), flTime, 0.18f );
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
			const int captionWidth = DrawHudStringLen( target->m_szCaption );
			const int bindingWidth = DrawHudStringLen( resolvedBinding );
			const bool managedCard = target->m_bManaged;
			const bool genericKey = m_hInstructorHintKeyGenericSprite &&
				resolvedBinding[0] && resolvedBinding[1] && !resolvedBinding[2] ? true :
				( m_hInstructorHintKeyGenericSprite && resolvedBinding[0] && !resolvedBinding[1] );
			int keyChipWidth = 0;
			if( managedCard && resolvedBinding[0] )
			{
				keyChipWidth = bindingWidth + ( genericKey ? 12 : 18 );
				if( genericKey && keyChipWidth < 24 ) keyChipWidth = 24;
				if( keyChipWidth > 64 ) keyChipWidth = 64;
			}

			HSPRITE icon = (HSPRITE)target->m_hOnscreenSprite;
			if( target->m_iHintId == -1 )
				icon = target->m_iIconMode == 2 ? m_hInstructorHintHoldSprite :
					( target->m_iIconMode == 1 ? m_hInstructorHintMoveSprite : 0 );
			int iconWidth = icon ? SPR_Width( icon, 0 ) : 0;
			int iconHeight = icon ? SPR_Height( icon, 0 ) : 0;
			if( iconWidth <= 0 || iconHeight <= 0 || iconWidth > 64 || iconHeight > 64 )
				iconWidth = iconHeight = 0;

			const int textWidth = managedCard ?
				captionWidth + ( keyChipWidth ? keyChipWidth + 12 : 0 ) :
				Q_max( captionWidth, bindingWidth );
			int panelWidth = textWidth + ( iconWidth ? iconWidth + 12 : 0 ) +
				( managedCard ? 44 : 36 );
			const int maxPanelWidth = Q_max( 1, ScreenWidth - 24 );
			if( panelWidth > maxPanelWidth ) panelWidth = maxPanelWidth;
			const int minPanelWidth = managedCard ? 224 : 180;
			if( panelWidth < minPanelWidth && maxPanelWidth >= minPanelWidth )
				panelWidth = minPanelWidth;

			const int basePanelHeight = managedCard ? 62 : ( resolvedBinding[0] ? 58 : 42 );
			const int panelHeight = Q_max( basePanelHeight,
				iconHeight + ( managedCard ? 24 : 16 ) );
			int panelX = ( ScreenWidth - panelWidth ) / 2;
			int panelY = ScreenHeight - panelHeight - 30;
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
		target->m_wide = 40;
		target->m_tall = 40;
		if( HUD_GetInstructorHintScreenPosition( worldOrigin, &screenX, &screenY, &behind ) )
		{
			target->UpdateProjection( screenX, screenY, behind,
				ScreenWidth, ScreenHeight, 24 );
			if( !target->m_bOnscreen && ( target->m_iEffectsFlags & LOCATOR_ICON_FX_NO_OFFSCREEN ) )
				continue;
			// Use the same target-specific safe edge margin during layout and
			// final drawing. ResolveDecluttering then offsets from this exact
			// base position instead of a subtly different second projection.
			const bool targetOffscreen = !target->m_bOnscreen;
			const char *requestedIcon = targetOffscreen ?
				target->m_szOffscreenIcon : target->m_szOnscreenIcon;
			const bool sourceBindingIcon = requestedIcon &&
				stricmp( requestedIcon, "use_binding" ) == 0;
			const bool hasDirectionalLocatorSprites =
				m_hInstructorHintArrowRightSprite && m_hInstructorHintArrowUpSprite;
			const int sourceBindingIconHeight = Q_max( 1, (int)( ScreenWidth * 0.04f ) );
			target->m_iEdgeMargin = hasDirectionalLocatorSprites ?
				( sourceBindingIcon ?
					sourceBindingIconHeight / 2 + 3 + 24 + 4 : 22 + 24 + 4 ) : 22;
			target->CalculateEdgePosition( ScreenWidth, ScreenHeight, target->m_iEdgeMargin );

			HSPRITE icon = target->m_bOnscreen ?
				(HSPRITE)target->m_hOnscreenSprite : (HSPRITE)target->m_hOffscreenSprite;
			if( target->m_iHintId == -1 )
				icon = target->m_iIconMode == 2 ? m_hInstructorHintHoldSprite :
					( target->m_iIconMode == 1 ? m_hInstructorHintMoveSprite : 0 );
			if( icon )
			{
				const int iconWidth = SPR_Width( icon, 0 );
				const int iconHeight = SPR_Height( icon, 0 );
				if( iconWidth > 0 && iconHeight > 0 && iconWidth <= 64 && iconHeight <= 64 )
				{
					target->m_wide = (int)( iconWidth * 1.2f + 0.5f );
					target->m_tall = (int)( iconHeight * 1.2f + 0.5f );
				}
			}
			if( ( stricmp( target->m_szOnscreenIcon, "use_binding" ) == 0 ) ||
				( stricmp( target->m_szOffscreenIcon, "use_binding" ) == 0 ) )
				target->m_wide = target->m_tall = Q_max( target->m_wide, (int)( ScreenWidth * 0.04f ) );
			// Source's dynamic locator only reserves caption width while the
			// target is on-screen and within ICON_DIST_TOO_FAR (720 units),
			// unless FORCE_CAPTION is set. Off-screen and distant markers use
			// the icon footprint only.
			const bool forceCaption = ( target->m_iEffectsFlags & LOCATOR_ICON_FX_FORCE_CAPTION ) != 0;
			if( target->m_bOnscreen &&
				( forceCaption || target->m_flDistFromPlayer <= INSTRUCTOR_LOCATOR_CAPTION_MAX_DISTANCE ) )
			{
				const int captionFootprint = DrawHudStringLen( target->m_szCaption ) + 24;
				if( target->m_wide < captionFootprint )
					target->m_wide = Q_min( Q_max( 40, captionFootprint ), Q_max( 40, ScreenWidth / 2 ) );
			}
			if( target->m_tall < 48 )
				target->m_tall = 48;
		}
	}
	m_InstructorLocatorPanel.ResolveDecluttering( ScreenWidth, ScreenHeight, 56, 8 );

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

			int captionWidth = DrawHudStringLen( locatorTarget->m_szCaption );
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
			const int edgeMargin = 24;
			if( hasTarget )
				locatorTarget->UpdateProjection( targetX, targetY, targetBehind,
					ScreenWidth, ScreenHeight, edgeMargin );
			const bool targetOffscreen = hasTarget && !locatorTarget->m_bOnscreen;

			// "use_binding" is a virtual icon: the resolved key art becomes the locator.
			char instructorBindingGlyph[32];
			instructorBindingGlyph[0] = '\0';
			HSPRITE instructorBindingSprite = 0;
			const char *requestedBindingIcon = targetOffscreen ?
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
				sourceBindingIconHeight = Q_max( 1, (int)( ScreenWidth * 0.04f ) );
				sourceBindingIconWidth = sourceBindingIconHeight;
			}

			// Source counts a lesson only after its icon has remained on-screen
			// for at least 1.5 seconds, rather than when it is merely opened.
			if( locatorTarget->m_bManaged && !locatorTarget->m_bManagedDisplayCounted )
			{
				const bool countsAsVisible = locatorTarget->m_bOriginInScreenspace ||
					( hasTarget && !targetOffscreen );
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
					const int locatorArrowDrawSize = 24;
					const int locatorArrowGap = 4;
					const int locatorEdgeMargin = locatorTarget->m_iEdgeMargin > 0 ?
						locatorTarget->m_iEdgeMargin : 22;

					// Projection and edge placement belong to the target model.
					// The renderer adds only transient pulse/shake offsets.
					locatorTarget->CalculateEdgePosition( ScreenWidth, ScreenHeight, locatorEdgeMargin );
					float locatorX = (float)( locatorTarget->m_iconX + locatorTarget->m_offsetX ) +
						locatorTarget->m_flShakeX;
					float locatorY = (float)( locatorTarget->m_iconY + locatorTarget->m_offsetY ) +
						locatorTarget->m_flShakeY;

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
					if( !targetOffscreen && hasEntityAnchor && !entityAnchorBehind &&
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
						( targetOffscreen ? (HSPRITE)locatorTarget->m_hOffscreenSprite :
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

					int locatorAlpha = locatorTarget->GetRenderAlpha( 220 );
					const int locatorTextR = locatorR * locatorAlpha / 255;
					const int locatorTextG = locatorG * locatorAlpha / 255;
					const int locatorTextB = locatorB * locatorAlpha / 255;

					// A 20% increase keeps the 32x32 art compact while making
					// the locator easier to pick out at normal gameplay distance.
					if( sourceBindingIcon && locatorIconHeight > 0 && locatorIconWidth > 0 )
						sourceBindingIconWidth = Q_max( 1, (int)( sourceBindingIconHeight *
							( (float)locatorIconWidth / (float)locatorIconHeight ) + 0.5f ) );
					const int locatorDrawWidth = sourceBindingIcon ?
						Q_max( 1, (int)( sourceBindingIconWidth * locatorTarget->m_flPulseScale + 0.5f ) ) :
						( locatorSprite ? (int)( locatorIconWidth * 1.20f * locatorTarget->m_flPulseScale + 0.5f ) : 0 );
					const int locatorDrawHeight = sourceBindingIcon ?
						Q_max( 1, (int)( sourceBindingIconHeight * locatorTarget->m_flPulseScale + 0.5f ) ) :
						( locatorSprite ? (int)( locatorIconHeight * 1.20f * locatorTarget->m_flPulseScale + 0.5f ) : 0 );
					const int markerHalfWidth = sourceBindingIcon ? locatorDrawWidth / 2 :
						( locatorSprite ? locatorDrawWidth / 2 : 10 );
					const int markerHalfHeight = sourceBindingIcon ? locatorDrawHeight / 2 :
						( locatorSprite ? locatorDrawHeight / 2 : 10 );

					if( locatorSprite )
					{
						// Keep the sprite's transparent palette pixels fully clear.
						// Do not draw a rectangular backing/halo behind the locator.

						const int spriteR = locatorR * locatorAlpha / 255;
						const int spriteG = locatorG * locatorAlpha / 255;
						const int spriteB = locatorB * locatorAlpha / 255;
						SPR_Set( locatorSprite, spriteR, spriteG, spriteB );
						if( locatorTarget->m_iHintId == -1 )
						{
							HUD_DrawInstructorLocatorSprite( locatorSprite,
								markerX - locatorDrawWidth / 2,
								markerY - locatorDrawHeight / 2,
								locatorIconWidth, locatorIconHeight,
								locatorDrawWidth, locatorDrawHeight,
								kRenderTransAdd, true );
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
								kRenderTransAlpha, false );
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
						FillRGBA( markerX - 10, markerY - 10, 20, 20, 5, 7, 10, locatorAlpha );
						if( !hasDirectionalLocatorSprites )
							DrawHudString( markerX - 4, markerY - 8, markerX + 10,
								arrow, locatorTextR, locatorTextG, locatorTextB );
					}
					else if( !sourceBindingIcon )
					{
						// Sprite-free on-screen fallback: a small reticle, not
						// the PS2 interaction brackets and not a tutorial card.
						FillRGBA( markerX - 9, markerY - 9, 18, 18, 5, 7, 10, locatorAlpha / 2 );
						FillRGBA( markerX - 8, markerY - 8, 16, 1, locatorR, locatorG, locatorB, locatorAlpha );
						FillRGBA( markerX - 8, markerY + 7, 16, 1, locatorR, locatorG, locatorB, locatorAlpha );
						FillRGBA( markerX - 8, markerY - 7, 1, 14, locatorR, locatorG, locatorB, locatorAlpha );
						FillRGBA( markerX + 7, markerY - 7, 1, 14, locatorR, locatorG, locatorB, locatorAlpha );
						FillRGBA( markerX - 2, markerY - 2, 5, 5, locatorR, locatorG, locatorB, locatorAlpha );
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
						SPR_Set( arrowSprite,
							locatorR * locatorAlpha / 255,
							locatorG * locatorAlpha / 255,
							locatorB * locatorAlpha / 255 );
						const bool arrowDrawn = HUD_DrawInstructorLocatorSprite(
							arrowSprite, arrowX, arrowY,
							arrowNativeWidth, arrowNativeHeight,
							arrowWidth, arrowHeight, kRenderTransAlpha, false,
							flipArrowX, flipArrowY );
						if( !arrowDrawn )
						{
							const char *arrow = locatorTarget->m_drawArrowDirection == INSTRUCTOR_LOCATOR_ARROW_LEFT ? "<" :
								( locatorTarget->m_drawArrowDirection == INSTRUCTOR_LOCATOR_ARROW_RIGHT ? ">" :
								( locatorTarget->m_drawArrowDirection == INSTRUCTOR_LOCATOR_ARROW_UP ? "^" : "v" ) );
							DrawHudString( markerX - 4, markerY - 8, markerX + 10,
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

					const int locatorCaptionWidth = DrawHudStringLen( locatorTarget->m_szCaption );
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
					const bool captionWithinSourceRange = forceCaption ||
						locatorTarget->m_flDistFromPlayer <= INSTRUCTOR_LOCATOR_CAPTION_MAX_DISTANCE;
					if( hasLocatorText && !sourceBindingIcon && !targetOffscreen &&
						captionWithinSourceRange )
					{
						int locatorKeyWidth = 0;
						if( locatorBindingWidth > 0 )
						{
							locatorKeyWidth = locatorBindingWidth +
								( locatorUsesGenericKey ? 12 : 10 );
							if( locatorUsesGenericKey && locatorKeyWidth < 22 )
								locatorKeyWidth = 22;
							if( locatorKeyWidth > 64 )
								locatorKeyWidth = 64;
						}
						const int maxLabelWidth = Q_max( 32, ScreenWidth - 24 );
						int labelWidth = locatorCaptionWidth + locatorKeyWidth + 24;
						if( labelWidth < 44 ) labelWidth = 44;
						if( labelWidth > maxLabelWidth ) labelWidth = maxLabelWidth;
						const int labelHeight = 20;
						int labelX = markerX - labelWidth / 2;
						int labelY = markerY + markerHalfHeight + 7;

						if( targetOffscreen )
						{
							if( markerX <= locatorEdgeMargin + 2 )
							{
								labelX = markerX + markerHalfWidth + 6;
								labelY = markerY - labelHeight / 2;
							}
							else if( markerX >= ScreenWidth - locatorEdgeMargin - 2 )
							{
								labelX = markerX - markerHalfWidth - labelWidth - 6;
								labelY = markerY - labelHeight / 2;
							}
							else if( markerY <= locatorEdgeMargin + 2 )
							{
								labelY = markerY + markerHalfHeight + 6;
							}
							else
							{
								labelY = markerY - markerHalfHeight - labelHeight - 6;
							}
						}
						else if( labelY + labelHeight > ScreenHeight - 8 )
						{
							labelY = markerY - markerHalfHeight - labelHeight - 7;
						}

						const int minLabelX = 8;
						const int minLabelY = 8;
						const int maxLabelX = Q_max( minLabelX, ScreenWidth - labelWidth - 8 );
						const int maxLabelY = Q_max( minLabelY, ScreenHeight - labelHeight - 8 );
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
						FillRGBA( labelX, labelY, labelWidth, 1, locatorR, locatorG, locatorB, locatorAlpha * 3 / 4 );
						FillRGBA( labelX, labelY + labelHeight - 1, labelWidth, 1, 0, 0, 0, locatorAlpha );

						const int labelTextY = labelY + 5;
						const int labelTextX = labelX + 8;
						const int keyX = labelX + labelWidth - locatorKeyWidth - 5;
						const int captionRight = locatorKeyWidth > 0 ? keyX - 6 : labelX + labelWidth - 6;
						if( locatorCaptionWidth > 0 )
							DrawHudString( labelTextX, labelTextY, captionRight,
								locatorTarget->m_szCaption, locatorTextR, locatorTextG, locatorTextB );

						if( locatorKeyWidth > 0 )
						{
							const int keyY = labelY + 2;
							const int keyHeight = 16;
						bool keySpriteDrawn = false;
						if( locatorUsesGenericKey )
						{
							const int keyNativeWidth = SPR_Width( m_hInstructorHintKeyGenericSprite, 0 );
						const int keyNativeHeight = SPR_Height( m_hInstructorHintKeyGenericSprite, 0 );
						if( keyNativeWidth > 0 && keyNativeHeight > 0 &&
							keyNativeWidth <= 64 && keyNativeHeight <= 64 )
						{
							SPR_Set( m_hInstructorHintKeyGenericSprite,
								locatorR * locatorAlpha / 255,
								locatorG * locatorAlpha / 255,
								locatorB * locatorAlpha / 255 );
							keySpriteDrawn = HUD_DrawInstructorLocatorSprite(
								m_hInstructorHintKeyGenericSprite, keyX, keyY,
								keyNativeWidth, keyNativeHeight,
								locatorKeyWidth, keyHeight, kRenderTransAlpha, false );
						}
						}
						if( !keySpriteDrawn )
						{
							// Keep the old drawn key chip if the optional .spr is absent.
							FillRGBA( keyX, keyY, locatorKeyWidth, keyHeight, 12, 15, 19, locatorAlpha );
							FillRGBA( keyX, keyY, locatorKeyWidth, 1, locatorR, locatorG, locatorB, locatorAlpha / 2 );
						}
						int keyTextX = keyX + ( locatorKeyWidth - locatorBindingWidth ) / 2;
					if( keyTextX < keyX + 3 )
						keyTextX = keyX + 3;
					// The supplied keycap sprites have a light face. Keep the
					// resolved key label black on that face, not locator-white.
					int keyGlyphInk = keySpriteDrawn ?
						( 16 + ( 255 - locatorAlpha ) * 3 / 4 ) : locatorTextR;
					if( keyGlyphInk > 190 ) keyGlyphInk = 190;
					DrawHudString( keyTextX, keyY + 4, keyX + locatorKeyWidth - 3,
						locatorBinding, keyGlyphInk, keyGlyphInk, keyGlyphInk );
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
						const int sourceCaptionWidth = DrawHudStringLen( locatorTarget->m_szCaption );
						if( !targetOffscreen && captionWithinSourceRange && sourceCaptionWidth > 0 )
						{
							const int fontHeight = m_scrinfo.iCharHeight > 0 ? m_scrinfo.iCharHeight :
								( m_iFontHeight > 0 ? m_iFontHeight : 12 );
							int captionX = markerX + markerHalfWidth + 5;
							if( captionX + sourceCaptionWidth > ScreenWidth - 8 )
								captionX = markerX - markerHalfWidth - 5 - sourceCaptionWidth;
							if( captionX < 8 ) captionX = 8;
							if( captionX + sourceCaptionWidth > ScreenWidth - 8 )
								captionX = Q_max( 8, ScreenWidth - sourceCaptionWidth - 8 );
							DrawHudString( captionX, markerY - fontHeight / 2 - 1,
								Q_min( ScreenWidth - 8, captionX + sourceCaptionWidth + 4 ),
								locatorTarget->m_szCaption, locatorTextR, locatorTextG, locatorTextB );
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

			int iconWidth = hintIcon ? SPR_Width( hintIcon, 0 ) : 0;
			int iconHeight = hintIcon ? SPR_Height( hintIcon, 0 ) : 0;
			if( iconWidth <= 0 || iconHeight <= 0 || iconWidth > 64 || iconHeight > 64 )
			{
				hintIcon = 0;
				iconWidth = 0;
				iconHeight = 0;
			}

			const bool managedCard = locatorTarget->m_bManaged;
			const bool cardUsesGenericKey = m_hInstructorHintKeyGenericSprite &&
				bindingText[0] && bindingText[1] && !bindingText[2] ? true :
				( m_hInstructorHintKeyGenericSprite && bindingText[0] && !bindingText[1] );
			int keyChipWidth = 0;
			if( managedCard && bindingText[0] )
			{
				keyChipWidth = bindingWidth +
					( cardUsesGenericKey ? 12 : 18 );
				if( cardUsesGenericKey && keyChipWidth < 24 )
					keyChipWidth = 24;
				if( keyChipWidth > 64 )
					keyChipWidth = 64;
			}
			const int contentTextWidth = managedCard ?
				captionWidth + ( keyChipWidth ? keyChipWidth + 12 : 0 ) :
				Q_max( captionWidth, bindingWidth );
			const int contentWidth = contentTextWidth + ( hintIcon ? iconWidth + 12 : 0 );
			int panelWidth = contentWidth + ( managedCard ? 44 : 36 );
			const int maxPanelWidth = Q_max( 1, ScreenWidth - 24 );
			if( panelWidth > maxPanelWidth )
				panelWidth = maxPanelWidth;
			const int minPanelWidth = managedCard ? 224 : 180;
			if( panelWidth < minPanelWidth && maxPanelWidth >= minPanelWidth )
				panelWidth = minPanelWidth;

			const int basePanelHeight = managedCard ? 62 : ( bindingText[0] ? 58 : 42 );
			const int panelHeight = Q_max( basePanelHeight, iconHeight + ( managedCard ? 24 : 16 ) );
			int panelX = ( ScreenWidth - panelWidth ) / 2;
			int panelY = ScreenHeight - panelHeight - 30;

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
				const int edgeMargin = 24;
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

					FillRGBA( arrowX - 12, arrowY - 12, 24, 24, 8, 10, 14, 225 );
					DrawHudString( arrowX - 4, arrowY - 7, arrowX + 12,
						arrow, locatorTarget->m_iCaptionColorR, locatorTarget->m_iCaptionColorG, locatorTarget->m_iCaptionColorB );

					if( arrowX <= edgeMargin + 2 )
				{
					panelX = edgeMargin + 22;
					panelY = arrowY - panelHeight / 2;
				}
				else if( arrowX >= ScreenWidth - edgeMargin - 2 )
				{
					panelX = ScreenWidth - edgeMargin - panelWidth - 22;
					panelY = arrowY - panelHeight / 2;
				}
				else if( arrowY <= edgeMargin + 2 )
				{
					panelX = arrowX - panelWidth / 2;
					panelY = edgeMargin + 22;
				}
				else
				{
					panelX = arrowX - panelWidth / 2;
					panelY = ScreenHeight - edgeMargin - panelHeight - 22;
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
			if( locatorTarget->m_iEffectsFlags & 0x00000007 )
				panelAlpha = Q_min( 255, panelAlpha + (int)( 28.0f * locatorTarget->m_flPulse ) );
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

				FillRGBA( panelX + 2, panelY + 3, panelWidth, panelHeight, 0, 0, 0, panelAlpha / 2 );
				FillRGBA( panelX, panelY, panelWidth, panelHeight, 4, 6, 10, panelAlpha );
				FillRGBA( panelX + 1, panelY + 1, panelWidth - 2, panelHeight - 2, 19, 24, 33, panelAlpha );
				FillRGBA( panelX + 1, panelY + 1, panelWidth - 2, 1, 74, 86, 104, panelAlpha );
				FillRGBA( panelX + 1, panelY + panelHeight - 2, panelWidth - 2, 1, 5, 7, 11, panelAlpha );
				FillRGBA( panelX + 1, panelY + 1, 3, panelHeight - 2, hintR, hintG, hintB, panelAlpha );
				FillRGBA( panelX + 5, panelY + 5, 1, panelHeight - 10, hintR, hintG, hintB, panelAlpha / 3 );
			}
			else
			{
				// Preserve the established env_instructor_hint / pushable styling.
				FillRGBA( panelX, panelY, panelWidth, panelHeight, 8, 10, 14, panelAlpha );
				FillRGBA( panelX, panelY, 3, panelHeight, hintR, hintG, hintB, panelAlpha );
			}

			int textAreaX = panelX + ( managedCard ? 16 : 14 );
			if( hintIcon )
			{
				const int iconX = panelX + 12;
				const int iconY = panelY + ( panelHeight - iconHeight ) / 2;
				if( managedCard )
				{
					FillRGBA( iconX - 5, iconY - 5, iconWidth + 10, iconHeight + 10, 31, 39, 52, panelAlpha );
					FillRGBA( iconX - 5, iconY - 5, iconWidth + 10, 1, hintR, hintG, hintB, panelAlpha );
				}
				SPR_Set( hintIcon, hintTextR, hintTextG, hintTextB );
				SPR_DrawAdditive( 0, iconX, iconY, NULL );
				textAreaX = iconX + iconWidth + 12;
			}

			const int keyX = panelX + panelWidth - keyChipWidth - 12;
			const int textAreaRight = ( managedCard && keyChipWidth ) ?
				keyX - 10 : panelX + panelWidth - ( managedCard ? 14 : 10 );
			const int textAreaWidth = Q_max( 1, textAreaRight - textAreaX );

			if( managedCard )
			{
				DrawHudString( textAreaX, panelY + 9, textAreaRight,
					"INSTRUCTION", hintTextR, hintTextG, hintTextB );
				DrawHudString( textAreaX, panelY + 31, textAreaRight,
					locatorTarget->m_szCaption, whiteText, whiteText, whiteText );

				if( bindingText[0] )
				{
					const int keyY = panelY + 22;
					bool keySpriteDrawn = false;
					if( cardUsesGenericKey )
					{
						const int keyNativeWidth = SPR_Width( m_hInstructorHintKeyGenericSprite, 0 );
						const int keyNativeHeight = SPR_Height( m_hInstructorHintKeyGenericSprite, 0 );
						if( keyNativeWidth > 0 && keyNativeHeight > 0 &&
							keyNativeWidth <= 64 && keyNativeHeight <= 64 )
						{
							SPR_Set( m_hInstructorHintKeyGenericSprite,
								hintR * panelAlpha / 255,
								hintG * panelAlpha / 255,
								hintB * panelAlpha / 255 );
							keySpriteDrawn = HUD_DrawInstructorLocatorSprite(
								m_hInstructorHintKeyGenericSprite, keyX, keyY,
								keyNativeWidth, keyNativeHeight,
								keyChipWidth, 22, kRenderTransAlpha, false );
						}
					}
					if( !keySpriteDrawn )
					{
						FillRGBA( keyX, keyY, keyChipWidth, 22, 10, 14, 21, panelAlpha );
						FillRGBA( keyX, keyY, keyChipWidth, 1, hintR, hintG, hintB, panelAlpha );
						FillRGBA( keyX, keyY + 21, keyChipWidth, 1, 63, 73, 89, panelAlpha );
					}
					int keyTextX = keyX + ( keyChipWidth - bindingWidth ) / 2;
					if( keyTextX < keyX + 3 )
						keyTextX = keyX + 3;
					int keyGlyphInk = keySpriteDrawn ?
						( 16 + ( 255 - panelAlpha ) * 3 / 4 ) : hintTextR;
					if( keyGlyphInk > 190 ) keyGlyphInk = 190;
					DrawHudString( keyTextX, keyY + 6, keyX + keyChipWidth - 5,
						bindingText, keyGlyphInk, keyGlyphInk, keyGlyphInk );
				}
			}
			else
			{
				int textX = textAreaX + ( textAreaWidth - captionWidth ) / 2;
				if( textX < textAreaX )
					textX = textAreaX;
				const int textOffsetY = ( panelHeight - basePanelHeight ) / 2;
				DrawHudString( textX, panelY + textOffsetY + 9, textAreaRight,
					locatorTarget->m_szCaption, whiteText, whiteText, whiteText );

				if( bindingText[0] )
				{
					int keyTextX = textAreaX + ( textAreaWidth - bindingWidth ) / 2;
					if( keyTextX < textAreaX )
						keyTextX = textAreaX;
					DrawHudString( keyTextX, panelY + textOffsetY + 32, textAreaRight,
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

int CHud::GetNumWidth( int iNumber, int iFlags )
{
	if( iFlags & ( DHN_3DIGITS ) )
		return 3;

	if( iFlags & ( DHN_2DIGITS ) )
		return 2;

	if( iNumber <= 0 )
	{
		if( iFlags & ( DHN_DRAWZERO ) )
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

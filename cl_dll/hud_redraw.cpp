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
#include <math.h>
#include <stdio.h>

#include "hud.h"
#include "game_instructor.h"
#include "cl_util.h"
#include "cl_entity.h"
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
	GameInstructor_Update( flTime );

	// Resolve the active scripted locator before entering the HUD draw branch:
	// this lets range and NODRAW rules suppress the entire card, not only its icon.
	if( m_bInstructorHintActive && m_bInstructorHintManaged &&
		m_bInstructorHintHasTargetPosition && m_iInstructorHintTarget > 0 )
	{
		bool targetAllowed = true;
		cl_entity_t *managedTarget = gEngfuncs.GetEntityByIndex ?
			gEngfuncs.GetEntityByIndex( m_iInstructorHintTarget ) : NULL;
		if( !managedTarget )
		{
			targetAllowed = false;
		}
		else
		{
			// GoldSrc brush entities commonly keep their network origin at
			// (0,0,0). Match the server's VecBModelOrigin() calculation:
			// absmin + size * 0.5 == origin + (mins + maxs) * 0.5.
			// Non-brush entities retain their ordinary entity origin.
			m_vecInstructorHintTargetOrigin[0] = managedTarget->origin[0];
			m_vecInstructorHintTargetOrigin[1] = managedTarget->origin[1];
			m_vecInstructorHintTargetOrigin[2] = managedTarget->origin[2];
			if( managedTarget->curstate.solid == SOLID_BSP )
			{
				m_vecInstructorHintTargetOrigin[0] +=
					( managedTarget->curstate.mins[0] + managedTarget->curstate.maxs[0] ) * 0.5f;
				m_vecInstructorHintTargetOrigin[1] +=
					( managedTarget->curstate.mins[1] + managedTarget->curstate.maxs[1] ) * 0.5f;
				m_vecInstructorHintTargetOrigin[2] +=
					( managedTarget->curstate.mins[2] + managedTarget->curstate.maxs[2] ) * 0.5f;
			}

			if( ( managedTarget->curstate.effects & EF_NODRAW ) &&
				!( m_iInstructorHintFlags & 0x00010000 ) )
				targetAllowed = false;

			if( targetAllowed && m_flInstructorHintRange > 0.0f && gEngfuncs.GetLocalPlayer )
			{
				cl_entity_t *localPlayer = gEngfuncs.GetLocalPlayer();
				if( localPlayer )
				{
					const float dx = m_vecInstructorHintTargetOrigin[0] - localPlayer->origin[0];
					const float dy = m_vecInstructorHintTargetOrigin[1] - localPlayer->origin[1];
					const float dz = m_vecInstructorHintTargetOrigin[2] - localPlayer->origin[2];
					const float distance = sqrtf( dx * dx + dy * dy + dz * dz );
					if( distance > m_flInstructorHintRange )
						targetAllowed = false;
				}
			}
		}
		m_bInstructorHintTargetVisible = targetAllowed;
	}

	// Match Source's 0.25-second occlusion cadence. Only client-managed lessons
	// need this trace; env_instructor_hint already sends its server-side result.
	if( m_bInstructorHintActive && m_bInstructorHintManaged &&
		m_bInstructorHintHasTargetPosition && m_bInstructorHintTargetVisible &&
		m_iInstructorHintTarget > 0 && flTime >= m_flInstructorHintNextOcclusionTest )
	{
		m_bInstructorHintOccluded = HUD_IsInstructorHintOccluded(
			m_vecInstructorHintTargetOrigin, m_iInstructorHintTarget );
		m_flInstructorHintNextOcclusionTest = flTime + 0.25f;
	}

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

	// Source-style instructor hint presentation. The PS2 world-space bracket
	// remains unchanged; this panel follows a scripted target in screen space.
	if( m_bInstructorHintActive && m_bInstructorHintTargetVisible &&
		( !m_bInstructorHintOccluded || ( m_iInstructorHintFlags & 0x00000400 ) ) &&
		!intermission && m_pCvarDraw && m_pCvarDraw->value &&
		!( m_iHideHUDDisplay & HIDEHUD_ALL ) )
	{
		if( m_flInstructorHintEndTime > 0.0f && flTime >= m_flInstructorHintEndTime )
		{
			m_bInstructorHintActive = false;
			m_iInstructorHintId = 0;
			m_iInstructorHintTarget = 0;
			m_bInstructorHintHasTargetPosition = false;
			m_bInstructorHintTargetVisible = false;
			m_bInstructorHintOccluded = false;
			m_vecInstructorHintTargetOrigin[0] = 0.0f;
			m_vecInstructorHintTargetOrigin[1] = 0.0f;
			m_vecInstructorHintTargetOrigin[2] = 0.0f;
			m_flInstructorHintEndTime = 0.0f;
			m_flInstructorHintRange = 0.0f;
			m_flInstructorHintIconOffset = 0.0f;
			m_bInstructorHintManaged = false;
			m_flInstructorManagedVisibleSince = 0.0f;
			m_iInstructorHintIconMode = 0;
			m_iInstructorHintFlags = 0;
			m_hInstructorHintOnscreenSprite = 0;
			m_hInstructorHintOffscreenSprite = 0;
			m_szInstructorHintIconOnscreen[0] = '\0';
			m_szInstructorHintIconOffscreen[0] = '\0';
			m_szInstructorHintCaption[0] = '\0';
			m_szInstructorHintBinding[0] = '\0';
		}
		else
		{
			char bindingText[80];
			bindingText[0] = '\0';
			if( m_szInstructorHintBinding[0] )
				snprintf( bindingText, sizeof( bindingText ),
					m_bInstructorHintManaged ? "%s" : "[ %s ]", m_szInstructorHintBinding );

			int captionWidth = DrawHudStringLen( m_szInstructorHintCaption );
			int bindingWidth = DrawHudStringLen( bindingText );

			// Project the current world target before selecting its on/off-screen icon.
			float targetX = 0.0f, targetY = 0.0f;
			float entityAnchorX = 0.0f, entityAnchorY = 0.0f;
			bool targetBehind = false;
			bool entityAnchorBehind = false;
			float locatorWorldOrigin[3] =
			{
				m_vecInstructorHintTargetOrigin[0],
				m_vecInstructorHintTargetOrigin[1],
				m_vecInstructorHintTargetOrigin[2] + m_flInstructorHintIconOffset
			};
			const bool hasTarget = m_bInstructorHintHasTargetPosition &&
				HUD_GetInstructorHintScreenPosition( locatorWorldOrigin,
					&targetX, &targetY, &targetBehind );
			const bool hasEntityAnchor = m_bInstructorHintHasTargetPosition &&
				HUD_GetInstructorHintScreenPosition( m_vecInstructorHintTargetOrigin,
					&entityAnchorX, &entityAnchorY, &entityAnchorBehind );
			const int edgeMargin = 24;
			const bool targetOffscreen = hasTarget && ( targetBehind ||
				targetX < edgeMargin || targetX > ScreenWidth - edgeMargin ||
				targetY < edgeMargin || targetY > ScreenHeight - edgeMargin );

			// Source counts a lesson only after its icon has remained on-screen
			// for at least 1.5 seconds, rather than when it is merely opened.
			if( m_bInstructorHintManaged && !m_bInstructorManagedDisplayCounted )
			{
				const bool countsAsVisible = !m_bInstructorHintHasTargetPosition || ( hasTarget && !targetOffscreen );
				if( countsAsVisible )
				{
					if( m_flInstructorManagedVisibleSince <= 0.0f )
						m_flInstructorManagedVisibleSince = flTime;
					else if( flTime - m_flInstructorManagedVisibleSince >= 1.5f )
					{
						GameInstructor_MarkDisplayed( m_szInstructorHintName );
						m_bInstructorManagedDisplayCounted = true;
					}
				}
				else
				{
					m_flInstructorManagedVisibleSince = 0.0f;
				}
			}


			// A target-backed hint is a world-space locator, not a tutorial card.
			// Keep its icon attached to the projected target and place only a compact
			// caption tag beside it. Fixed/no-target lessons retain the HUD card path.
			const bool worldLocator = hasTarget && !m_bInstructorHintFixedPosition;
			if( worldLocator )
			{
				if( !( targetOffscreen && ( m_iInstructorHintFlags & 0x00000200 ) ) )
				{
					float centerX = ScreenWidth * 0.5f;
					float centerY = ScreenHeight * 0.5f;
					float locatorX = targetX;
					float locatorY = targetY;
					const bool hasDirectionalLocatorSprites =
						m_hInstructorHintArrowRightSprite && m_hInstructorHintArrowUpSprite;
					const int locatorArrowDrawSize = 24;
					const int locatorArrowGap = 4;
					const int locatorEdgeMargin = hasDirectionalLocatorSprites ?
						( 22 + locatorArrowDrawSize + locatorArrowGap ) : 22;
					enum
					{
						INSTRUCTOR_ARROW_NONE,
						INSTRUCTOR_ARROW_LEFT,
						INSTRUCTOR_ARROW_RIGHT,
						INSTRUCTOR_ARROW_UP,
						INSTRUCTOR_ARROW_DOWN
					};
					int locatorArrowDirection = INSTRUCTOR_ARROW_NONE;

					if( targetOffscreen )
					{
						float dx = targetX - centerX;
						float dy = targetY - centerY;
						if( fabsf( dx ) < 1.0f && fabsf( dy ) < 1.0f )
							dy = 1.0f;

						const float left = (float)locatorEdgeMargin;
						const float right = (float)( ScreenWidth - locatorEdgeMargin );
						const float top = (float)locatorEdgeMargin;
						const float bottom = (float)( ScreenHeight - locatorEdgeMargin );
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

						locatorX = centerX + dx * t;
						locatorY = centerY + dy * t;
					}

					// The Source locator's pulse/shake options affect the marker,
					// not a full-screen panel.
					const int pulseFlags = m_iInstructorHintFlags & 0x00000007;
					const float pulseRate = ( pulseFlags & 0x4 ) ? 5.0f :
						( ( pulseFlags & 0x2 ) ? 2.5f : 1.0f );
					const float pulse = pulseFlags ?
						( 0.5f + 0.5f * sinf( flTime * pulseRate * 6.28318f ) ) : 0.0f;

					const int shakeFlags = m_iInstructorHintFlags & 0x000000C0;
					if( shakeFlags )
					{
						const float shakeRate = ( shakeFlags & 0x80 ) ? 22.0f : 10.0f;
						locatorX += sinf( flTime * shakeRate ) * 2.0f;
						locatorY += cosf( flTime * shakeRate * 0.83f ) * 1.5f;
					}

					if( locatorX < locatorEdgeMargin ) locatorX = (float)locatorEdgeMargin;
					if( locatorX > ScreenWidth - locatorEdgeMargin ) locatorX = (float)( ScreenWidth - locatorEdgeMargin );
					if( locatorY < locatorEdgeMargin ) locatorY = (float)locatorEdgeMargin;
					if( locatorY > ScreenHeight - locatorEdgeMargin ) locatorY = (float)( ScreenHeight - locatorEdgeMargin );

					if( targetOffscreen && hasDirectionalLocatorSprites )
					{
						if( locatorX <= locatorEdgeMargin + 0.5f )
							locatorArrowDirection = INSTRUCTOR_ARROW_LEFT;
						else if( locatorX >= ScreenWidth - locatorEdgeMargin - 0.5f )
							locatorArrowDirection = INSTRUCTOR_ARROW_RIGHT;
						else if( locatorY <= locatorEdgeMargin + 0.5f )
							locatorArrowDirection = INSTRUCTOR_ARROW_UP;
						else
							locatorArrowDirection = INSTRUCTOR_ARROW_DOWN;
					}

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
								markerX, markerY, m_iInstructorHintColorR,
								m_iInstructorHintColorG, m_iInstructorHintColorB, 150 );
					}

					HSPRITE locatorSprite = targetOffscreen ?
						m_hInstructorHintOffscreenSprite : m_hInstructorHintOnscreenSprite;
					if( m_iInstructorHintId == -1 )
					{
						locatorSprite = 0;
						if( m_iInstructorHintIconMode == 2 )
							locatorSprite = m_hInstructorHintHoldSprite;
						else if( m_iInstructorHintIconMode == 1 )
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

					int locatorR = m_iInstructorHintColorR;
					int locatorG = m_iInstructorHintColorG;
					int locatorB = m_iInstructorHintColorB;

					int locatorAlpha = 220;
					const int alphaFlags = m_iInstructorHintFlags & 0x00000038;
					if( alphaFlags )
					{
						const float alphaRate = ( alphaFlags & 0x20 ) ? 5.0f :
							( ( alphaFlags & 0x10 ) ? 2.5f : 1.0f );
						locatorAlpha = 150 + (int)( 105.0f *
							( 0.5f + 0.5f * sinf( flTime * alphaRate * 6.28318f ) ) );
					}
					if( m_bInstructorHintManaged && m_flInstructorManagedVisibleSince > 0.0f )
					{
						float fade = ( flTime - m_flInstructorManagedVisibleSince ) / 0.18f;
						if( fade < 0.0f ) fade = 0.0f;
						if( fade > 1.0f ) fade = 1.0f;
						locatorAlpha = (int)( locatorAlpha * ( 0.25f + 0.75f * fade ) );
					}

					// A 20% increase keeps the 32x32 art compact while making
					// the locator easier to pick out at normal gameplay distance.
					const int locatorDrawWidth = locatorSprite ?
						(int)( locatorIconWidth * 1.20f + 0.5f ) : 0;
					const int locatorDrawHeight = locatorSprite ?
						(int)( locatorIconHeight * 1.20f + 0.5f ) : 0;
					const int markerHalfWidth = locatorSprite ? locatorDrawWidth / 2 : 10;
					const int markerHalfHeight = locatorSprite ? locatorDrawHeight / 2 : 10;

					if( locatorSprite )
					{
						// Keep the sprite's transparent palette pixels fully clear.
						// Do not draw a rectangular backing/halo behind the locator.

						const int spriteR = locatorR * locatorAlpha / 255;
						const int spriteG = locatorG * locatorAlpha / 255;
						const int spriteB = locatorB * locatorAlpha / 255;
						SPR_Set( locatorSprite, spriteR, spriteG, spriteB );
						if( m_iInstructorHintId == -1 )
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
					else if( targetOffscreen )
					{
						// If the requested Source icon has no GoldSrc .spr
						// counterpart, fall back to a direction marker.
						const float dx = targetX - centerX;
						const float dy = targetY - centerY;
						const char *arrow = fabsf( dx ) > fabsf( dy ) ?
							( dx < 0.0f ? "<" : ">" ) :
							( dy < 0.0f ? "^" : "v" );
						FillRGBA( markerX - 10, markerY - 10, 20, 20, 5, 7, 10, locatorAlpha );
						if( !hasDirectionalLocatorSprites )
							DrawHudString( markerX - 4, markerY - 8, markerX + 10,
								arrow, locatorR, locatorG, locatorB );
					}
					else
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
					if( targetOffscreen && locatorArrowDirection != INSTRUCTOR_ARROW_NONE )
					{
						HSPRITE arrowSprite = 0;
						bool flipArrowX = false;
						bool flipArrowY = false;
						int arrowX = markerX;
						int arrowY = markerY;
						const int arrowWidth = locatorArrowDrawSize;
					const int arrowHeight = locatorArrowDrawSize;

						switch( locatorArrowDirection )
						{
						case INSTRUCTOR_ARROW_LEFT:
							arrowSprite = m_hInstructorHintArrowRightSprite;
							flipArrowX = true;
							arrowX = markerX - markerHalfWidth - locatorArrowGap - arrowWidth;
							arrowY = markerY - arrowHeight / 2;
							break;
						case INSTRUCTOR_ARROW_RIGHT:
							arrowSprite = m_hInstructorHintArrowRightSprite;
							arrowX = markerX + markerHalfWidth + locatorArrowGap;
							arrowY = markerY - arrowHeight / 2;
							break;
						case INSTRUCTOR_ARROW_UP:
							arrowSprite = m_hInstructorHintArrowUpSprite;
							arrowX = markerX - arrowWidth / 2;
							arrowY = markerY - markerHalfHeight - locatorArrowGap - arrowHeight;
							break;
						case INSTRUCTOR_ARROW_DOWN:
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
							const char *arrow = locatorArrowDirection == INSTRUCTOR_ARROW_LEFT ? "<" :
								( locatorArrowDirection == INSTRUCTOR_ARROW_RIGHT ? ">" :
								( locatorArrowDirection == INSTRUCTOR_ARROW_UP ? "^" : "v" ) );
							DrawHudString( markerX - 4, markerY - 8, markerX + 10,
								arrow, locatorR, locatorG, locatorB );
						}
						}
					}

					// The caption follows the icon as a compact locator label.
					// Binding text is shown in a small key chip on the same line.
					char locatorBinding[80];
					locatorBinding[0] = '\0';
					if( m_szInstructorHintBinding[0] )
						snprintf( locatorBinding, sizeof( locatorBinding ), "%s", m_szInstructorHintBinding );

					const int locatorCaptionWidth = DrawHudStringLen( m_szInstructorHintCaption );
					const int locatorBindingWidth = DrawHudStringLen( locatorBinding );
					const bool locatorUsesGenericKey = m_hInstructorHintKeyGenericSprite &&
						locatorBinding[0] && locatorBinding[1] && !locatorBinding[2] ? true :
						( m_hInstructorHintKeyGenericSprite && locatorBinding[0] && !locatorBinding[1] );
					const bool hasLocatorText = locatorCaptionWidth > 0 || locatorBindingWidth > 0;

					if( hasLocatorText )
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
								m_szInstructorHintCaption, locatorR, locatorG, locatorB );

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
					DrawHudString( keyTextX, keyY + 4, keyX + locatorKeyWidth - 3,
						locatorBinding, locatorR, locatorG, locatorB );
						}
					}
				}
			}
			else
			{
			// LOCATOR_ICON_FX_NO_OFFSCREEN deactivates the locator when it leaves
			// the viewport; don't leave a floating caption behind.
			if( !( targetOffscreen && ( m_iInstructorHintFlags & 0x00000200 ) ) )
			{
			// Source has distinct icon_onscreen and icon_offscreen resources.
			// Automatic pushable guidance retains its own move/hold fallback sprites.
			HSPRITE hintIcon = targetOffscreen ?
				m_hInstructorHintOffscreenSprite : m_hInstructorHintOnscreenSprite;
			if( m_iInstructorHintId == -1 )
			{
				hintIcon = 0;
				if( m_iInstructorHintIconMode == 2 )
					hintIcon = m_hInstructorHintHoldSprite;
				else if( m_iInstructorHintIconMode == 1 )
					hintIcon = m_hInstructorHintMoveSprite;
			}
			if( targetOffscreen && ( m_iInstructorHintFlags & 0x00000200 ) )
				hintIcon = 0;

			int iconWidth = hintIcon ? SPR_Width( hintIcon, 0 ) : 0;
			int iconHeight = hintIcon ? SPR_Height( hintIcon, 0 ) : 0;
			if( iconWidth <= 0 || iconHeight <= 0 || iconWidth > 64 || iconHeight > 64 )
			{
				hintIcon = 0;
				iconWidth = 0;
				iconHeight = 0;
			}

			const bool managedCard = m_bInstructorHintManaged;
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

			if( m_bInstructorHintFixedPosition )
			{
				panelX = (int)( m_flInstructorHintFixedX * ScreenWidth - panelWidth * 0.5f );
				panelY = (int)( m_flInstructorHintFixedY * ScreenHeight - panelHeight * 0.5f );
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
						arrow, m_iInstructorHintColorR, m_iInstructorHintColorG, m_iInstructorHintColorB );

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

			// Clamp the card after choosing its target-relative or edge-relative
			// placement. Keep a small inset even with unusual screen sizes.
			const int maxX = Q_max( 0, ScreenWidth - panelWidth );
			const int maxY = Q_max( 0, ScreenHeight - panelHeight );
			if( panelX < 12 ) panelX = 12;
			if( panelX > maxX - 12 ) panelX = Q_max( 0, maxX - 12 );
			if( panelY < 12 ) panelY = 12;
			if( panelY > maxY - 12 ) panelY = Q_max( 0, maxY - 12 );

			// Approximate Source locator FX flags using the GoldSrc HUD primitives.
			int panelAlpha = 205;
			const int alphaFlags = m_iInstructorHintFlags & 0x00000038;
			if( alphaFlags )
			{
				const float alphaRate = ( alphaFlags & 0x20 ) ? 5.0f :
					( ( alphaFlags & 0x10 ) ? 2.5f : 1.0f );
				panelAlpha = 150 + (int)( 105.0f * ( 0.5f + 0.5f * sinf( flTime * alphaRate * 6.28318f ) ) );
			}
			if( m_iInstructorHintFlags & 0x00000007 )
			{
				const float pulseRate = ( m_iInstructorHintFlags & 0x4 ) ? 5.0f :
					( ( m_iInstructorHintFlags & 0x2 ) ? 2.5f : 1.0f );
				panelAlpha = Q_min( 255, panelAlpha + (int)( 28.0f * ( 0.5f + 0.5f * sinf( flTime * pulseRate * 6.28318f ) ) ) );
			}
			if( m_iInstructorHintFlags & 0x000000C0 )
				panelX += (int)( sinf( flTime * ( ( m_iInstructorHintFlags & 0x80 ) ? 22.0f : 10.0f ) ) * 3.0f );

			int hintR = m_iInstructorHintColorR;
			int hintG = m_iInstructorHintColorG;
			int hintB = m_iInstructorHintColorB;

			if( managedCard )
			{
				// A Source-inspired layered card: subtle drop shadow, inset face,
				// crisp edge and a restrained locator-color accent.
				if( hintR == 255 && hintG == 255 && hintB == 255 )
				{
					hintR = 112;
					hintG = 180;
					hintB = 255;
				}

				// Fade the managed card in without changing the map-authored hint path.
				if( m_flInstructorManagedVisibleSince > 0.0f )
				{
					float fade = ( flTime - m_flInstructorManagedVisibleSince ) / 0.18f;
					if( fade < 0.0f ) fade = 0.0f;
					if( fade > 1.0f ) fade = 1.0f;
					panelAlpha = (int)( panelAlpha * ( 0.25f + 0.75f * fade ) );
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
				FillRGBA( panelX, panelY, 3, panelHeight, hintR, hintG, hintB, 255 );
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
				SPR_Set( hintIcon, hintR, hintG, hintB );
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
					"INSTRUCTION", hintR, hintG, hintB );
				DrawHudString( textAreaX, panelY + 31, textAreaRight,
					m_szInstructorHintCaption, 245, 247, 250 );

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
					DrawHudString( keyTextX, keyY + 6, keyX + keyChipWidth - 5,
						bindingText, hintR, hintG, hintB );
				}
			}
			else
			{
				int textX = textAreaX + ( textAreaWidth - captionWidth ) / 2;
				if( textX < textAreaX )
					textX = textAreaX;
				const int textOffsetY = ( panelHeight - basePanelHeight ) / 2;
				DrawHudString( textX, panelY + textOffsetY + 9, textAreaRight,
					m_szInstructorHintCaption, 245, 245, 245 );

				if( bindingText[0] )
				{
					int keyTextX = textAreaX + ( textAreaWidth - bindingWidth ) / 2;
					if( keyTextX < textAreaX )
						keyTextX = textAreaX;
					DrawHudString( keyTextX, panelY + textOffsetY + 32, textAreaRight,
						bindingText, hintR, hintG, hintB );
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

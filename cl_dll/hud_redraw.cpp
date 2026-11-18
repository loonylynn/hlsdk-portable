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
#include "cl_util.h"
#include "cl_entity.h"
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

// Resolve the active hint target to HUD viewport coordinates. TriAPI's
// WorldToScreen returns viewport-relative pixels and reports points behind
// the view separately; mirror those coordinates so the offscreen ray points
// toward the target rather than through the camera.
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

	*screenX = projected[0];
	*screenY = projected[1];
	*behind = ( zClipped != 0 );

	if( *behind )
	{
		// WorldToScreen cannot divide by a negative clip-space W. Reflect
		// its raw clip direction around the viewport center for edge placement.
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
	if( m_bInstructorHintActive && !intermission &&
		m_pCvarDraw && m_pCvarDraw->value && !( m_iHideHUDDisplay & HIDEHUD_ALL ) )
	{
		if( m_flInstructorHintEndTime > 0.0f && flTime >= m_flInstructorHintEndTime )
		{
			m_bInstructorHintActive = false;
			m_iInstructorHintId = 0;
			m_iInstructorHintTarget = 0;
			m_bInstructorHintHasTargetPosition = false;
			m_vecInstructorHintTargetOrigin[0] = 0.0f;
			m_vecInstructorHintTargetOrigin[1] = 0.0f;
			m_vecInstructorHintTargetOrigin[2] = 0.0f;
			m_flInstructorHintEndTime = 0.0f;
			m_iInstructorHintIconMode = 0;
			m_szInstructorHintCaption[0] = '\0';
			m_szInstructorHintBinding[0] = '\0';
		}
		else
		{
			char bindingText[80];
			bindingText[0] = '\0';
			if( m_szInstructorHintBinding[0] )
				snprintf( bindingText, sizeof( bindingText ), "[ %s ]", m_szInstructorHintBinding );

			int captionWidth = DrawHudStringLen( m_szInstructorHintCaption );
			int bindingWidth = DrawHudStringLen( bindingText );

			// Only the automatic func_pushable hint uses these two action sprites.
			// The files are optional so older game directories retain the text-only UI.
			HSPRITE hintIcon = 0;
			if( m_iInstructorHintId == -1 )
			{
				if( m_iInstructorHintIconMode == 2 )
					hintIcon = m_hInstructorHintHoldSprite;
				else if( m_iInstructorHintIconMode == 1 )
					hintIcon = m_hInstructorHintMoveSprite;
			}

			int iconWidth = hintIcon ? SPR_Width( hintIcon, 0 ) : 0;
			int iconHeight = hintIcon ? SPR_Height( hintIcon, 0 ) : 0;
			if( iconWidth <= 0 || iconHeight <= 0 || iconWidth > 64 || iconHeight > 64 )
			{
				hintIcon = 0;
				iconWidth = 0;
				iconHeight = 0;
			}

			int contentWidth = captionWidth + ( hintIcon ? iconWidth + 12 : 0 );
			if( bindingWidth > contentWidth )
				contentWidth = bindingWidth;
			int panelWidth = contentWidth + 36;
			const int maxPanelWidth = Q_max( 1, ScreenWidth - 24 );
			if( panelWidth > maxPanelWidth )
				panelWidth = maxPanelWidth;
			if( panelWidth < 180 && maxPanelWidth >= 180 )
				panelWidth = 180;

			const int basePanelHeight = bindingText[0] ? 58 : 42;
			const int panelHeight = Q_max( basePanelHeight, iconHeight + 16 );
			int panelX = ( ScreenWidth - panelWidth ) / 2;
			int panelY = ScreenHeight - panelHeight - 30;

			float targetX = 0.0f, targetY = 0.0f;
			bool targetBehind = false;
			const bool hasTarget = m_bInstructorHintHasTargetPosition &&
				HUD_GetInstructorHintScreenPosition( m_vecInstructorHintTargetOrigin,
					&targetX, &targetY, &targetBehind );

			if( hasTarget )
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
						arrow, 236, 9, 77 );

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

			FillRGBA( panelX, panelY, panelWidth, panelHeight, 8, 10, 14, 205 );
			FillRGBA( panelX, panelY, 3, panelHeight, 236, 9, 77, 255 );

			int textAreaX = panelX + 14;
			if( hintIcon )
			{
				const int iconX = panelX + 12;
				const int iconY = panelY + ( panelHeight - iconHeight ) / 2;
				SPR_Set( hintIcon, 236, 9, 77 );
				SPR_DrawAdditive( 0, iconX, iconY, NULL );
				textAreaX = iconX + iconWidth + 10;
			}

			const int textAreaRight = panelX + panelWidth - 10;
			const int textAreaWidth = Q_max( 1, textAreaRight - textAreaX );
			int textX = textAreaX + ( textAreaWidth - captionWidth ) / 2;
			if( textX < textAreaX )
				textX = textAreaX;
			const int textOffsetY = ( panelHeight - basePanelHeight ) / 2;
			DrawHudString( textX, panelY + textOffsetY + 9, textAreaRight,
				m_szInstructorHintCaption, 245, 245, 245 );

			if( bindingText[0] )
			{
				int keyX = textAreaX + ( textAreaWidth - bindingWidth ) / 2;
				if( keyX < textAreaX )
					keyX = textAreaX;
				DrawHudString( keyX, panelY + textOffsetY + 32, textAreaRight,
					bindingText, 236, 9, 77 );
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

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
// cl_util.h
//
#if !defined(CL_UTIL_H)
#define CL_UTIL_H
#include <assert.h>
#include "exportdef.h"
#include "cvardef.h"

#if !defined(TRUE)
#define TRUE 1
#define FALSE 0
#endif

// Macros to hook function calls into the HUD object

#define HOOK_MESSAGE(x) gEngfuncs.pfnHookUserMsg( #x, __MsgFunc_##x );

#define DECLARE_MESSAGE(y, x) int __MsgFunc_##x( const char *pszName, int iSize, void *pbuf ) \
						{ \
							return gHUD.y.MsgFunc_##x(pszName, iSize, pbuf ); \
						}

#define HOOK_COMMAND(x, y) gEngfuncs.pfnAddCommand( x, __CmdFunc_##y );
#define DECLARE_COMMAND(y, x) void __CmdFunc_##x( void ) \
							{ \
								gHUD.y.UserCmd_##x( ); \
							}

inline float CVAR_GET_FLOAT( const char *x ) {	return gEngfuncs.pfnGetCvarFloat( (char*)x ); }
inline char* CVAR_GET_STRING( const char *x ) {	return gEngfuncs.pfnGetCvarString( (char*)x ); }
inline struct cvar_s *CVAR_CREATE( const char *cv, const char *val, const int flags ) {	return gEngfuncs.pfnRegisterVariable( (char*)cv, (char*)val, flags ); }

#define SPR_Load ( *gEngfuncs.pfnSPR_Load )
#define SPR_Set ( *gEngfuncs.pfnSPR_Set )
#define SPR_Frames ( *gEngfuncs.pfnSPR_Frames )
#define SPR_GetList ( *gEngfuncs.pfnSPR_GetList )

// HUD-only overscan helpers. They move draw coordinates inward from screen edges.
void HUD_UpdateOverscan( void );
int HUD_AdjustX( int x );
int HUD_AdjustY( int y );
int HUD_AdjustScissorWidth( int x, int width );
int HUD_AdjustScissorHeight( int y, int height );
int HUD_AdjustWidth( int x, int width );
int HUD_AdjustHeight( int y, int height );

inline void HUD_SPR_Draw( int frame, int x, int y, const wrect_t *prc )
{
	gEngfuncs.pfnSPR_Draw( frame, HUD_AdjustX( x ), HUD_AdjustY( y ), prc );
}
inline void HUD_SPR_DrawHoles( int frame, int x, int y, const wrect_t *prc )
{
	gEngfuncs.pfnSPR_DrawHoles( frame, HUD_AdjustX( x ), HUD_AdjustY( y ), prc );
}
inline void HUD_SPR_DrawAdditive( int frame, int x, int y, const wrect_t *prc )
{
	gEngfuncs.pfnSPR_DrawAdditive( frame, HUD_AdjustX( x ), HUD_AdjustY( y ), prc );
}
inline void HUD_SPR_EnableScissor( int x, int y, int width, int height )
{
	gEngfuncs.pfnSPR_EnableScissor( HUD_AdjustX( x ), HUD_AdjustY( y ),
		HUD_AdjustScissorWidth( x, width ), HUD_AdjustScissorHeight( y, height ) );
}
inline void HUD_FillRGBA( int x, int y, int width, int height, int r, int g, int b, int a )
{
	gEngfuncs.pfnFillRGBA( HUD_AdjustX( x ), HUD_AdjustY( y ), HUD_AdjustWidth( x, width ), HUD_AdjustHeight( y, height ), r, g, b, a );
}
inline void HUD_FillRGBABlend( int x, int y, int width, int height, int r, int g, int b, int a )
{
	gEngfuncs.pfnFillRGBABlend( HUD_AdjustX( x ), HUD_AdjustY( y ), HUD_AdjustWidth( x, width ), HUD_AdjustHeight( y, height ), r, g, b, a );
}

#define SPR_Draw HUD_SPR_Draw
#define SPR_DrawHoles HUD_SPR_DrawHoles
#define SPR_DrawAdditive HUD_SPR_DrawAdditive
#define SPR_EnableScissor HUD_SPR_EnableScissor
#define SPR_DisableScissor ( *gEngfuncs.pfnSPR_DisableScissor )
#define FillRGBA HUD_FillRGBA
#define FillRGBABlend HUD_FillRGBABlend

// ScreenHeight returns the height of the screen, in pixels
#define ScreenHeight ( gHUD.m_scrinfo.iHeight )
// ScreenWidth returns the width of the screen, in pixels
#define ScreenWidth ( gHUD.m_scrinfo.iWidth )

// Use this to set any co-ords in 640x480 space
#define XRES(x)		( (int)( float(x) * ( (float)ScreenWidth / 640.0f ) + 0.5f ) )
#define YRES(y)		( (int)( float(y) * ( (float)ScreenHeight / 480.0f ) + 0.5f ) )
#define XRES_HD(x)      ( (int)( float(x) * Q_max(1.f, (float)ScreenWidth / 1280.f )))
#define YRES_HD(y)	( (int)( float(y) * Q_max(1.f, (float)ScreenHeight / 720.f )))

// use this to project world coordinates to screen coordinates
#define XPROJECT(x)	( ( 1.0f + (x) ) * ScreenWidth * 0.5f )
#define YPROJECT(y)	( ( 1.0f - (y) ) * ScreenHeight * 0.5f )

#define GetScreenInfo ( *gEngfuncs.pfnGetScreenInfo )
#define ServerCmd ( *gEngfuncs.pfnServerCmd )
#define ClientCmd ( *gEngfuncs.pfnClientCmd )
#define SetCrosshair ( *gEngfuncs.pfnSetCrosshair )
#define AngleVectors ( *gEngfuncs.pfnAngleVectors )
extern cvar_t *hud_textmode;
extern float g_hud_text_color[3];

// Apply the EC094D accent to the original amber and previous wine-red HUD text.
inline void HUD_ApplyTextColor( int &r, int &g, int &b )
{
	if( ( r == 255 && g == 160 && b == 0 ) ||
		( r == 79 && g == 26 && b == 35 ) )
		r = 236, g = 9, b = 77;
}

inline void HUD_ApplyTextColor( float &r, float &g, float &b )
{
	const int ir = (int)( r * 255.0f + 0.5f );
	const int ig = (int)( g * 255.0f + 0.5f );
	const int ib = (int)( b * 255.0f + 0.5f );

	if( ( ir == 255 && ig == 160 && ib == 0 ) ||
		( ir == 79 && ig == 26 && ib == 35 ) )
		r = 236.0f / 255.0f, g = 9.0f / 255.0f, b = 77.0f / 255.0f;
}

inline void DrawSetTextColor( float r, float g, float b )
{
	HUD_ApplyTextColor( r, g, b );

	if( hud_textmode->value == 1 )
		g_hud_text_color[0] = r, g_hud_text_color[1] = g, g_hud_text_color[2] = b;
	else
		gEngfuncs.pfnDrawSetTextColor( r, g, b );
}

// Gets the height & width of a sprite,  at the specified frame
inline int SPR_Height( HSPRITE x, int f )	{ return gEngfuncs.pfnSPR_Height(x, f); }
inline int SPR_Width( HSPRITE x, int f )	{ return gEngfuncs.pfnSPR_Width(x, f); }

inline client_textmessage_t *TextMessageGet( const char *pName )
{
	return gEngfuncs.pfnTextMessageGet( pName );
}

// Draw a character at an already-adjusted position. Text strings use this to
// move as a block while retaining the font's native glyph spacing.
inline int TextMessageDrawCharAt( int x, int y, int number, int r, int g, int b )
{
	HUD_ApplyTextColor( r, g, b );
	return gEngfuncs.pfnDrawCharacter( x, y, number, r, g, b );
}

inline int TextMessageDrawChar( int x, int y, int number, int r, int g, int b )
{
	return TextMessageDrawCharAt( HUD_AdjustX( x ), HUD_AdjustY( y ), number, r, g, b );
}

inline int DrawConsoleString( int x, int y, const char *string )
{
	if( hud_textmode->value == 1 )
		return gHUD.DrawHudString( x, y, 9999, (char*)string, (int)( (float)g_hud_text_color[0] * 255.0f ),
			(int)( (float)g_hud_text_color[1] * 255.0f ), (int)( (float)g_hud_text_color[2] * 255.0f ) );
	return gEngfuncs.pfnDrawConsoleString( HUD_AdjustX( x ), HUD_AdjustY( y ), (char*) string );
}

inline void GetConsoleStringSize( const char *string, int *width, int *height )
{
	if( hud_textmode->value == 1 )
		*height = 13, *width = gHUD.DrawHudStringLen( (char*)string );
	else
		gEngfuncs.pfnDrawConsoleStringLen( (char*)string, width, height );
}

int DrawUtfString( int xpos, int ypos, int iMaxX, const char *szIt, int r, int g, int b );

inline int ConsoleStringLen( const char *string )
{
	int _width = 0, _height = 0;
	if( hud_textmode->value == 1 )
		return gHUD.DrawHudStringLen( (char*)string );
	GetConsoleStringSize( string, &_width, &_height );
	return _width;
}

inline void ConsolePrint( const char *string )
{
	gEngfuncs.pfnConsolePrint( string );
}

inline void CenterPrint( const char *string )
{
	gEngfuncs.pfnCenterPrint( string );
}

// returns the players name of entity no.
#define GetPlayerInfo ( *gEngfuncs.pfnGetPlayerInfo )

// sound functions
inline void PlaySound( const char *szSound, float vol ) { gEngfuncs.pfnPlaySoundByName( szSound, vol ); }
inline void PlaySound( int iSound, float vol ) { gEngfuncs.pfnPlaySoundByIndex( iSound, vol ); }

#define Q_max(a, b)  (((a) > (b)) ? (a) : (b))
#define Q_min(a, b)  (((a) < (b)) ? (a) : (b))
#define fabs(x)	   ((x) > 0 ? (x) : 0 - (x))

inline int GetSpriteRes( int width, int height )
{
	int i;

	if( width < 640 )
		i = 320;
	else if( width < 1280 || !gHUD.m_pAllowHD->value )
		i = 640;
	else
	{
		if( height <= 720 )
			i = 640;
		else if( width <= 2560 || height <= 1600 )
			i = 1280;
		else
			i = 2560;
	}

	return Q_min( i, gHUD.m_iMaxRes );
}

void ScaleColors( int &r, int &g, int &b, int a );

#define DotProduct(x, y) ((x)[0] * (y)[0] + (x)[1] * (y)[1] + (x)[2] * (y)[2])
#define VectorSubtract(a, b, c) { (c)[0] = (a)[0] - (b)[0]; (c)[1] = (a)[1] - (b)[1]; (c)[2] = (a)[2] - (b)[2]; }
#define VectorAdd(a, b, c) { (c)[0] = (a)[0] + (b)[0]; (c)[1] = (a)[1] + (b)[1]; (c)[2] = (a)[2] + (b)[2]; }
#define VectorCopy(a, b) { (b)[0] = (a)[0]; (b)[1] = (a)[1]; (b)[2] = (a)[2]; }
inline void VectorClear( float *a ) { a[0] = 0.0; a[1] = 0.0; a[2] = 0.0; }
float Length( const float *v );
void VectorMA( const float *veca, float scale, const float *vecb, float *vecc );
void VectorScale( const float *in, float scale, float *out );
float VectorNormalize( float *v );
void VectorInverse( float *v );

// extern vec3_t vec3_origin;
extern float vec3_origin[3];

// disable 'possible loss of data converting float to int' warning message
#pragma warning( disable: 4244 )
// disable 'truncation from 'const double' to 'float' warning message
#pragma warning( disable: 4305 )

inline void UnpackRGB( int &r, int &g, int &b, unsigned long ulRGB )\
{\
	r = ( ulRGB & 0xFF0000 ) >> 16;\
	g = ( ulRGB & 0xFF00 ) >> 8;\
	b = ulRGB & 0xFF;\
}

HSPRITE LoadSprite( const char *pszName );

bool HUD_MessageBox( const char *msg );
bool IsXashFWGS();
#endif

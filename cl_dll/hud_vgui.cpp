#include "hud_vgui.h"
#include "hud.h"
#include "cl_util.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#include <VGUI_App.h>
#include <VGUI_Font.h>
#include <VGUI_Panel.h>
#include <VGUI_Scheme.h>

namespace
{

// The health HUD uses the active viewport font, enlarged without replacing
// the font selected by the game's VGUI scheme.
static int g_iNextHudFontId = 0x50000000;

// Rounded translucent backing used by the three VGUI HUD groups.
static const int HUD_VGUI_PANEL_PADDING_X = 8;
static const int HUD_VGUI_PANEL_PADDING_Y = 5;
static const int HUD_VGUI_PANEL_CORNER_RADIUS = 10;
static const int HUD_VGUI_PANEL_CORNER_INSET[10] = { 7, 5, 3, 2, 2, 1, 1, 0, 0, 0 };

// Keep the VGUI HUD safe area independent of the legacy sprite HUD cvar.
// This matches the project's established 2.5% default without inheriting
// user changes to hud_overscan.
static const float HUD_VGUI_FIXED_OVERSCAN = 0.025f;

class CHudScaledFont : public vgui::Font
{
public:
	explicit CHudScaledFont( vgui::Font *base ) :
		vgui::Font( "HUDHealthScaled", 1, 1, 0.0f, 400, false, false, false, false ),
		m_pBase( base ),
		m_iId( g_iNextHudFontId++ )
	{
	}

	virtual void getCharRGBA( int ch, int rgbaX, int rgbaY, int rgbaWide, int rgbaTall, uchar *rgba )
	{
		int a, b, c;
		int sourceTall;

		if( !m_pBase || !rgba )
			return;

		m_pBase->getCharABCwide( ch, a, b, c );
		(void)a;
		(void)c;
		sourceTall = m_pBase->getTall();

		if( b <= 0 || sourceTall <= 0 )
			return;

		std::vector<uchar> source( (size_t)b * sourceTall * 4, 0 );
		m_pBase->getCharRGBA( ch, 0, 0, b, sourceTall, &source[0] );

		for( int y = 0; y < sourceTall; y++ )
		{
			int destY0 = ( y * 3 ) / 2;
			int destY1 = (( y + 1 ) * 3 ) / 2;

			for( int x = 0; x < b; x++ )
			{
				int destX0 = ( x * 3 ) / 2;
				int destX1 = (( x + 1 ) * 3 ) / 2;
				const uchar *pixel = &source[((size_t)y * b + x) * 4];

				for( int dy = destY0; dy < destY1; dy++ )
				{
					for( int dx = destX0; dx < destX1; dx++ )
					{
						int outX = rgbaX + dx;
						int outY = rgbaY + dy;

						if( outX < 0 || outY < 0 || outX >= rgbaWide || outY >= rgbaTall )
							continue;

						memcpy( &rgba[((size_t)outY * rgbaWide + outX) * 4], pixel, 4 );
					}
				}
			}
		}
	}

	virtual void getCharABCwide( int ch, int &a, int &b, int &c )
	{
		if( !m_pBase )
		{
			a = b = c = 0;
			return;
		}

		m_pBase->getCharABCwide( ch, a, b, c );
		a = ScaleMetric( a );
		b = ScaleMetric( b );
		c = ScaleMetric( c );
	}

	virtual int getTall( void )
	{
		return m_pBase ? ScaleMetric( m_pBase->getTall() ) : 1;
	}

#ifndef _WIN32
	virtual int getWide( void )
	{
		return m_pBase ? ScaleMetric( m_pBase->getWide() ) : 1;
	}
#endif

	virtual int getId( void )
	{
		return m_iId;
	}

private:
	static int ScaleMetric( int value )
	{
		if( value >= 0 )
			return ( value * 3 + 1 ) / 2;

		return -( ( -value * 3 + 1 ) / 2 );
	}

	vgui::Font *m_pBase;
	int m_iId;
};

// This panel draws directly through the VGUI surface so that its text can
// request the engine's additive image pass without changing other VGUI panels.
class CHudHealthPanel : public vgui::Panel
{
public:
	CHudHealthPanel() :
		vgui::Panel( 0, 0, 1, 1 ),
		m_pNumberFont( NULL ),
		m_pCaptionFont( NULL ),
		m_iRed( 255 ),
		m_iGreen( 255 ),
		m_iBlue( 255 ),
		m_iBackgroundOpacity( -1 ),
		m_iNumberX( 0 ),
		m_iCaptionX( 0 ),
		m_iNumberY( 0 ),
		m_iCaptionY( 0 )
	{
		strncpy( m_szHealth, "100", sizeof( m_szHealth ) );
		m_szHealth[sizeof( m_szHealth ) - 1] = '\0';
		setPaintBorderEnabled( false );
		setPaintBackgroundEnabled( false );
		setPaintEnabled( true );
	}

	virtual bool isWithin( int x, int y )
	{
		(void)x;
		(void)y;
		return false;
	}

	void SetFonts( vgui::Font *numberFont, vgui::Font *captionFont )
	{
		m_pNumberFont = numberFont;
		m_pCaptionFont = captionFont;
	}

	void SetHealthText( const char *text )
	{
		if( !text || !strcmp( m_szHealth, text ) )
			return;

		strncpy( m_szHealth, text, sizeof( m_szHealth ) );
		m_szHealth[sizeof( m_szHealth ) - 1] = '\0';
	}

	void SetHealthColor( int red, int green, int blue )
	{
		m_iRed = red;
		m_iGreen = green;
		m_iBlue = blue;
	}

	bool SetBackgroundOpacity( int opacity )
	{
		if( opacity < 0 ) opacity = 0;
		if( opacity > 255 ) opacity = 255;
		if( m_iBackgroundOpacity == opacity )
			return false;

		m_iBackgroundOpacity = opacity;
		return true;
	}

	bool SetTextLayout( int numberX, int numberY, int captionX, int captionY )
	{
		if( m_iNumberX == numberX && m_iNumberY == numberY &&
			m_iCaptionX == captionX && m_iCaptionY == captionY )
			return false;

		m_iNumberX = numberX;
		m_iNumberY = numberY;
		m_iCaptionX = captionX;
		m_iCaptionY = captionY;
		return true;
	}

protected:
	void PaintRoundedBackground( void )
	{
		int wide, tall;
		int radius = HUD_VGUI_PANEL_CORNER_RADIUS;

		if( m_iBackgroundOpacity <= 0 )
			return;

		getSize( wide, tall );
		if( wide <= 0 || tall <= 0 )
			return;

		if( radius * 2 > wide )
			radius = wide / 2;
		if( radius * 2 > tall )
			radius = tall / 2;

		// Match the standard VGUI panel alpha path. The engine bridge accepts
		// transparency here, so invert the user-facing opacity value.
		drawSetColor( 0, 0, 0, 255 - m_iBackgroundOpacity );

		if( tall > radius * 2 )
			drawFilledRect( 0, radius, wide, tall - radius );

		for( int row = 0; row < radius; row++ )
		{
			int profileRow = ( row * HUD_VGUI_PANEL_CORNER_RADIUS ) / radius;
			if( profileRow >= HUD_VGUI_PANEL_CORNER_RADIUS )
				profileRow = HUD_VGUI_PANEL_CORNER_RADIUS - 1;
			int inset = ( HUD_VGUI_PANEL_CORNER_INSET[profileRow] * radius +
				HUD_VGUI_PANEL_CORNER_RADIUS / 2 ) / HUD_VGUI_PANEL_CORNER_RADIUS;
			drawFilledRect( inset, row, wide - inset, row + 1 );
			drawFilledRect( inset, tall - row - 1, wide - inset, tall - row );
		}
	}

	virtual void paint( void )
	{
		static const char caption[] = "HEALTH";

		if( !m_pNumberFont || !m_pCaptionFont )
			return;

		PaintRoundedBackground();

		// A negative VGUI alpha is an internal marker consumed by the
		// Xash3D VGUI bridge. It is never stored in a VGUI Color object.
		drawSetTextColor( m_iRed, m_iGreen, m_iBlue, -1 );

		drawSetTextFont( m_pCaptionFont );
		drawSetTextPos( m_iCaptionX, m_iCaptionY );
		drawPrintText( caption, sizeof( caption ) - 1 );

		drawSetTextFont( m_pNumberFont );
		drawSetTextPos( m_iNumberX, m_iNumberY );
		drawPrintText( m_szHealth, (int)strlen( m_szHealth ) );
	}

private:
	vgui::Font *m_pNumberFont;
	vgui::Font *m_pCaptionFont;
	int m_iRed;
	int m_iGreen;
	int m_iBlue;
	int m_iBackgroundOpacity;
	int m_iNumberX;
	int m_iCaptionX;
	int m_iNumberY;
	int m_iCaptionY;
	char m_szHealth[16];
};

 
// Shared VGUI text panel for the suit and weapon-ammo readouts. It uses the
// same OCR-B number font and additive text path as the health panel.
class CHudLabeledTextPanel : public vgui::Panel
{
public:
	CHudLabeledTextPanel() :
		vgui::Panel( 0, 0, 1, 1 ),
		m_pValueFont( NULL ),
		m_pCaptionFont( NULL ),
		m_pSmallValueFont( NULL ),
		m_iRed( 255 ),
		m_iGreen( 255 ),
		m_iBlue( 255 ),
		m_iBackgroundOpacity( -1 ),
		m_iValueX( 0 ),
		m_iCaptionX( 0 ),
		m_iValueY( 0 ),
		m_iCaptionY( 0 )
	{
		m_szCaption[0] = '\0';
		m_szValue[0] = '\0';
		setPaintBorderEnabled( false );
		setPaintBackgroundEnabled( false );
		setPaintEnabled( true );
	}

	virtual bool isWithin( int x, int y )
	{
		(void)x;
		(void)y;
		return false;
	}

	void SetFonts( vgui::Font *valueFont, vgui::Font *captionFont,
		vgui::Font *smallValueFont = NULL )
	{
		m_pValueFont = valueFont;
		m_pCaptionFont = captionFont;
		m_pSmallValueFont = smallValueFont;
	}

	void GetValueTextSize( const char *value, int &wide, int &tall )
	{
		const char *separator = ( m_pSmallValueFont && value ) ? strchr( value, '|' ) : NULL;

		if( !separator )
		{
			m_pValueFont->getTextSize( value, wide, tall );
			return;
		}

		char primary[64];
		size_t primaryLength = (size_t)( separator - value );
		if( primaryLength >= sizeof( primary ) )
			primaryLength = sizeof( primary ) - 1;
		memcpy( primary, value, primaryLength );
		primary[primaryLength] = '\0';

		int primaryWide, primaryTall;
		int separatorWide, separatorTall;
		int reserveWide, reserveTall;
		m_pValueFont->getTextSize( primary, primaryWide, primaryTall );
		m_pValueFont->getTextSize( "|", separatorWide, separatorTall );
		m_pSmallValueFont->getTextSize( separator + 1, reserveWide, reserveTall );

		wide = primaryWide + separatorWide + reserveWide;
		tall = primaryTall > separatorTall ? primaryTall : separatorTall;
		if( reserveTall > tall )
			tall = reserveTall;
	}

	bool SetText( const char *caption, const char *value )
	{
		bool changed = false;

		if( caption && strcmp( m_szCaption, caption ) )
		{
			strncpy( m_szCaption, caption, sizeof( m_szCaption ) );
			m_szCaption[sizeof( m_szCaption ) - 1] = '\0';
			changed = true;
		}

		if( value && strcmp( m_szValue, value ) )
		{
			strncpy( m_szValue, value, sizeof( m_szValue ) );
			m_szValue[sizeof( m_szValue ) - 1] = '\0';
			changed = true;
		}

		return changed;
	}

	bool SetTextColor( int red, int green, int blue )
	{
		if( m_iRed == red && m_iGreen == green && m_iBlue == blue )
			return false;

		m_iRed = red;
		m_iGreen = green;
		m_iBlue = blue;
		return true;
	}

	bool SetBackgroundOpacity( int opacity )
	{
		if( opacity < 0 ) opacity = 0;
		if( opacity > 255 ) opacity = 255;
		if( m_iBackgroundOpacity == opacity )
			return false;

		m_iBackgroundOpacity = opacity;
		return true;
	}

	bool SetTextLayout( int valueX, int valueY, int captionX, int captionY )
	{
		if( m_iValueX == valueX && m_iValueY == valueY &&
			m_iCaptionX == captionX && m_iCaptionY == captionY )
			return false;

		m_iValueX = valueX;
		m_iValueY = valueY;
		m_iCaptionX = captionX;
		m_iCaptionY = captionY;
		return true;
	}

protected:
	void PaintRoundedBackground( void )
	{
		int wide, tall;
		int radius = HUD_VGUI_PANEL_CORNER_RADIUS;

		if( m_iBackgroundOpacity <= 0 )
			return;

		getSize( wide, tall );
		if( wide <= 0 || tall <= 0 )
			return;

		if( radius * 2 > wide )
			radius = wide / 2;
		if( radius * 2 > tall )
			radius = tall / 2;

		// Match the standard VGUI panel alpha path. The engine bridge accepts
		// transparency here, so invert the user-facing opacity value.
		drawSetColor( 0, 0, 0, 255 - m_iBackgroundOpacity );

		if( tall > radius * 2 )
			drawFilledRect( 0, radius, wide, tall - radius );

		for( int row = 0; row < radius; row++ )
		{
			int profileRow = ( row * HUD_VGUI_PANEL_CORNER_RADIUS ) / radius;
			if( profileRow >= HUD_VGUI_PANEL_CORNER_RADIUS )
				profileRow = HUD_VGUI_PANEL_CORNER_RADIUS - 1;
			int inset = ( HUD_VGUI_PANEL_CORNER_INSET[profileRow] * radius +
				HUD_VGUI_PANEL_CORNER_RADIUS / 2 ) / HUD_VGUI_PANEL_CORNER_RADIUS;
			drawFilledRect( inset, row, wide - inset, row + 1 );
			drawFilledRect( inset, tall - row - 1, wide - inset, tall - row );
		}
	}

	virtual void paint( void )
	{
		if( !m_pValueFont || !m_pCaptionFont || !m_szCaption[0] || !m_szValue[0] )
			return;

		PaintRoundedBackground();

		// Negative alpha is the private marker for Xash3D's scoped additive
		// VGUI text pass. It is not stored in a VGUI Color object.
		drawSetTextColor( m_iRed, m_iGreen, m_iBlue, -1 );

		drawSetTextFont( m_pCaptionFont );
		drawSetTextPos( m_iCaptionX, m_iCaptionY );
		drawPrintText( m_szCaption, (int)strlen( m_szCaption ) );

		const char *separator = m_pSmallValueFont ? strchr( m_szValue, '|' ) : NULL;
		if( separator )
		{
			char primary[64];
			size_t primaryLength = (size_t)( separator - m_szValue );
			if( primaryLength >= sizeof( primary ) )
				primaryLength = sizeof( primary ) - 1;
			memcpy( primary, m_szValue, primaryLength );
			primary[primaryLength] = '\0';

			int primaryWide, primaryTall;
			int separatorWide, separatorTall;
			m_pValueFont->getTextSize( primary, primaryWide, primaryTall );
			m_pValueFont->getTextSize( "|", separatorWide, separatorTall );

			drawSetTextFont( m_pValueFont );
			drawSetTextPos( m_iValueX, m_iValueY );
			drawPrintText( primary, (int)strlen( primary ) );
			drawSetTextPos( m_iValueX + primaryWide, m_iValueY );
			drawPrintText( "|", 1 );

			drawSetTextFont( m_pSmallValueFont );
			drawSetTextPos( m_iValueX + primaryWide + separatorWide, m_iValueY );
			drawPrintText( separator + 1, (int)strlen( separator + 1 ) );
		}
		else
		{
			drawSetTextFont( m_pValueFont );
			drawSetTextPos( m_iValueX, m_iValueY );
			drawPrintText( m_szValue, (int)strlen( m_szValue ) );
		}
	}

private:
	vgui::Font *m_pValueFont;
	vgui::Font *m_pCaptionFont;
	vgui::Font *m_pSmallValueFont;
	int m_iRed;
	int m_iGreen;
	int m_iBlue;
	int m_iBackgroundOpacity;
	int m_iValueX;
	int m_iCaptionX;
	int m_iValueY;
	int m_iCaptionY;
	char m_szCaption[24];
	char m_szValue[64];
};

CHudHealthPanel *g_pHudHealthPanel = NULL;
CHudLabeledTextPanel *g_pHudSuitPanel = NULL;
CHudLabeledTextPanel *g_pHudAmmoPanel = NULL;
vgui::Font *g_pHudHealthNumberFont = NULL;
CHudScaledFont *g_pHudHealthScaledFallback = NULL;
vgui::Font *g_pHudOCRBFont = NULL;
vgui::Font *g_pHudOCRBAmmoReserveFont = NULL;
vgui::Font *g_pHudHealthCaptionFont = NULL;
int g_iLastHealth = -1;
int g_iLastRed = -1;
int g_iLastGreen = -1;
int g_iLastBlue = -1;

static int HUD_VGuiAdjustX( int x )
{
	const int width = gHUD.m_scrinfo.iWidth;
	if( width <= 0 || x < 0 || x > width )
		return x;

	const float scale = 1.0f - 2.0f * HUD_VGUI_FIXED_OVERSCAN;
	return (int)( (float)width * 0.5f +
		( (float)x - (float)width * 0.5f ) * scale + 0.5f );
}

static int HUD_VGuiAdjustY( int y )
{
	const int height = gHUD.m_scrinfo.iHeight;
	if( height <= 0 || y < 0 || y > height )
		return y;

	const float scale = 1.0f - 2.0f * HUD_VGUI_FIXED_OVERSCAN;
	return (int)( (float)height * 0.5f +
		( (float)y - (float)height * 0.5f ) * scale + 0.5f );
}

static int HUD_VGuiReadOffset( const char *name )
{
	return (int)CVAR_GET_FLOAT( name );
}

static int HUD_VGuiReadPanelOpacity( void )
{
	int opacity = HUD_VGuiReadOffset( "hud_vgui_panel_opacity" );
	if( opacity < 0 ) opacity = 0;
	if( opacity > 255 ) opacity = 255;
	return opacity;
}

void HUD_VGuiHealthResetCache( void )
{
	g_iLastHealth = -1;
	g_iLastRed = -1;
	g_iLastGreen = -1;
	g_iLastBlue = -1;
}

}

static vgui::Font *HUD_VGuiHealthTryLoadOCRBFont( const char *fontPath, int fontTall )
{
	int fontFileLength = 0;
	byte *pFontData;
	vgui::Font *font = NULL;

	pFontData = gEngfuncs.COM_LoadFile( fontPath, 5, &fontFileLength );
	if( !pFontData )
	{
		gEngfuncs.Con_DPrintf( "VGUI health: %s not found\n", fontPath );
		return NULL;
	}

	// FreeVGUI first recognises its legacy TGA bitmap format. If the data is
	// not a bitmap sheet, its POSIX backend attempts a FreeType memory face.
	// The FreeType implementation copies the bytes before this buffer is freed.
	font = new vgui::Font( "OCR-B", pFontData, fontFileLength,
		fontTall, 0, 0.0f, 400, false, false, false, false );

	if( font && !font->getPlat() )
	{
		delete font;
		font = NULL;
	}

	gEngfuncs.COM_FreeFile( pFontData );

	if( font )
		gEngfuncs.Con_DPrintf( "VGUI health: loaded OCR-B font from %s\n", fontPath );

	return font;
}

static vgui::Font *HUD_VGuiHealthLoadOCRBFont( void )
{
	vgui::Font *font;

	// Prefer the real TrueType file. The TGA atlas remains as a compatibility
	// fallback for builds/platforms which do not have the FreeType backend.
	font = HUD_VGuiHealthTryLoadOCRBFont( "resource/ocr-b.ttf", 54 );
	if( font )
		return font;

	font = HUD_VGuiHealthTryLoadOCRBFont( "resource/ocr-b-vgui.tga", 0 );
	if( font )
		return font;

	gEngfuncs.Con_DPrintf( "VGUI health: OCR-B files unavailable; using the VGUI scheme font\n" );
	return NULL;
}

void HUD_VGuiHealthStartup( vgui::Panel *parent )
{
	vgui::App *app;
	vgui::Scheme *scheme;
	vgui::Font *baseNumberFont;

	if( !parent )
		return;

	app = vgui::App::getInstance();
	scheme = app ? app->getScheme() : NULL;
	if( !scheme )
		return;

	// The viewport installs its game scheme before this startup hook runs.
	baseNumberFont = scheme->getFont( vgui::Scheme::sf_primary1 );
	g_pHudHealthCaptionFont = scheme->getFont( vgui::Scheme::sf_primary3 );
	if( !baseNumberFont || !g_pHudHealthCaptionFont )
		return;

	// Keep the OCR-B font alive for the lifetime of the client. FreeVGUI
	// retains the TrueType/bitmap platform font in its static font registry.
	if( !g_pHudOCRBFont )
		g_pHudOCRBFont = HUD_VGuiHealthLoadOCRBFont();

	if( g_pHudOCRBFont && !g_pHudOCRBAmmoReserveFont )
		g_pHudOCRBAmmoReserveFont = HUD_VGuiHealthTryLoadOCRBFont(
			"resource/ocr-b.ttf", 38 );

	if( g_pHudOCRBFont )
	{
		if( g_pHudHealthScaledFallback )
		{
			delete g_pHudHealthScaledFallback;
			g_pHudHealthScaledFallback = NULL;
		}

		g_pHudHealthNumberFont = g_pHudOCRBFont;
	}
	else
	{
		if( g_pHudHealthScaledFallback )
			delete g_pHudHealthScaledFallback;

		// Preserve the larger scheme-font display when the custom atlas is absent.
		g_pHudHealthScaledFallback = new CHudScaledFont( baseNumberFont );
		g_pHudHealthNumberFont = g_pHudHealthScaledFallback;
	}

	if( !g_pHudHealthPanel )
		g_pHudHealthPanel = new CHudHealthPanel();

	g_pHudHealthPanel->SetFonts( g_pHudHealthNumberFont, g_pHudHealthCaptionFont );

	if( g_pHudHealthPanel->getParent() != parent )
	{
		if( g_pHudHealthPanel->getParent() )
			g_pHudHealthPanel->getParent()->removeChild( g_pHudHealthPanel );

		// Child zero draws beneath viewport menus and dialog panels.
		parent->insertChildAt( g_pHudHealthPanel, 0 );
	}

	g_pHudHealthPanel->setVisible( false );
	g_pHudHealthPanel->repaint();
	HUD_VGuiHealthResetCache();
}

void HUD_VGuiHealthShutdown( void )
{
	if( g_pHudHealthPanel )
	{
		// The parent does not own its child panels. Detach before viewport teardown.
		if( g_pHudHealthPanel->getParent() )
		{
			vgui::Panel *parent = g_pHudHealthPanel->getParent();
			parent->removeChild( g_pHudHealthPanel );
			parent->repaint();
		}

		g_pHudHealthPanel->setVisible( false );
		g_pHudHealthPanel->SetFonts( NULL, NULL );
	}

	// The viewport owns the caption font. The OCR-B bitmap font persists
	// with FreeVGUI's font registry; only dispose the per-viewport fallback.
	if( g_pHudHealthScaledFallback )
	{
		delete g_pHudHealthScaledFallback;
		g_pHudHealthScaledFallback = NULL;
	}

	g_pHudHealthNumberFont = NULL;
	g_pHudHealthCaptionFont = NULL;
	HUD_VGuiHealthResetCache();
}

void HUD_VGuiHealthSetVisible( int visible )
{
	if( !g_pHudHealthPanel )
		return;

	bool shouldBeVisible = ( visible != 0 );
	if( g_pHudHealthPanel->isVisible() != shouldBeVisible )
	{
		g_pHudHealthPanel->setVisible( shouldBeVisible );
		g_pHudHealthPanel->repaint();
	}
}

int HUD_VGuiHealthUpdate( int health, int red, int green, int blue, int x, int y )
{
	static const char caption[] = "HEALTH";
	char healthText[16];
	int numberWide, numberTall;
	int captionWide, captionTall;
	int numberAbsX, numberAbsY, captionAbsX, captionAbsY;
	int left, top, right, bottom;
	int panelWide, panelTall;
	int numberX, numberY, captionX, captionY;
	int oldWide, oldTall, oldX, oldY;
	int textOffsetX, textOffsetY, numberOffsetX, numberOffsetY;
	bool changed = false;

	if( !g_pHudHealthPanel || !g_pHudHealthNumberFont || !g_pHudHealthCaptionFont )
		return 0;

	// Preserve the legacy anchor, but apply the VGUI HUD's fixed safe area.
	// This is deliberately independent of the user-adjustable hud_overscan.
	x = HUD_VGuiAdjustX( x );
	y = HUD_VGuiAdjustY( y );
	textOffsetX = HUD_VGuiReadOffset( "hud_vgui_text_x" );
	textOffsetY = HUD_VGuiReadOffset( "hud_vgui_text_y" );
	numberOffsetX = HUD_VGuiReadOffset( "hud_vgui_numbers_x" );
	numberOffsetY = HUD_VGuiReadOffset( "hud_vgui_numbers_y" );

	snprintf( healthText, sizeof( healthText ), "%03d", health );

	if( health != g_iLastHealth )
	{
		g_pHudHealthPanel->SetHealthText( healthText );
		g_iLastHealth = health;
		changed = true;
	}

	changed |= g_pHudHealthPanel->SetBackgroundOpacity( HUD_VGuiReadPanelOpacity() );

	if( red != g_iLastRed || green != g_iLastGreen || blue != g_iLastBlue )
	{
		g_pHudHealthPanel->SetHealthColor( red, green, blue );
		g_iLastRed = red;
		g_iLastGreen = green;
		g_iLastBlue = blue;
		changed = true;
	}

	g_pHudHealthNumberFont->getTextSize( healthText, numberWide, numberTall );
	g_pHudHealthCaptionFont->getTextSize( caption, captionWide, captionTall );

	if( numberWide < 1 ) numberWide = 1;
	if( numberTall < 1 ) numberTall = 1;
	if( captionWide < 1 ) captionWide = 1;
	if( captionTall < 1 ) captionTall = 1;

	numberAbsX = x + numberOffsetX;
	numberAbsY = y + numberOffsetY;
	captionAbsX = numberAbsX + textOffsetX;
	captionAbsY = y - captionTall - 2 + textOffsetY;

	left = ( numberAbsX < captionAbsX ? numberAbsX : captionAbsX ) - HUD_VGUI_PANEL_PADDING_X;
	top = ( numberAbsY < captionAbsY ? numberAbsY : captionAbsY ) - HUD_VGUI_PANEL_PADDING_Y;
	right = ( numberAbsX + numberWide ) > ( captionAbsX + captionWide ) ?
		( numberAbsX + numberWide ) : ( captionAbsX + captionWide );
	right += HUD_VGUI_PANEL_PADDING_X;
	bottom = ( numberAbsY + numberTall ) > ( captionAbsY + captionTall ) ?
		( numberAbsY + numberTall ) : ( captionAbsY + captionTall );
	bottom += HUD_VGUI_PANEL_PADDING_Y;

	panelWide = right - left;
	panelTall = bottom - top;
	if( panelWide < 1 ) panelWide = 1;
	if( panelTall < 1 ) panelTall = 1;

	numberX = numberAbsX - left;
	numberY = numberAbsY - top;
	captionX = captionAbsX - left;
	captionY = captionAbsY - top;

	g_pHudHealthPanel->getSize( oldWide, oldTall );
	if( panelWide != oldWide || panelTall != oldTall )
	{
		g_pHudHealthPanel->setSize( panelWide, panelTall );
		changed = true;
	}

	changed |= g_pHudHealthPanel->SetTextLayout( numberX, numberY, captionX, captionY );

	g_pHudHealthPanel->getPos( oldX, oldY );
	if( left != oldX || top != oldY )
	{
		g_pHudHealthPanel->setPos( left, top );
		changed = true;
	}

	if( !g_pHudHealthPanel->isVisible() )
	{
		g_pHudHealthPanel->setVisible( true );
		changed = true;
	}

	if( changed )
		g_pHudHealthPanel->repaint();

	return 1;
}


static int HUD_VGuiUpdateLabeledPanel( CHudLabeledTextPanel *panel,
	const char *caption, const char *value, int red, int green, int blue,
	int anchorX, int anchorY, bool rightAlignValue )
{
	int valueWide, valueTall;
	int captionWide, captionTall;
	int valueAbsX, valueAbsY, captionAbsX, captionAbsY;
	int left, top, right, bottom;
	int panelWidePixels, panelTallPixels;
	int valueX, valueY, captionX, captionY;
	int oldWide, oldTall, oldX, oldY;
	int textOffsetX, textOffsetY, numberOffsetX, numberOffsetY;
	bool changed = false;

	if( !panel || !g_pHudHealthNumberFont || !g_pHudHealthCaptionFont )
		return 0;

	// Apply the same fixed VGUI safe area to every readout, independent of
	// hud_overscan; the shared offsets are then added to text and numbers.
	anchorX = HUD_VGuiAdjustX( anchorX );
	anchorY = HUD_VGuiAdjustY( anchorY );
	textOffsetX = HUD_VGuiReadOffset( "hud_vgui_text_x" );
	textOffsetY = HUD_VGuiReadOffset( "hud_vgui_text_y" );
	numberOffsetX = HUD_VGuiReadOffset( "hud_vgui_numbers_x" );
	numberOffsetY = HUD_VGuiReadOffset( "hud_vgui_numbers_y" );

	changed |= panel->SetText( caption, value );
	changed |= panel->SetTextColor( red, green, blue );
	changed |= panel->SetBackgroundOpacity( HUD_VGuiReadPanelOpacity() );

	panel->GetValueTextSize( value, valueWide, valueTall );
	g_pHudHealthCaptionFont->getTextSize( caption, captionWide, captionTall );

	if( valueWide < 1 ) valueWide = 1;
	if( valueTall < 1 ) valueTall = 1;
	if( captionWide < 1 ) captionWide = 1;
	if( captionTall < 1 ) captionTall = 1;

	// Keep the numeric readout's anchor, but left-align its caption to the
	// first digit instead of centering the caption over the number block.
	if( rightAlignValue )
		valueAbsX = anchorX - valueWide;
	else
		valueAbsX = anchorX;
	captionAbsX = valueAbsX;
	valueAbsY = anchorY;
	captionAbsY = anchorY - captionTall - 2;

	valueAbsX += numberOffsetX;
	valueAbsY += numberOffsetY;
	captionAbsX += textOffsetX;
	captionAbsY += textOffsetY;

	left = ( valueAbsX < captionAbsX ? valueAbsX : captionAbsX ) - HUD_VGUI_PANEL_PADDING_X;
	top = ( valueAbsY < captionAbsY ? valueAbsY : captionAbsY ) - HUD_VGUI_PANEL_PADDING_Y;
	right = ( valueAbsX + valueWide ) > ( captionAbsX + captionWide ) ?
		( valueAbsX + valueWide ) : ( captionAbsX + captionWide );
	right += HUD_VGUI_PANEL_PADDING_X;
	bottom = ( valueAbsY + valueTall ) > ( captionAbsY + captionTall ) ?
		( valueAbsY + valueTall ) : ( captionAbsY + captionTall );
	bottom += HUD_VGUI_PANEL_PADDING_Y;

	panelWidePixels = right - left;
	panelTallPixels = bottom - top;
	if( panelWidePixels < 1 ) panelWidePixels = 1;
	if( panelTallPixels < 1 ) panelTallPixels = 1;

	valueX = valueAbsX - left;
	valueY = valueAbsY - top;
	captionX = captionAbsX - left;
	captionY = captionAbsY - top;

	panel->getSize( oldWide, oldTall );
	if( panelWidePixels != oldWide || panelTallPixels != oldTall )
	{
		panel->setSize( panelWidePixels, panelTallPixels );
		changed = true;
	}

	changed |= panel->SetTextLayout( valueX, valueY, captionX, captionY );

	panel->getPos( oldX, oldY );
	if( left != oldX || top != oldY )
	{
		panel->setPos( left, top );
		changed = true;
	}

	if( !panel->isVisible() )
	{
		panel->setVisible( true );
		changed = true;
	}

	if( changed )
		panel->repaint();

	return 1;
}

static void HUD_VGuiDetachLabeledPanel( CHudLabeledTextPanel *panel )
{
	if( !panel )
		return;

	if( panel->getParent() )
	{
		vgui::Panel *parent = panel->getParent();
		parent->removeChild( panel );
		parent->repaint();
	}

	panel->setVisible( false );
	panel->SetFonts( NULL, NULL );
}

void HUD_VGuiSuitStartup( vgui::Panel *parent )
{
	if( !parent || !g_pHudHealthNumberFont || !g_pHudHealthCaptionFont )
		return;

	if( !g_pHudSuitPanel )
		g_pHudSuitPanel = new CHudLabeledTextPanel();

	g_pHudSuitPanel->SetFonts( g_pHudHealthNumberFont, g_pHudHealthCaptionFont );
	g_pHudSuitPanel->SetText( "SUIT", "000" );

	if( g_pHudSuitPanel->getParent() != parent )
	{
		if( g_pHudSuitPanel->getParent() )
			g_pHudSuitPanel->getParent()->removeChild( g_pHudSuitPanel );
		parent->insertChildAt( g_pHudSuitPanel, 0 );
	}

	g_pHudSuitPanel->setVisible( false );
	g_pHudSuitPanel->repaint();
}

void HUD_VGuiSuitShutdown( void )
{
	HUD_VGuiDetachLabeledPanel( g_pHudSuitPanel );
}

void HUD_VGuiSuitSetVisible( int visible )
{
	if( !g_pHudSuitPanel )
		return;

	bool shouldBeVisible = ( visible != 0 );
	if( g_pHudSuitPanel->isVisible() != shouldBeVisible )
	{
		g_pHudSuitPanel->setVisible( shouldBeVisible );
		g_pHudSuitPanel->repaint();
	}
}

int HUD_VGuiSuitUpdate( int battery, int red, int green, int blue, int x, int y )
{
	char value[16];
	snprintf( value, sizeof( value ), "%03d", battery );
	return HUD_VGuiUpdateLabeledPanel( g_pHudSuitPanel, "SUIT", value,
		red, green, blue, x, y, false );
}

void HUD_VGuiAmmoStartup( vgui::Panel *parent )
{
	if( !parent || !g_pHudHealthNumberFont || !g_pHudHealthCaptionFont )
		return;

	if( !g_pHudAmmoPanel )
		g_pHudAmmoPanel = new CHudLabeledTextPanel();

	g_pHudAmmoPanel->SetFonts( g_pHudHealthNumberFont, g_pHudHealthCaptionFont,
		g_pHudOCRBAmmoReserveFont );
	g_pHudAmmoPanel->SetText( "AMMO", "000" );

	if( g_pHudAmmoPanel->getParent() != parent )
	{
		if( g_pHudAmmoPanel->getParent() )
			g_pHudAmmoPanel->getParent()->removeChild( g_pHudAmmoPanel );
		parent->insertChildAt( g_pHudAmmoPanel, 0 );
	}

	g_pHudAmmoPanel->setVisible( false );
	g_pHudAmmoPanel->repaint();
}

void HUD_VGuiAmmoShutdown( void )
{
	HUD_VGuiDetachLabeledPanel( g_pHudAmmoPanel );
}

void HUD_VGuiAmmoSetVisible( int visible )
{
	if( !g_pHudAmmoPanel )
		return;

	bool shouldBeVisible = ( visible != 0 );
	if( g_pHudAmmoPanel->isVisible() != shouldBeVisible )
	{
		g_pHudAmmoPanel->setVisible( shouldBeVisible );
		g_pHudAmmoPanel->repaint();
	}
}

int HUD_VGuiAmmoUpdate( int clip, int reserve, int hasClip,
	int red, int green, int blue, int rightX, int y )
{
	char value[64];

	if( hasClip )
		snprintf( value, sizeof( value ), "%03d|%03d", clip, reserve );
	else
		snprintf( value, sizeof( value ), "%03d", reserve );

	return HUD_VGuiUpdateLabeledPanel( g_pHudAmmoPanel, "AMMO", value,
		red, green, blue, rightX, y, true );
}

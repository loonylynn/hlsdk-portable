#include "hud_vgui.h"
#include "hud.h"

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
		m_iNumberX( 0 ),
		m_iCaptionX( 0 ),
		m_iNumberY( 0 )
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

	void SetTextLayout( int numberX, int captionX, int numberY )
	{
		m_iNumberX = numberX;
		m_iCaptionX = captionX;
		m_iNumberY = numberY;
	}

protected:
	virtual void paint( void )
	{
		static const char caption[] = "HEALTH";

		if( !m_pNumberFont || !m_pCaptionFont )
			return;

		// A negative VGUI alpha is an internal marker consumed by the
		// Xash3D VGUI bridge. It is never stored in a VGUI Color object.
		drawSetTextColor( m_iRed, m_iGreen, m_iBlue, -1 );

		drawSetTextFont( m_pCaptionFont );
		drawSetTextPos( m_iCaptionX, 0 );
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
	int m_iNumberX;
	int m_iCaptionX;
	int m_iNumberY;
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
		m_iRed( 255 ),
		m_iGreen( 255 ),
		m_iBlue( 255 ),
		m_iValueX( 0 ),
		m_iCaptionX( 0 ),
		m_iValueY( 0 )
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

	void SetFonts( vgui::Font *valueFont, vgui::Font *captionFont )
	{
		m_pValueFont = valueFont;
		m_pCaptionFont = captionFont;
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

	bool SetTextLayout( int valueX, int captionX, int valueY )
	{
		if( m_iValueX == valueX && m_iCaptionX == captionX && m_iValueY == valueY )
			return false;

		m_iValueX = valueX;
		m_iCaptionX = captionX;
		m_iValueY = valueY;
		return true;
	}

protected:
	virtual void paint( void )
	{
		if( !m_pValueFont || !m_pCaptionFont || !m_szCaption[0] || !m_szValue[0] )
			return;

		// Negative alpha is the private marker for Xash3D's scoped additive
		// VGUI text pass. It is not stored in a VGUI Color object.
		drawSetTextColor( m_iRed, m_iGreen, m_iBlue, -1 );

		drawSetTextFont( m_pCaptionFont );
		drawSetTextPos( m_iCaptionX, 0 );
		drawPrintText( m_szCaption, (int)strlen( m_szCaption ) );

		drawSetTextFont( m_pValueFont );
		drawSetTextPos( m_iValueX, m_iValueY );
		drawPrintText( m_szValue, (int)strlen( m_szValue ) );
	}

private:
	vgui::Font *m_pValueFont;
	vgui::Font *m_pCaptionFont;
	int m_iRed;
	int m_iGreen;
	int m_iBlue;
	int m_iValueX;
	int m_iCaptionX;
	int m_iValueY;
	char m_szCaption[24];
	char m_szValue[64];
};

CHudHealthPanel *g_pHudHealthPanel = NULL;
CHudLabeledTextPanel *g_pHudSuitPanel = NULL;
CHudLabeledTextPanel *g_pHudAmmoPanel = NULL;
vgui::Font *g_pHudHealthNumberFont = NULL;
CHudScaledFont *g_pHudHealthScaledFallback = NULL;
vgui::Font *g_pHudOCRBFont = NULL;
vgui::Font *g_pHudHealthCaptionFont = NULL;
int g_iLastHealth = -1;
int g_iLastRed = -1;
int g_iLastGreen = -1;
int g_iLastBlue = -1;

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
		gEngfuncs.Con_DPrintf( "VGUI health: %s not found\\n", fontPath );
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
		gEngfuncs.Con_DPrintf( "VGUI health: loaded OCR-B font from %s\\n", fontPath );

	return font;
}

static vgui::Font *HUD_VGuiHealthLoadOCRBFont( void )
{
	vgui::Font *font;

	// Prefer the real TrueType file. The TGA atlas remains as a compatibility
	// fallback for builds/platforms which do not have the FreeType backend.
	font = HUD_VGuiHealthTryLoadOCRBFont( "resource/ocr-b.ttf", 30 );
	if( font )
		return font;

	font = HUD_VGuiHealthTryLoadOCRBFont( "resource/ocr-b-vgui.tga", 0 );
	if( font )
		return font;

	gEngfuncs.Con_DPrintf( "VGUI health: OCR-B files unavailable; using the VGUI scheme font\\n" );
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
	int panelWide, panelTall;
	int numberX, captionX, numberY;
	int panelX, panelY;
	int oldWide, oldTall;
	int oldX, oldY;
	bool changed = false;

	if( !g_pHudHealthPanel || !g_pHudHealthNumberFont || !g_pHudHealthCaptionFont )
		return 0;

	snprintf( healthText, sizeof( healthText ), "%03d", health );

	if( health != g_iLastHealth )
	{
		g_pHudHealthPanel->SetHealthText( healthText );
		g_iLastHealth = health;
		changed = true;
	}

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

	panelWide = numberWide > captionWide ? numberWide : captionWide;
	numberX = ( panelWide - numberWide ) / 2;
	captionX = ( panelWide - captionWide ) / 2;
	numberY = captionTall + 2;
	panelTall = numberY + numberTall;

	g_pHudHealthPanel->getSize( oldWide, oldTall );
	if( panelWide != oldWide || panelTall != oldTall )
	{
		g_pHudHealthPanel->setSize( panelWide, panelTall );
		changed = true;
	}

	g_pHudHealthPanel->SetTextLayout( numberX, captionX, numberY );

	// Keep the number at the original HUD anchor; only the caption extends above it.
	// Re-evaluate the panel origin if a variable-width health value changes.
	panelX = x - numberX;
	panelY = y - numberY;
	g_pHudHealthPanel->getPos( oldX, oldY );
	if( panelX != oldX || panelY != oldY )
	{
		g_pHudHealthPanel->setPos( panelX, panelY );
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
	int panelWide, panelTall;
	int valueX, captionX, valueY;
	int panelX, panelY;
	int oldWide, oldTall;
	int oldX, oldY;
	bool changed = false;

	if( !panel || !g_pHudHealthNumberFont || !g_pHudHealthCaptionFont )
		return 0;

	changed |= panel->SetText( caption, value );
	changed |= panel->SetTextColor( red, green, blue );

	g_pHudHealthNumberFont->getTextSize( value, valueWide, valueTall );
	g_pHudHealthCaptionFont->getTextSize( caption, captionWide, captionTall );

	if( valueWide < 1 ) valueWide = 1;
	if( valueTall < 1 ) valueTall = 1;
	if( captionWide < 1 ) captionWide = 1;
	if( captionTall < 1 ) captionTall = 1;

	panelWide = valueWide > captionWide ? valueWide : captionWide;
	valueX = rightAlignValue ? panelWide - valueWide : ( panelWide - valueWide ) / 2;
	captionX = ( panelWide - captionWide ) / 2;
	valueY = captionTall + 2;
	panelTall = valueY + valueTall;

	panel->getSize( oldWide, oldTall );
	if( panelWide != oldWide || panelTall != oldTall )
	{
		panel->setSize( panelWide, panelTall );
		changed = true;
	}

	changed |= panel->SetTextLayout( valueX, captionX, valueY );

	// The value keeps its original HUD anchor. Ammo is right-aligned so the
	// final digit stays beside the weapon-specific ammo icon.
	panelX = rightAlignValue ? anchorX - panelWide : anchorX - valueX;
	panelY = anchorY - valueY;
	panel->getPos( oldX, oldY );
	if( panelX != oldX || panelY != oldY )
	{
		panel->setPos( panelX, panelY );
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

	g_pHudAmmoPanel->SetFonts( g_pHudHealthNumberFont, g_pHudHealthCaptionFont );
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

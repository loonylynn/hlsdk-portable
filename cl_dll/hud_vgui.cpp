#include "hud_vgui.h"

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

CHudHealthPanel *g_pHudHealthPanel = NULL;
CHudScaledFont *g_pHudHealthNumberFont = NULL;
vgui::Font *g_pHudHealthCaptionFont = NULL;
int g_iLastHealth = -1;
int g_iLastRed = -1;
int g_iLastGreen = -1;
int g_iLastBlue = -1;
int g_iLastX = -1;
int g_iLastY = -1;

void HUD_VGuiHealthResetCache( void )
{
	g_iLastHealth = -1;
	g_iLastRed = -1;
	g_iLastGreen = -1;
	g_iLastBlue = -1;
	g_iLastX = -1;
	g_iLastY = -1;
}

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

	if( g_pHudHealthNumberFont )
	{
		delete g_pHudHealthNumberFont;
		g_pHudHealthNumberFont = NULL;
	}

	// 150% of the active VGUI primary font; menus keep their original sizing.
	g_pHudHealthNumberFont = new CHudScaledFont( baseNumberFont );

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

	// The viewport owns the scheme fonts; discard our wrapper before it is destroyed.
	if( g_pHudHealthNumberFont )
	{
		delete g_pHudHealthNumberFont;
		g_pHudHealthNumberFont = NULL;
	}

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
	g_iLastX = x;
	g_iLastY = y;

	if( !g_pHudHealthPanel->isVisible() )
	{
		g_pHudHealthPanel->setVisible( true );
		changed = true;
	}

	if( changed )
		g_pHudHealthPanel->repaint();

	return 1;
}

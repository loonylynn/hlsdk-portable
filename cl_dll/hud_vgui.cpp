#include "hud_vgui.h"

#include <stddef.h>
#include <VGUI_Label.h>
#include <VGUI_Scheme.h>

namespace
{

// The HUD is visual-only: it must never take mouse focus away from menus.
class CHudHealthLabel : public vgui::Label
{
public:
	CHudHealthLabel() : vgui::Label( "100", 0, 0 ) {}

	virtual bool isWithin( int x, int y )
	{
		(void)x;
		(void)y;
		return false;
	}
};

CHudHealthLabel *g_pHudHealthLabel = NULL;
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
	if( !parent )
		return;

	if( !g_pHudHealthLabel )
	{
		g_pHudHealthLabel = new CHudHealthLabel();
		g_pHudHealthLabel->setTextAlignment( vgui::Label::a_northwest );
		g_pHudHealthLabel->setContentAlignment( vgui::Label::a_northwest );
		g_pHudHealthLabel->setPaintBorderEnabled( false );
		g_pHudHealthLabel->setPaintBackgroundEnabled( false );
	}

	if( g_pHudHealthLabel->getParent() != parent )
	{
		if( g_pHudHealthLabel->getParent() )
			g_pHudHealthLabel->getParent()->removeChild( g_pHudHealthLabel );

		// Child zero draws beneath viewport menus and dialog panels.
		parent->insertChildAt( g_pHudHealthLabel, 0 );
	}

	// The viewport installs the VGUI scheme in its constructor.
	g_pHudHealthLabel->setFont( vgui::Scheme::sf_primary1 );
	g_pHudHealthLabel->setVisible( false );
	g_pHudHealthLabel->repaint();
	HUD_VGuiHealthResetCache();
}

void HUD_VGuiHealthShutdown( void )
{
	if( !g_pHudHealthLabel )
		return;

	// The parent does not own its child panels. Detach before viewport teardown.
	if( g_pHudHealthLabel->getParent() )
	{
		vgui::Panel *parent = g_pHudHealthLabel->getParent();
		parent->removeChild( g_pHudHealthLabel );
		parent->repaint();
	}

	g_pHudHealthLabel->setVisible( false );
	HUD_VGuiHealthResetCache();
}

void HUD_VGuiHealthSetVisible( int visible )
{
	if( !g_pHudHealthLabel )
		return;

	bool shouldBeVisible = ( visible != 0 );
	if( g_pHudHealthLabel->isVisible() != shouldBeVisible )
	{
		g_pHudHealthLabel->setVisible( shouldBeVisible );
		g_pHudHealthLabel->repaint();
	}
}

int HUD_VGuiHealthUpdate( int health, int red, int green, int blue, int x, int y )
{
	int textWide, textTall;
	int oldWide, oldTall;
	bool changed = false;

	if( !g_pHudHealthLabel )
		return 0;

	if( health != g_iLastHealth )
	{
		g_pHudHealthLabel->setText( "%d", health );
		g_iLastHealth = health;
	}

	if( red != g_iLastRed || green != g_iLastGreen || blue != g_iLastBlue )
	{
		// VGUI1 uses zero transparency for opaque text. RGB already includes
		// the original CHudHealth flash/fade scaling.
		g_pHudHealthLabel->setFgColor( red, green, blue, 0 );
		g_iLastRed = red;
		g_iLastGreen = green;
		g_iLastBlue = blue;
	}

	g_pHudHealthLabel->getTextSize( textWide, textTall );
	if( textWide < 1 ) textWide = 1;
	if( textTall < 1 ) textTall = 1;
	g_pHudHealthLabel->getSize( oldWide, oldTall );
	if( textWide != oldWide || textTall != oldTall )
	{
		g_pHudHealthLabel->setSize( textWide, textTall );
		changed = true;
	}

	if( x != g_iLastX || y != g_iLastY )
	{
		g_pHudHealthLabel->setPos( x, y );
		g_iLastX = x;
		g_iLastY = y;
		changed = true;
	}

	if( !g_pHudHealthLabel->isVisible() )
	{
		g_pHudHealthLabel->setVisible( true );
		changed = true;
	}

	if( changed )
		g_pHudHealthLabel->repaint();

	return 1;
}

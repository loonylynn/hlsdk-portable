#ifndef HUD_VGUI_H
#define HUD_VGUI_H

namespace vgui
{
class Panel;
}

void HUD_VGuiHealthStartup( vgui::Panel *parent );
void HUD_VGuiHealthShutdown( void );
void HUD_VGuiHealthSetVisible( int visible );
int HUD_VGuiHealthUpdate( int health, int red, int green, int blue, int x, int y );


void HUD_VGuiSuitStartup( vgui::Panel *parent );
void HUD_VGuiSuitShutdown( void );
void HUD_VGuiSuitSetVisible( int visible );
int HUD_VGuiSuitUpdate( int battery, int red, int green, int blue, int x, int y );

void HUD_VGuiAmmoStartup( vgui::Panel *parent );
void HUD_VGuiAmmoShutdown( void );
void HUD_VGuiAmmoSetVisible( int visible );
int HUD_VGuiAmmoUpdate( int clip, int reserve, int hasClip,
	int red, int green, int blue, int rightX, int y );

#endif // HUD_VGUI_H

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

#endif // HUD_VGUI_H

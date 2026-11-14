#include "input_mouse.h"
#include "exportdef.h"
#include "hud.h"
#include "cl_util.h"

extern "C" int HUD_AuxPowerEditorIsActive( void );
extern "C" void HUD_AuxPowerEditorMouseMove( float deltaX, float deltaY );
extern "C" void HUD_AuxPowerEditorMouseButtons( int buttons );

// shared between backends
Vector dead_viewangles(0, 0, 0);
cvar_t      *sensitivity;
cvar_t  *in_joystick;

FWGSInput fwgsInput;

#if SUPPORT_GOLDSOURCE_INPUT
GoldSourceInput goldSourceInput;
AbstractInput* currentInput = &goldSourceInput;
#else
AbstractInput* currentInput = &fwgsInput;
#endif
extern "C"  void DLLEXPORT IN_ClientMoveEvent( float forwardmove, float sidemove )
{
	if( HUD_AuxPowerEditorIsActive() )
		return;

	currentInput->IN_ClientMoveEvent(forwardmove, sidemove);
}

extern "C" void DLLEXPORT IN_ClientLookEvent( float relyaw, float relpitch )
{
	if( HUD_AuxPowerEditorIsActive() )
	{
		// Xash passes mouse deltas after applying m_yaw/m_pitch. Undo that
		// scaling so the virtual cursor moves in screen-pixel-like units.
		float yawScale = CVAR_GET_FLOAT( "m_yaw" );
		float pitchScale = CVAR_GET_FLOAT( "m_pitch" );
		if( yawScale > -0.0001f && yawScale < 0.0001f )
			yawScale = 0.022f;
		if( pitchScale > -0.0001f && pitchScale < 0.0001f )
			pitchScale = 0.022f;

		HUD_AuxPowerEditorMouseMove( -relyaw / yawScale, relpitch / pitchScale );
		return;
	}

	currentInput->IN_ClientLookEvent(relyaw, relpitch);
}

void IN_Move( float frametime, usercmd_t *cmd )
{
	currentInput->IN_Move(frametime, cmd);
}

extern "C" void DLLEXPORT IN_MouseEvent( int mstate )
{
	if( HUD_AuxPowerEditorIsActive() )
	{
		HUD_AuxPowerEditorMouseButtons( mstate );
		return;
	}

	currentInput->IN_MouseEvent(mstate);
}

extern "C" void DLLEXPORT IN_ClearStates( void )
{
	currentInput->IN_ClearStates();
}

extern "C" void DLLEXPORT IN_ActivateMouse( void )
{
	currentInput->IN_ActivateMouse();
}

extern "C" void DLLEXPORT IN_DeactivateMouse( void )
{
	currentInput->IN_DeactivateMouse();
}

extern "C" void DLLEXPORT IN_Accumulate( void )
{
	currentInput->IN_Accumulate();
}

void IN_Commands( void )
{
	currentInput->IN_Commands();
}

void IN_Shutdown( void )
{
	currentInput->IN_Shutdown();
}

void IN_Init( void )
{
#if SUPPORT_GOLDSOURCE_INPUT
	if (IsXashFWGS()) {
		gEngfuncs.Con_Printf( "FWGS Xash3D input is in use\n" );
		currentInput = &fwgsInput;
	} else {
		gEngfuncs.Con_Printf( "GoldSource input is in use\n" );
		currentInput = &goldSourceInput;
	}
#else
	currentInput = &fwgsInput;
#endif
	currentInput->IN_Init();
}

void IN_ResetMouse()
{
	currentInput->IN_ResetMouse();
}

void IgnoreNextMouseDelta()
{
	currentInput->IgnoreNextMouseDelta();
}

AbstractInput* CurrentMouseInput()
{
	return currentInput;
}

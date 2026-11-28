#include "hud.h"
#include "usercmd.h"
#include <math.h>
#include "usercmd.h"
#include "cvardef.h"
#include "kbutton.h"
#include "keydefs.h"
#include "input_mouse.h"
extern cvar_t		*sensitivity;
extern cvar_t		*in_joystick;

extern kbutton_t	in_strafe;
extern kbutton_t	in_mlook;
extern kbutton_t	in_speed;
extern kbutton_t	in_jlook;
extern kbutton_t	in_forward;
extern kbutton_t	in_back;
extern kbutton_t	in_moveleft;
extern kbutton_t	in_moveright;

extern cvar_t	*m_pitch;
extern cvar_t	*m_yaw;
extern cvar_t	*m_forward;
extern cvar_t	*m_side;
extern cvar_t	*lookstrafe;
extern cvar_t	*lookspring;
extern cvar_t	*cl_pitchdown;
extern cvar_t	*cl_pitchup;
extern cvar_t	*cl_yawspeed;
extern cvar_t	*cl_sidespeed;
extern cvar_t	*cl_forwardspeed;
extern cvar_t	*cl_pitchspeed;
cvar_t	*cl_laddermode;


#define F 1U<<0	// Forward
#define B 1U<<1	// Back
#define L 1U<<2	// Left
#define R 1U<<3	// Right
#define T 1U<<4	// Forward stop
#define S 1U<<5	// Side stop

#define BUTTON_DOWN		1
#define IMPULSE_DOWN	2
#define IMPULSE_UP		4

int CL_IsDead( void );
extern Vector dead_viewangles;

/*
===========
IN_GetMouseSensitivity
Get mouse sensitivity with sanitization
===========
*/
float IN_GetMouseSensitivity()
{
	// Absurdly high sensitivity values can cause the game to hang, so clamp
	if( sensitivity->value > 10000.0 )
	{
		gEngfuncs.Cvar_SetValue( "sensitivity", 10000.0 );
	}
	else if( sensitivity->value < 0.01 )
	{
		gEngfuncs.Cvar_SetValue( "sensitivity", 0.01 );
	}
	return sensitivity->value;
}

void IN_ToggleButtons( float forwardmove, float sidemove )
{
	static unsigned int moveflags = T | S;

	if( forwardmove )
		moveflags &= ~T;
	else
	{
		//if( in_forward.state || in_back.state ) gEngfuncs.Con_Printf("Buttons pressed f%d b%d\n", in_forward.state, in_back.state);
		if( !( moveflags & T ) )
		{
			//IN_ForwardUp();
			//IN_BackUp();
			//gEngfuncs.Con_Printf("Reset forwardmove state f%d b%d\n", in_forward.state, in_back.state);
			in_forward.state &= ~BUTTON_DOWN;
			in_back.state &= ~BUTTON_DOWN;
			moveflags |= T;
		}
	}
	if( sidemove )
		moveflags &= ~S;
	else
	{
		//gEngfuncs.Con_Printf("l%d r%d\n", in_moveleft.state, in_moveright.state);
		//if( in_moveleft.state || in_moveright.state ) gEngfuncs.Con_Printf("Buttons pressed l%d r%d\n", in_moveleft.state, in_moveright.state);
		if( !( moveflags & S ) )
		{
			//IN_MoverightUp();
			//IN_MoveleftUp();
			//gEngfuncs.Con_Printf("Reset sidemove state f%d b%d\n", in_moveleft.state, in_moveright.state);
			in_moveleft.state &= ~BUTTON_DOWN;
			in_moveright.state &= ~BUTTON_DOWN;
			moveflags |= S;
		}
	}

	if( forwardmove > 0.7f && !( moveflags & F ) )
	{
		moveflags |= F;
		in_forward.state |= BUTTON_DOWN;
	}
	if( forwardmove < 0.7f && ( moveflags & F ) )
	{
		moveflags &= ~F;
		in_forward.state &= ~BUTTON_DOWN;
	}
	if( forwardmove < -0.7f && !( moveflags & B ) )
	{
		moveflags |= B;
		in_back.state |= BUTTON_DOWN;
	}
	if( forwardmove > -0.7f && ( moveflags & B ) )
	{
		moveflags &= ~B;
		in_back.state &= ~BUTTON_DOWN;
	}
	if( sidemove > 0.9f && !( moveflags & R ) )
	{
		moveflags |= R;
		in_moveright.state |= BUTTON_DOWN;
	}
	if( sidemove < 0.9f && ( moveflags & R ) )
	{
		moveflags &= ~R;
		in_moveright.state &= ~BUTTON_DOWN;
	}
	if( sidemove < -0.9f && !( moveflags & L ) )
	{
		moveflags |= L;
		in_moveleft.state |= BUTTON_DOWN;
	}
	if( sidemove > -0.9f && ( moveflags & L ) )
	{
		moveflags &= ~L;
		in_moveleft.state &= ~BUTTON_DOWN;
	}
}

void FWGSInput::IN_ClientMoveEvent( float forwardmove, float sidemove )
{
	//gEngfuncs.Con_Printf("IN_MoveEvent\n");

	ac_forwardmove += forwardmove;
	ac_sidemove += sidemove;
	ac_movecount++;
}


static float s_freeAimYaw = 0.0f;
static float s_freeAimPitch = 0.0f;
static float s_freeAimBlend = 0.0f;

extern "C" int IN_FreeAimIsActive( void );
extern "C" void IN_FreeAimClearButton( void );

extern "C" float IN_FreeAimGetYaw( void ) { return s_freeAimYaw; }
extern "C" float IN_FreeAimGetPitch( void ) { return s_freeAimPitch; }
extern "C" float IN_FreeAimGetBlend( void ) { return s_freeAimBlend; }

void FWGSInput::IN_ClientLookEvent( float relyaw, float relpitch )
{
	rel_yaw += relyaw;
	rel_pitch += relpitch;
}

// Rotate camera and add move values to usercmd
void FWGSInput::IN_Move( float frametime, usercmd_t *cmd )
{
	Vector viewangles;
	bool fLadder = false;

	if( gHUD.m_iIntermission )
		return; // we can't move during intermission

	if( cl_laddermode->value != 2 )
	{
		cl_entity_t *pplayer = gEngfuncs.GetLocalPlayer();
		if( pplayer )
			fLadder = pplayer->curstate.movetype == MOVETYPE_FLY;
	}
	//if(ac_forwardmove || ac_sidemove)
	//gEngfuncs.Con_Printf("Move: %f %f %f %f\n", ac_forwardmove, ac_sidemove, rel_pitch, rel_yaw);
#if 0
	if( in_mlook.state & 1 )
	{
		V_StopPitchDrift();
	}
#endif
	if( CL_IsDead() )
	{
		viewangles = dead_viewangles; // HACKHACK: see below
	}
	else
	{
		gEngfuncs.GetViewAngles( viewangles );
	}
	float mouse_sensitivity = gHUD.GetSensitivity() != 0 ? gHUD.GetSensitivity() : IN_GetMouseSensitivity();
	rel_yaw *= mouse_sensitivity;
	rel_pitch *= mouse_sensitivity;

	// Ease the zoom/recenter envelope with a time-based response. A 0.05s
	// time constant reaches roughly 95% of its target in the requested 0.15s.
	const float freeAimDt = bound( 0.0f, frametime, 0.25f );
	const float freeAimAlpha = 1.0f - expf( -freeAimDt / 0.05f );
	const bool freeAimActive = IN_FreeAimIsActive() != 0;
	s_freeAimBlend += ( ( freeAimActive ? 1.0f : 0.0f ) - s_freeAimBlend ) * freeAimAlpha;

	if( freeAimActive )
	{
		const float candidateYaw = s_freeAimYaw + rel_yaw;
		const float candidatePitch = s_freeAimPitch + rel_pitch;
		s_freeAimYaw = bound( -FREE_AIM_MAX_YAW, candidateYaw, FREE_AIM_MAX_YAW );
		s_freeAimPitch = bound( -FREE_AIM_MAX_PITCH, candidatePitch, FREE_AIM_MAX_PITCH );

		// Mouse movement that crosses the deadzone boundary transfers into
		// camera rotation. At the edge, continued input naturally turns the view.
		viewangles[YAW] += candidateYaw - s_freeAimYaw;
		viewangles[PITCH] += candidatePitch - s_freeAimPitch;
	}
	else
	{
		viewangles[YAW] += rel_yaw;
		viewangles[PITCH] += rel_pitch;
		s_freeAimYaw += ( 0.0f - s_freeAimYaw ) * freeAimAlpha;
		s_freeAimPitch += ( 0.0f - s_freeAimPitch ) * freeAimAlpha;
		if( fabsf( s_freeAimYaw ) < 0.01f ) s_freeAimYaw = 0.0f;
		if( fabsf( s_freeAimPitch ) < 0.01f ) s_freeAimPitch = 0.0f;
	}

	if( fLadder )
	{
		if( cl_laddermode->value == 1 )
			viewangles[YAW] -= ac_sidemove * 5;
		ac_sidemove = 0;
	}
	if( gHUD.m_MOTD.m_bShow )
		gHUD.m_MOTD.scroll += rel_pitch;
	else if( !freeAimActive )
		viewangles[PITCH] += rel_pitch;

	if( viewangles[PITCH] > cl_pitchdown->value )
		viewangles[PITCH] = cl_pitchdown->value;
	if( viewangles[PITCH] < -cl_pitchup->value )
		viewangles[PITCH] = -cl_pitchup->value;
	
	// HACKHACK: change viewangles directly in viewcode, 
	// so viewangles when player is dead will not be changed on server
	if( !CL_IsDead() )
	{
		gEngfuncs.SetViewAngles( viewangles );
	}

	dead_viewangles = viewangles; // keep them actual
	if( ac_movecount )
	{
		IN_ToggleButtons( ac_forwardmove / ac_movecount, ac_sidemove / ac_movecount );

		if( ac_forwardmove )
			cmd->forwardmove = ac_forwardmove * cl_forwardspeed->value / ac_movecount;
		if( ac_sidemove )
			cmd->sidemove  = ac_sidemove * cl_sidespeed->value / ac_movecount;
	}

	// Carry the independent aim angles in Xash's already delta-encoded
	// modder-reserved impact fields. The server validates the tag and weapon.
	if( cmd )
	{
		cmd->impact_index = FREE_AIM_USERCMD_TAG;
		cmd->impact_position[0] = freeAimActive ? s_freeAimYaw : 0.0f;
		cmd->impact_position[1] = freeAimActive ? s_freeAimPitch : 0.0f;
		cmd->impact_position[2] = freeAimActive ? 1.0f : 0.0f;
	}

	ac_sidemove = ac_forwardmove = rel_pitch = rel_yaw = 0;
	ac_movecount = 0;
}

void FWGSInput::IN_MouseEvent( int mstate )
{
	static int mouse_oldbuttonstate;
	// perform button actions
	for( int i = 0; i < 5; i++ )
	{
		if( ( mstate & ( 1 << i ) ) && !( mouse_oldbuttonstate & ( 1 << i ) ) )
		{
			gEngfuncs.Key_Event( K_MOUSE1 + i, 1 );
		}

		if( !( mstate & ( 1 << i ) ) && ( mouse_oldbuttonstate & ( 1 << i ) ) )
		{
			gEngfuncs.Key_Event( K_MOUSE1 + i, 0 );
		}
	}	

	mouse_oldbuttonstate = mstate;
}

// Stubs

void FWGSInput::IN_ClearStates( void )
{
	IN_FreeAimClearButton();
	s_freeAimYaw = s_freeAimPitch = s_freeAimBlend = 0.0f;
}

void FWGSInput::IN_ActivateMouse( void )
{
	//gEngfuncs.Con_Printf( "IN_ActivateMouse\n" );
}

void FWGSInput::IN_DeactivateMouse( void )
{
	//gEngfuncs.Con_Printf( "IN_DeactivateMouse\n" );
}

void FWGSInput::IN_Accumulate( void )
{
	//gEngfuncs.Con_Printf( "IN_Accumulate\n" );
}

void FWGSInput::IN_Commands( void )
{
	//gEngfuncs.Con_Printf( "IN_Commands\n" );
}

void FWGSInput::IN_Shutdown( void )
{
}

// Register cvars and reset data
void FWGSInput::IN_Init( void )
{
	sensitivity = gEngfuncs.pfnRegisterVariable( "sensitivity", "3", FCVAR_ARCHIVE | FCVAR_FILTERSTUFFTEXT );
	in_joystick = gEngfuncs.pfnRegisterVariable( "joystick", "0", FCVAR_ARCHIVE );
	cl_laddermode = gEngfuncs.pfnRegisterVariable( "cl_laddermode", "2", FCVAR_ARCHIVE );
	ac_forwardmove = ac_sidemove = rel_yaw = rel_pitch = 0;
}

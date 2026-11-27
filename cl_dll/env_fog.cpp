#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "triangleapi.h"
#include "env_fog.h"

struct client_env_fog_state_t
{
	int enabled;
	float color[3];
	float startDistance, endDistance;
	float density, maxDensity;
	int radial, skybox;
};

static client_env_fog_state_t g_ClientEnvFog =
{
	0, { 128.0f, 128.0f, 128.0f }, 0.0f, 3500.0f, 0.0003f, 1.0f, 1, 0
};

// Local client-side override for testing the renderer without a map entity.
static client_env_fog_state_t g_ClientEnvFogTest =
{
	1, { 150.0f, 165.0f, 180.0f }, 128.0f, 2200.0f, 0.0008f, 0.9f, 1, 0
};
static int g_ClientEnvFogTestActive = 0;

static float EnvFog_ClientClamp( float value, float minimum, float maximum )
{
	if( value < minimum ) return minimum;
	if( value > maximum ) return maximum;
	return value;
}

void EnvFog_Apply( void )
{
	if( !gEngfuncs.pTriAPI || !gEngfuncs.pTriAPI->Fog || !gEngfuncs.pTriAPI->FogParams )
		return;

	client_env_fog_state_t *state = g_ClientEnvFogTestActive ? &g_ClientEnvFogTest : &g_ClientEnvFog;
	float color[3] = { state->color[0], state->color[1], state->color[2] };
	int fogOptions = TRI_FOG_PARAMS_EXTENDED;
	if( state->skybox ) fogOptions |= TRI_FOG_PARAMS_SKYBOX;
	if( state->radial ) fogOptions |= TRI_FOG_PARAMS_RADIAL;
	int maxDensity = (int)( EnvFog_ClientClamp( state->maxDensity, 0.0f, 1.0f ) * 255.0f + 0.5f );
	fogOptions |= maxDensity << TRI_FOG_PARAMS_MAX_DENSITY_SHIFT;

	gEngfuncs.pTriAPI->FogParams( state->density, fogOptions );
	gEngfuncs.pTriAPI->Fog( color, state->startDistance, state->endDistance, state->enabled ? 1 : 0 );
}

void EnvFog_Clear( void )
{
	g_ClientEnvFog.enabled = 0;
	g_ClientEnvFog.color[0] = 128.0f;
	g_ClientEnvFog.color[1] = 128.0f;
	g_ClientEnvFog.color[2] = 128.0f;
	g_ClientEnvFog.startDistance = 0.0f;
	g_ClientEnvFog.endDistance = 3500.0f;
	g_ClientEnvFog.density = 0.0003f;
	g_ClientEnvFog.maxDensity = 1.0f;
	g_ClientEnvFog.radial = 1;
	g_ClientEnvFog.skybox = 0;
	EnvFog_Apply();
}

int EnvFog_MsgFunc( const char *pszName, int iSize, void *pbuf )
{
	(void)pszName;
	if( !pbuf || iSize < 18 )
		return 0;

	BEGIN_READ( pbuf, iSize );
	g_ClientEnvFog.enabled = READ_BYTE() != 0;
	g_ClientEnvFog.color[0] = (float)READ_BYTE();
	g_ClientEnvFog.color[1] = (float)READ_BYTE();
	g_ClientEnvFog.color[2] = (float)READ_BYTE();
	g_ClientEnvFog.startDistance = (float)READ_LONG() / 8.0f;
	g_ClientEnvFog.endDistance = (float)READ_LONG() / 8.0f;
	g_ClientEnvFog.density = EnvFog_ClientClamp( (float)READ_LONG() / 1000000.0f, 0.0f, 0.1f );
	g_ClientEnvFog.maxDensity = (float)READ_BYTE() / 255.0f;
	int flags = READ_BYTE();
	g_ClientEnvFog.radial = ( flags & 0x01 ) != 0;
	g_ClientEnvFog.skybox = ( flags & 0x02 ) != 0;
	EnvFog_Apply();
	return 1;
}

static void EnvFog_Test_f( void )
{
	int argc = gEngfuncs.Cmd_Argc();
	const char *mode = gEngfuncs.Cmd_Argv( 1 );

	if( argc < 2 || !stricmp( mode, "help" ) )
	{
		ConsolePrint( "env_fog_test on | off | set <r> <g> <b> <start> <end> <density> [maxdensity] [radial] [skybox]\n" );
		ConsolePrint( "Example: env_fog_test set 150 165 180 128 2200 0.0008 0.9 1 0\n" );
		return;
	}

	if( !stricmp( mode, "off" ) )
	{
		g_ClientEnvFogTestActive = 0;
		EnvFog_Apply();
		ConsolePrint( "env_fog_test: local override disabled; restored map fog state\n" );
		return;
	}

	if( !stricmp( mode, "on" ) )
	{
		g_ClientEnvFogTest.enabled = 1;
		g_ClientEnvFogTest.color[0] = 150.0f;
		g_ClientEnvFogTest.color[1] = 165.0f;
		g_ClientEnvFogTest.color[2] = 180.0f;
		g_ClientEnvFogTest.startDistance = 128.0f;
		g_ClientEnvFogTest.endDistance = 2200.0f;
		g_ClientEnvFogTest.density = 0.0008f;
		g_ClientEnvFogTest.maxDensity = 0.9f;
		g_ClientEnvFogTest.radial = 1;
		g_ClientEnvFogTest.skybox = 0;
		g_ClientEnvFogTestActive = 1;
		EnvFog_Apply();
		ConsolePrint( "env_fog_test: default radial fog enabled\n" );
		return;
	}

	if( !stricmp( mode, "set" ) )
	{
		if( argc < 8 )
		{
			ConsolePrint( "env_fog_test: set requires <r> <g> <b> <start> <end> <density>\n" );
			return;
		}

		g_ClientEnvFogTest.enabled = 1;
		g_ClientEnvFogTest.color[0] = EnvFog_ClientClamp( (float)atof( gEngfuncs.Cmd_Argv( 2 ) ), 0.0f, 255.0f );
		g_ClientEnvFogTest.color[1] = EnvFog_ClientClamp( (float)atof( gEngfuncs.Cmd_Argv( 3 ) ), 0.0f, 255.0f );
		g_ClientEnvFogTest.color[2] = EnvFog_ClientClamp( (float)atof( gEngfuncs.Cmd_Argv( 4 ) ), 0.0f, 255.0f );
		g_ClientEnvFogTest.startDistance = EnvFog_ClientClamp( (float)atof( gEngfuncs.Cmd_Argv( 5 ) ), 0.0f, 65534.0f );
		g_ClientEnvFogTest.endDistance = EnvFog_ClientClamp( (float)atof( gEngfuncs.Cmd_Argv( 6 ) ), g_ClientEnvFogTest.startDistance + 1.0f, 65535.0f );
		g_ClientEnvFogTest.density = EnvFog_ClientClamp( (float)atof( gEngfuncs.Cmd_Argv( 7 ) ), 0.0f, 0.1f );
		g_ClientEnvFogTest.maxDensity = argc > 8 ? EnvFog_ClientClamp( (float)atof( gEngfuncs.Cmd_Argv( 8 ) ), 0.0f, 1.0f ) : 1.0f;
		g_ClientEnvFogTest.radial = argc > 9 ? ( atoi( gEngfuncs.Cmd_Argv( 9 ) ) != 0 ) : 1;
		g_ClientEnvFogTest.skybox = argc > 10 ? ( atoi( gEngfuncs.Cmd_Argv( 10 ) ) != 0 ) : 0;
		g_ClientEnvFogTestActive = 1;
		EnvFog_Apply();
		ConsolePrint( "env_fog_test: custom fog override enabled\n" );
		return;
	}

	ConsolePrint( "env_fog_test: unknown option; use 'env_fog_test help'\n" );
}

void EnvFog_RegisterCommands( void )
{
	gEngfuncs.pfnAddCommand( "env_fog_test", EnvFog_Test_f );
}

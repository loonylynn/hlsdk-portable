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

	float color[3] = { g_ClientEnvFog.color[0], g_ClientEnvFog.color[1], g_ClientEnvFog.color[2] };
	int fogOptions = TRI_FOG_PARAMS_EXTENDED;
	if( g_ClientEnvFog.skybox ) fogOptions |= TRI_FOG_PARAMS_SKYBOX;
	if( g_ClientEnvFog.radial ) fogOptions |= TRI_FOG_PARAMS_RADIAL;
	int maxDensity = (int)( EnvFog_ClientClamp( g_ClientEnvFog.maxDensity, 0.0f, 1.0f ) * 255.0f + 0.5f );
	fogOptions |= maxDensity << TRI_FOG_PARAMS_MAX_DENSITY_SHIFT;

	gEngfuncs.pTriAPI->FogParams( g_ClientEnvFog.density, fogOptions );
	gEngfuncs.pTriAPI->Fog( color, g_ClientEnvFog.startDistance, g_ClientEnvFog.endDistance, g_ClientEnvFog.enabled ? 1 : 0 );
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
	if( !pbuf || iSize < 14 )
		return 0;

	BEGIN_READ( pbuf, iSize );
	g_ClientEnvFog.enabled = READ_BYTE() != 0;
	g_ClientEnvFog.color[0] = (float)READ_BYTE();
	g_ClientEnvFog.color[1] = (float)READ_BYTE();
	g_ClientEnvFog.color[2] = (float)READ_BYTE();
	g_ClientEnvFog.startDistance = READ_COORD();
	g_ClientEnvFog.endDistance = READ_COORD();
	g_ClientEnvFog.density = EnvFog_ClientClamp( (float)READ_LONG() / 1000000.0f, 0.0f, 0.1f );
	g_ClientEnvFog.maxDensity = (float)READ_BYTE() / 255.0f;
	int flags = READ_BYTE();
	g_ClientEnvFog.radial = ( flags & 0x01 ) != 0;
	g_ClientEnvFog.skybox = ( flags & 0x02 ) != 0;
	EnvFog_Apply();
	return 1;
}

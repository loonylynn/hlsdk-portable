#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "saverestore.h"
#include "env_fog.h"

#define SF_ENV_FOG_START_ACTIVE 0x0001
#define ENV_FOG_FLAG_RADIAL 0x01
#define ENV_FOG_FLAG_SKYBOX 0x02

struct env_fog_state_t
{
	int enabled;
	int color[3];
	float startDistance;
	float endDistance;
	float density;
	float maxDensity;
	int radial;
	int skybox;
};

static int gmsgEnvFog = 0;
static CBaseEntity *g_pActiveEnvFog = NULL;
static env_fog_state_t g_EnvFogState =
{
	0, { 128, 128, 128 }, 0.0f, 3500.0f, 0.0003f, 1.0f, 1, 0
};

static float EnvFog_Clamp( float value, float minimum, float maximum )
{
	if( value < minimum ) return minimum;
	if( value > maximum ) return maximum;
	return value;
}

static int EnvFog_ClampByte( float value )
{
	return (int)( EnvFog_Clamp( value, 0.0f, 255.0f ) + 0.5f );
}

static void EnvFog_WriteState( int destination, edict_t *pClient )
{
	if( !gmsgEnvFog )
		return;

	MESSAGE_BEGIN( destination, gmsgEnvFog, NULL, pClient );
		WRITE_BYTE( g_EnvFogState.enabled ? 1 : 0 );
		WRITE_BYTE( g_EnvFogState.color[0] );
		WRITE_BYTE( g_EnvFogState.color[1] );
		WRITE_BYTE( g_EnvFogState.color[2] );
		WRITE_COORD( g_EnvFogState.startDistance );
		WRITE_COORD( g_EnvFogState.endDistance );
		WRITE_LONG( (int)( g_EnvFogState.density * 1000000.0f + 0.5f ) );
		WRITE_BYTE( (int)( EnvFog_Clamp( g_EnvFogState.maxDensity, 0.0f, 1.0f ) * 255.0f + 0.5f ) );
		WRITE_BYTE( ( g_EnvFogState.radial ? ENV_FOG_FLAG_RADIAL : 0 ) |
			( g_EnvFogState.skybox ? ENV_FOG_FLAG_SKYBOX : 0 ) );
	MESSAGE_END();
}

class CEnvFog : public CBaseEntity
{
public:
	CEnvFog() :
		m_iColorR( 128 ), m_iColorG( 128 ), m_iColorB( 128 ),
		m_flStartDistance( 0.0f ), m_flEndDistance( 3500.0f ),
		m_flDensity( 0.0003f ), m_flMaxDensity( 1.0f ),
		m_iRadial( TRUE ), m_iSkybox( FALSE ), m_iActive( FALSE ) {}
	void Spawn( void );
	void KeyValue( KeyValueData *pkvd );
	void Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value );
	void Activate( void );
	int ObjectCaps( void ) { return CBaseEntity::ObjectCaps() & ~FCAP_ACROSS_TRANSITION; }
	static TYPEDESCRIPTION m_SaveData[];
private:
	void SetActive( void );
	int m_iColorR, m_iColorG, m_iColorB;
	float m_flStartDistance, m_flEndDistance;
	float m_flDensity, m_flMaxDensity;
	int m_iRadial, m_iSkybox, m_iActive;
};

LINK_ENTITY_TO_CLASS( env_fog, CEnvFog )

TYPEDESCRIPTION CEnvFog::m_SaveData[] =
{
	DEFINE_FIELD( CEnvFog, m_iColorR, FIELD_INTEGER ),
	DEFINE_FIELD( CEnvFog, m_iColorG, FIELD_INTEGER ),
	DEFINE_FIELD( CEnvFog, m_iColorB, FIELD_INTEGER ),
	DEFINE_FIELD( CEnvFog, m_flStartDistance, FIELD_FLOAT ),
	DEFINE_FIELD( CEnvFog, m_flEndDistance, FIELD_FLOAT ),
	DEFINE_FIELD( CEnvFog, m_flDensity, FIELD_FLOAT ),
	DEFINE_FIELD( CEnvFog, m_flMaxDensity, FIELD_FLOAT ),
	DEFINE_FIELD( CEnvFog, m_iRadial, FIELD_INTEGER ),
	DEFINE_FIELD( CEnvFog, m_iSkybox, FIELD_INTEGER ),
	DEFINE_FIELD( CEnvFog, m_iActive, FIELD_INTEGER ),
};

IMPLEMENT_SAVERESTORE( CEnvFog, CBaseEntity )

void CEnvFog::Spawn( void )
{
	pev->solid = SOLID_NOT;
	pev->movetype = MOVETYPE_NONE;
	SetUse( &CEnvFog::Use );
	if( pev->spawnflags & SF_ENV_FOG_START_ACTIVE )
	{
		m_iActive = TRUE;
		SetActive();
	}
}

void CEnvFog::KeyValue( KeyValueData *pkvd )
{
	if( FStrEq( pkvd->szKeyName, "fogcolor" ) || FStrEq( pkvd->szKeyName, "rendercolor" ) || FStrEq( pkvd->szKeyName, "color" ) )
	{
		float r = (float)m_iColorR, g = (float)m_iColorG, b = (float)m_iColorB;
		if( sscanf( pkvd->szValue, "%f %f %f", &r, &g, &b ) == 3 )
		{
			m_iColorR = EnvFog_ClampByte( r );
			m_iColorG = EnvFog_ClampByte( g );
			m_iColorB = EnvFog_ClampByte( b );
		}
		pkvd->fHandled = TRUE;
	}
	else if( FStrEq( pkvd->szKeyName, "fogstart" ) || FStrEq( pkvd->szKeyName, "startdist" ) )
	{
		m_flStartDistance = EnvFog_Clamp( (float)atof( pkvd->szValue ), 0.0f, 65535.0f );
		pkvd->fHandled = TRUE;
	}
	else if( FStrEq( pkvd->szKeyName, "fogend" ) || FStrEq( pkvd->szKeyName, "enddist" ) )
	{
		m_flEndDistance = EnvFog_Clamp( (float)atof( pkvd->szValue ), 1.0f, 65535.0f );
		pkvd->fHandled = TRUE;
	}
	else if( FStrEq( pkvd->szKeyName, "fogdensity" ) || FStrEq( pkvd->szKeyName, "density" ) )
	{
		m_flDensity = EnvFog_Clamp( (float)atof( pkvd->szValue ), 0.0f, 0.1f );
		pkvd->fHandled = TRUE;
	}
	else if( FStrEq( pkvd->szKeyName, "fogmaxdensity" ) || FStrEq( pkvd->szKeyName, "maxdensity" ) )
	{
		m_flMaxDensity = EnvFog_Clamp( (float)atof( pkvd->szValue ), 0.0f, 1.0f );
		pkvd->fHandled = TRUE;
	}
	else if( FStrEq( pkvd->szKeyName, "fog_radial" ) || FStrEq( pkvd->szKeyName, "radial" ) )
	{
		m_iRadial = atoi( pkvd->szValue ) != 0;
		pkvd->fHandled = TRUE;
	}
	else if( FStrEq( pkvd->szKeyName, "fogskybox" ) || FStrEq( pkvd->szKeyName, "fog_enableskybox" ) || FStrEq( pkvd->szKeyName, "skybox" ) )
	{
		m_iSkybox = atoi( pkvd->szValue ) != 0;
		pkvd->fHandled = TRUE;
	}
	else
		CBaseEntity::KeyValue( pkvd );
}

void CEnvFog::SetActive( void )
{
	if( g_pActiveEnvFog && g_pActiveEnvFog != this )
		((CEnvFog *)g_pActiveEnvFog)->m_iActive = FALSE;

	g_pActiveEnvFog = this;
	g_EnvFogState.enabled = TRUE;
	g_EnvFogState.color[0] = m_iColorR;
	g_EnvFogState.color[1] = m_iColorG;
	g_EnvFogState.color[2] = m_iColorB;
	g_EnvFogState.startDistance = m_flStartDistance;
	g_EnvFogState.endDistance = m_flEndDistance;
	if( g_EnvFogState.endDistance <= g_EnvFogState.startDistance )
		g_EnvFogState.endDistance = g_EnvFogState.startDistance + 1.0f;
	g_EnvFogState.density = m_flDensity;
	g_EnvFogState.maxDensity = m_flMaxDensity;
	g_EnvFogState.radial = m_iRadial;
	g_EnvFogState.skybox = m_iSkybox;
	EnvFog_WriteState( MSG_ALL, (edict_t *)NULL );
}

void CEnvFog::Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
{
	int shouldEnable = m_iActive;
	switch( useType )
	{
	case USE_ON: shouldEnable = TRUE; break;
	case USE_OFF: shouldEnable = FALSE; break;
	case USE_TOGGLE: shouldEnable = !m_iActive; break;
	case USE_SET: shouldEnable = value >= 0.5f; break;
	default: return;
	}
	m_iActive = shouldEnable;
	if( shouldEnable )
		SetActive();
	else if( g_pActiveEnvFog == this )
	{
		g_pActiveEnvFog = NULL;
		g_EnvFogState.enabled = FALSE;
		EnvFog_WriteState( MSG_ALL, (edict_t *)NULL );
	}
}

void CEnvFog::Activate( void )
{
	if( m_iActive )
		SetActive();
}

void EnvFog_Init( void )
{
	gmsgEnvFog = REG_USER_MSG( "EnvFog", -1 );
}

void EnvFog_ResetState( void )
{
	g_pActiveEnvFog = NULL;
	g_EnvFogState.enabled = FALSE;
	g_EnvFogState.color[0] = 128;
	g_EnvFogState.color[1] = 128;
	g_EnvFogState.color[2] = 128;
	g_EnvFogState.startDistance = 0.0f;
	g_EnvFogState.endDistance = 3500.0f;
	g_EnvFogState.density = 0.0003f;
	g_EnvFogState.maxDensity = 1.0f;
	g_EnvFogState.radial = TRUE;
	g_EnvFogState.skybox = FALSE;
	EnvFog_WriteState( MSG_ALL, (edict_t *)NULL );
}

void EnvFog_SendToClient( edict_t *pClient )
{
	EnvFog_WriteState( MSG_ONE, pClient );
}

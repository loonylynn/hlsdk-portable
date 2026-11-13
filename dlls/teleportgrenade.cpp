/*
 * Half-Life 2 Episode Three Teleport Grenade - GoldSrc/Xash3D port
 *
 * This implementation deliberately uses the stock Half-Life hand-grenade
 * view/player/world models and animation sequence.  The projectile is a
 * GoldSrc MOVETYPE_BOUNCE entity; no Source SDK or VPhysics interfaces are
 * required.
 *
 * Retained gameplay:
 *   - 1000 u/s launch with the original five-degree upward pre-arc
 *   - teleport on collision except with the owner or the owner's active weapon
 *   - blast/burn-only damage reception without projectile health loss
 *   - safe landing-space search around the impact point
 *   - delayed projectile removal after a successful teleport
 *   - zero player arrival velocity and retained player view angles
 *   - no artificial range or age limit; remove only when leaving the world
 *   - 3-second recharge
 *   - primary-fire revoke window while the projectile is in flight
 *
 * Source-only NPC, physgun and particle-system integrations are intentionally
 * omitted.  The flight ribbon and player route replay are implemented with
 * GoldSrc temp entities and server-authoritative movement.
 */

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include <math.h>

#define TELEPORT_GRENADE_VELOCITY       1000.0f
#define TELEPORT_GRENADE_ARC             5.0f
#define TELEPORT_GRENADE_RECHARGE        3.0f
#define TELEPORT_GRENADE_REVOKE_TIME     0.2f
#define TELEPORT_GRENADE_FLOOR_NORMAL    0.87f
#define TELEPORT_GRENADE_GROUND_TRACE_DOWN 512.0f
#define TELEPORT_GRENADE_GROUND_CLEARANCE 1.0f
#define TELEPORT_GRENADE_MAX_PATH_POINTS 512
#define TELEPORT_GRENADE_PATH_POINT_SPACING 24.0f
#define TELEPORT_GRENADE_PATH_THINK_INTERVAL 0.025f
#define TELEPORT_GRENADE_PLAYER_TRAVEL_SPEED 1800.0f
#define TELEPORT_GRENADE_PLAYER_TRAVEL_MIN_TIME 0.18f
#define TELEPORT_GRENADE_PLAYER_TRAVEL_MAX_TIME 2.5f
#define TELEPORT_GRENADE_TRAIL_LIFE 24
#define TELEPORT_GRENADE_TRAIL_WIDTH 24

enum teleport_grenade_e
{
	TELEPORT_GRENADE_IDLE = 0,
	TELEPORT_GRENADE_FIDGET,
	TELEPORT_GRENADE_PINPULL,
	TELEPORT_GRENADE_THROW1,
	TELEPORT_GRENADE_THROW2,
	TELEPORT_GRENADE_THROW3,
	TELEPORT_GRENADE_HOLSTER,
	TELEPORT_GRENADE_DRAW
};

#if !CLIENT_DLL

static short g_sTeleportTrailSprite = 0;

static short g_sTeleportPortalGlowSprite = 0;
static short g_sTeleportPortalLightningSprite = 0;

static void SendTeleportPortalRing( const Vector &vecOrigin, int iRadius, int iLife, int iWidth, int iNoise, int r, int g, int b, int iBrightness )
{
	if( g_sTeleportTrailSprite <= 0 )
		return;

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, vecOrigin );
		WRITE_BYTE( TE_BEAMTORUS );
		WRITE_COORD( vecOrigin.x );
		WRITE_COORD( vecOrigin.y );
		WRITE_COORD( vecOrigin.z );
		WRITE_COORD( vecOrigin.x );
		WRITE_COORD( vecOrigin.y );
		WRITE_COORD( vecOrigin.z + iRadius );
		WRITE_SHORT( g_sTeleportTrailSprite );
		WRITE_BYTE( 0 );
		WRITE_BYTE( 20 );
		WRITE_BYTE( iLife );
		WRITE_BYTE( iWidth );
		WRITE_BYTE( iNoise );
		WRITE_BYTE( r );
		WRITE_BYTE( g );
		WRITE_BYTE( b );
		WRITE_BYTE( iBrightness );
		WRITE_BYTE( 24 );
	MESSAGE_END();
}


static void SendTeleportBeamFollow( int iEntity, const Vector &vecOrigin, int iLife, int iWidth, int r, int g, int b, int iBrightness )
{
	if( g_sTeleportTrailSprite <= 0 )
		return;

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, vecOrigin );
		WRITE_BYTE( TE_BEAMFOLLOW );
		WRITE_SHORT( iEntity );
		WRITE_SHORT( g_sTeleportTrailSprite );
		WRITE_BYTE( iLife );
		WRITE_BYTE( iWidth );
		WRITE_BYTE( r );
		WRITE_BYTE( g );
		WRITE_BYTE( b );
		WRITE_BYTE( iBrightness );
	MESSAGE_END();
}

class CTeleportProjectile : public CGrenade
{
public:
	int Save( CSave &save );
	int Restore( CRestore &restore );
	static TYPEDESCRIPTION m_SaveData[];

	void Spawn( void );
	void Precache( void );

	void EXPORT ProjectileTouch( CBaseEntity *pOther );
	void EXPORT LimitThink( void );
	void EXPORT DelayedRemoveThink( void );
	void EXPORT PlayerTravelThink( void );

	virtual int TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType );

	void TeleportPerformTeleport( void );
	void TeleportPerformReset( void );

protected:
	BOOL FindTeleportDestination( CBasePlayer *pPlayer, Vector &vecDestination );
	BOOL CanTeleportPlayer( CBasePlayer *pPlayer, const Vector &vecCandidate, Vector &vecDestination );
	BOOL BeginPlayerTravel( CBasePlayer *pPlayer, const Vector &vecDestination );
	void RecordPathPoint( void );
	void CompletePlayerTravel( CBasePlayer *pPlayer );
	void MakeProjectileInert( void );
	void FinishWeaponState( void );
	void SendTeleportEffect( const Vector &vecOrigin );

	Vector m_vecPath[ TELEPORT_GRENADE_MAX_PATH_POINTS ];
	int m_iPathPointCount;
	int m_iTravelMoveType;
	int m_iTravelWasFrozen;
	float m_flTravelStartTime;
	float m_flTravelDuration;
	float m_flTravelPathLength;
};

LINK_ENTITY_TO_CLASS( teleport_projectile, CTeleportProjectile );

void CTeleportProjectile::Precache( void )
{
	PRECACHE_MODEL( "models/grenade.mdl" );
	g_sTeleportTrailSprite = PRECACHE_MODEL( "sprites/laserbeam.spr" );
	g_sTeleportPortalGlowSprite = PRECACHE_MODEL( "sprites/flare6.spr" );
	g_sTeleportPortalLightningSprite = PRECACHE_MODEL( "sprites/lgtning.spr" );

	PRECACHE_SOUND( "weapons/grenade_hit1.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit2.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit3.wav" );
	PRECACHE_SOUND( "buttons/blip1.wav" );
	PRECACHE_SOUND( "items/suitchargeok1.wav" );
}

void CTeleportProjectile::Spawn( void )
{
	Precache();

	m_iPathPointCount = 1;
	m_vecPath[0] = pev->origin;
	m_iTravelMoveType = MOVETYPE_WALK;
	m_iTravelWasFrozen = FALSE;
	m_flTravelStartTime = 0.0f;
	m_flTravelDuration = 0.0f;
	m_flTravelPathLength = 0.0f;

	pev->movetype = MOVETYPE_BOUNCE;
	pev->solid = SOLID_BBOX;
	// GoldSrc has no DAMAGE_EVENTS_ONLY mode. TakeDamage below provides the
	// equivalent behavior by accepting selected damage events without reducing
	// the projectile's health.
	pev->takedamage = DAMAGE_YES;
	pev->health = 1.0f;
	pev->max_health = 1.0f;
	pev->gravity = 1.0f;
	pev->friction = 0.5f;

	SET_MODEL( ENT( pev ), "models/grenade.mdl" );
	UTIL_SetSize( pev, Vector( -4, -4, -4 ), Vector( 4, 4, 4 ) );

	pev->avelocity = Vector(
		RANDOM_FLOAT( -1000.0f, 1000.0f ),
		RANDOM_FLOAT( -1000.0f, 1000.0f ),
		RANDOM_FLOAT( -1000.0f, 1000.0f ) );

	SetTouch( &CTeleportProjectile::ProjectileTouch );
	SetThink( &CTeleportProjectile::LimitThink );
	pev->nextthink = gpGlobals->time + TELEPORT_GRENADE_PATH_THINK_INTERVAL;

	// Additive beam ribbon plus small fading glow sprites along the flight path.
	SendTeleportBeamFollow( entindex(), pev->origin, TELEPORT_GRENADE_TRAIL_LIFE,
		TELEPORT_GRENADE_TRAIL_WIDTH, 255, 140, 32, 235 );
}

int CTeleportProjectile::TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType )
{
	// Match CGrenadeTeleport::OnTakeDamage: only blast and burn damage are
	// accepted. Do not call CBaseMonster::TakeDamage, because its GoldSrc
	// implementation subtracts health and can kill/detonate the projectile.
	if( !( bitsDamageType & ( DMG_BLAST | DMG_BURN ) ) )
		return 0;

	// GoldSrc has no VPhysicsTakeDamage. Approximate its blast response using
	// the stock monster damage-force calculation, but leave health unchanged.
	if( ( bitsDamageType & DMG_BLAST ) && flDamage > 0.0f && !FNullEnt( pevInflictor ) )
	{
		CBaseEntity *pInflictor = CBaseEntity::Instance( pevInflictor );
		if( pInflictor && pInflictor != this )
		{
			Vector vecDirection = pInflictor->Center() - Vector( 0, 0, 10 ) - Center();
			if( vecDirection.Length() > 0.1f )
			{
				vecDirection = vecDirection.Normalize();
				pev->velocity = pev->velocity - vecDirection * DamageForce( flDamage );
			}
		}
	}

	// Accepted damage is an event/physics interaction, not projectile health.
	return 1;
}

void CTeleportProjectile::FinishWeaponState( void )
{
	CBaseEntity *pOwnerEntity = CBaseEntity::Instance( pev->owner );
	if( !pOwnerEntity || !pOwnerEntity->IsPlayer() )
		return;

	CBasePlayer *pPlayer = (CBasePlayer *)pOwnerEntity;

	// m_iClip is unused by this weapon; it is the networked/predicted
	// "projectile currently exists" bit.  Clear it when the entity goes away.
	for( int i = 0; i < MAX_ITEM_TYPES; ++i )
	{
		CBasePlayerItem *pItem = pPlayer->m_rgpPlayerItems[i];
		while( pItem )
		{
			if( pItem->m_iId == WEAPON_TELEPORT )
			{
				((CTeleportGrenade *)pItem)->OnProjectileFinished();
				return;
			}
			pItem = pItem->m_pNext;
		}
	}
}

void CTeleportProjectile::SendTeleportEffect( const Vector &vecOrigin )
{
	// A compact Xen-style rift flash: animated green core, expanding energy
	// rings and short electrical tendrils, inspired by the Vortigaunt arrival.
	const Vector vecPortalOrigin = vecOrigin + Vector( 0, 0, 8 );

	if( g_sTeleportPortalGlowSprite > 0 )
	{
		MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, vecPortalOrigin );
			WRITE_BYTE( TE_SPRITE );
			WRITE_COORD( vecPortalOrigin.x );
			WRITE_COORD( vecPortalOrigin.y );
			WRITE_COORD( vecPortalOrigin.z );
			WRITE_SHORT( g_sTeleportPortalGlowSprite );
			WRITE_BYTE( 10 );
			WRITE_BYTE( 230 );
		MESSAGE_END();
	}

	SendTeleportPortalRing( vecPortalOrigin, 22, 6, 7, 20, 115, 255, 135, 235 );
	SendTeleportPortalRing( vecPortalOrigin, 38, 9, 4, 8, 45, 190, 90, 190 );

	if( g_sTeleportPortalLightningSprite > 0 )
	{
		for( int i = 0; i < 4; ++i )
		{
			const Vector vecArcEnd = vecPortalOrigin + Vector(
				RANDOM_FLOAT( -28.0f, 28.0f ),
				RANDOM_FLOAT( -28.0f, 28.0f ),
				RANDOM_FLOAT( -12.0f, 24.0f ) );

			MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, vecPortalOrigin );
				WRITE_BYTE( TE_BEAMPOINTS );
				WRITE_COORD( vecPortalOrigin.x );
				WRITE_COORD( vecPortalOrigin.y );
				WRITE_COORD( vecPortalOrigin.z );
				WRITE_COORD( vecArcEnd.x );
				WRITE_COORD( vecArcEnd.y );
				WRITE_COORD( vecArcEnd.z );
				WRITE_SHORT( g_sTeleportPortalLightningSprite );
				WRITE_BYTE( 0 );
				WRITE_BYTE( 25 );
				WRITE_BYTE( 3 );
				WRITE_BYTE( 3 );
				WRITE_BYTE( 45 );
				WRITE_BYTE( 105 );
				WRITE_BYTE( 255 );
				WRITE_BYTE( 145 );
				WRITE_BYTE( 225 );
				WRITE_BYTE( 30 );
			MESSAGE_END();
		}
	}

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, vecPortalOrigin );
		WRITE_BYTE( TE_DLIGHT );
		WRITE_COORD( vecPortalOrigin.x );
		WRITE_COORD( vecPortalOrigin.y );
		WRITE_COORD( vecPortalOrigin.z );
		WRITE_BYTE( 8 );
		WRITE_BYTE( 70 );
		WRITE_BYTE( 255 );
		WRITE_BYTE( 105 );
		WRITE_BYTE( 5 );
		WRITE_BYTE( 16 );
	MESSAGE_END();
}

void CTeleportProjectile::DelayedRemoveThink( void )
{
	UTIL_Remove( this );
}

void CTeleportProjectile::RecordPathPoint( void )
{
	if( m_iPathPointCount < 1 )
	{
		m_vecPath[0] = pev->origin;
		m_iPathPointCount = 1;
		return;
	}

	Vector vecLastPoint = m_vecPath[m_iPathPointCount - 1];
	if( ( pev->origin - vecLastPoint ).Length() < TELEPORT_GRENADE_PATH_POINT_SPACING )
		return;

	// Compact the route if it reaches capacity, preserving its launch and
	// latest points while increasing spacing between older samples.
	if( m_iPathPointCount >= TELEPORT_GRENADE_MAX_PATH_POINTS )
	{
		const int iOldCount = m_iPathPointCount;
		int iWrite = 1;

		for( int i = 2; i < iOldCount - 1; i += 2 )
			m_vecPath[iWrite++] = m_vecPath[i];

		m_vecPath[iWrite++] = m_vecPath[iOldCount - 1];
		m_iPathPointCount = iWrite;
		vecLastPoint = m_vecPath[m_iPathPointCount - 1];
	}

	m_vecPath[m_iPathPointCount++] = pev->origin;
}

void CTeleportProjectile::MakeProjectileInert( void )
{
	pev->takedamage = DAMAGE_NO;
	pev->solid = SOLID_NOT;
	pev->movetype = MOVETYPE_NONE;
	pev->effects |= EF_NODRAW;
	pev->velocity = g_vecZero;
	pev->avelocity = g_vecZero;
	SetTouch( NULL );
}

void CTeleportProjectile::CompletePlayerTravel( CBasePlayer *pPlayer )
{
	if( !pPlayer )
		return;

	UTIL_SetOrigin( pPlayer->pev, m_vecPath[m_iPathPointCount - 1] );
	pPlayer->pev->velocity = g_vecZero;
	pPlayer->pev->flags &= ~FL_ONGROUND;
	pPlayer->pev->groundentity = NULL;
	pPlayer->pev->effects |= EF_NOINTERP;

	pPlayer->pev->movetype = m_iTravelMoveType;
	if( !m_iTravelWasFrozen )
		pPlayer->pev->flags &= ~FL_FROZEN;

	SendTeleportEffect( pPlayer->pev->origin );
	EMIT_SOUND( ENT( pPlayer->pev ), CHAN_ITEM, "items/suitchargeok1.wav", 1.0f, ATTN_NORM );
	UTIL_ScreenShake( pPlayer->pev->origin, 15.0f, 150.0f, 1.0f, 384.0f );

	SetThink( &CTeleportProjectile::DelayedRemoveThink );
	pev->nextthink = gpGlobals->time + 0.1f;
}

BOOL CTeleportProjectile::CanTeleportPlayer( CBasePlayer *pPlayer, const Vector &vecCandidate, Vector &vecDestination )
{
	TraceResult trGround;
	const int iHull = ( pPlayer->pev->flags & FL_DUCKING ) ? head_hull : human_hull;

	// Resolve the actual floor height at this candidate. The grenade can hit
	// walls or ceilings well above the floor, so trace farther down from the
	// candidate itself. Starting above the grenade can put the trace inside a
	// ceiling and incorrectly reject an otherwise valid landing position.
	const Vector vecTraceStart = vecCandidate;
	const Vector vecTraceEnd = vecCandidate - Vector( 0, 0, TELEPORT_GRENADE_GROUND_TRACE_DOWN );

	// Monsters and players must not be mistaken for supporting ground.
	UTIL_TraceLine( vecTraceStart, vecTraceEnd, ignore_monsters, edict(), &trGround );
	if( trGround.fStartSolid || trGround.fAllSolid || trGround.flFraction >= 1.0f )
		return FALSE;

	// Reject walls and excessively steep surfaces.  Each candidate gets its
	// own floor trace, allowing nearby stairs and uneven ground to resolve
	// to their actual local height.
	if( trGround.vecPlaneNormal.z < TELEPORT_GRENADE_FLOOR_NORMAL )
		return FALSE;

	// GoldSrc player origins are centred in their collision hulls:
	// standing is 72 units tall (-36..+36), ducking is 36 (-18..+18).
	// Convert the floor contact point into the matching player-origin height.
	const float flHullHalfHeight = ( pPlayer->pev->flags & FL_DUCKING ) ? 18.0f : 36.0f;
	vecDestination = trGround.vecEndPos + Vector( 0, 0, flHullHalfHeight + TELEPORT_GRENADE_GROUND_CLEARANCE );

	// Test the complete player hull at the resolved feet position.  This
	// catches walls, other actors, low ceilings and other landing obstructions.
	// The projectile itself is ignored because it is removed on success.
	TraceResult trHull;
	UTIL_TraceHull( vecDestination, vecDestination, dont_ignore_monsters, iHull, edict(), &trHull );
	if( trHull.fStartSolid || trHull.fAllSolid )
		return FALSE;

	return TRUE;
}

BOOL CTeleportProjectile::FindTeleportDestination( CBasePlayer *pPlayer, Vector &vecDestination )
{
	Vector vecAway = pPlayer->pev->origin - pev->origin;
	vecAway.z = 0.0f;

	if( vecAway.Length() > 0.1f )
		vecAway = vecAway.Normalize();
	else
		vecAway = Vector( 1, 0, 0 );

	// Search outward from the impact point, preferring the direction away
	// from the owner.  Every candidate is independently grounded and checked
	// against the owner's current standing/crouching hull.
	const float flCandidateRadii[] = { 20.0f, 36.0f, 52.0f };
	const float flRadiansPerDegree = 0.017453292519943295f;

	for( int iRadius = 0; iRadius < 3; ++iRadius )
	{
		const Vector vecSearch = vecAway * flCandidateRadii[iRadius];

		// Try the direction away from the player first, then sweep around it.
		for( int i = 0; i < 8; ++i )
		{
			const float flYaw = (float)i * 45.0f;
			const float flRadians = flYaw * flRadiansPerDegree;
			const float flCos = cosf( flRadians );
			const float flSin = sinf( flRadians );

			Vector vecOffset;
			vecOffset.x = vecSearch.x * flCos - vecSearch.y * flSin;
			vecOffset.y = vecSearch.x * flSin + vecSearch.y * flCos;
			vecOffset.z = 0.0f;

			const Vector vecCandidate = pev->origin + vecOffset;
			if( CanTeleportPlayer( pPlayer, vecCandidate, vecDestination ) )
				return TRUE;
		}
	}

	return FALSE;
}

BOOL CTeleportProjectile::BeginPlayerTravel( CBasePlayer *pPlayer, const Vector &vecDestination )
{
	if( !pPlayer || m_iPathPointCount < 2 )
		return FALSE;

	const Vector vecStartOffset = pPlayer->pev->origin - m_vecPath[0];
	const Vector vecEndOffset = vecDestination - m_vecPath[m_iPathPointCount - 1];
	const float flLastIndex = (float)( m_iPathPointCount - 1 );

	// Convert the grenade's recorded route into player-origin waypoints. The
	// offset blends from the thrower's origin at launch to the safe landing
	// origin at impact, keeping the final hull position exact.
	for( int i = 0; i < m_iPathPointCount; ++i )
	{
		const float flFraction = (float)i / flLastIndex;
		const Vector vecOffset = vecStartOffset + ( vecEndOffset - vecStartOffset ) * flFraction;
		m_vecPath[i] = m_vecPath[i] + vecOffset;
	}

	m_vecPath[0] = pPlayer->pev->origin;
	m_vecPath[m_iPathPointCount - 1] = vecDestination;

	m_flTravelPathLength = 0.0f;
	for( int i = 1; i < m_iPathPointCount; ++i )
		m_flTravelPathLength += ( m_vecPath[i] - m_vecPath[i - 1] ).Length();

	if( m_flTravelPathLength < 1.0f )
		return FALSE;

	m_iTravelMoveType = pPlayer->pev->movetype;
	m_iTravelWasFrozen = ( pPlayer->pev->flags & FL_FROZEN ) ? TRUE : FALSE;
	m_flTravelDuration = m_flTravelPathLength / TELEPORT_GRENADE_PLAYER_TRAVEL_SPEED;

	if( m_flTravelDuration < TELEPORT_GRENADE_PLAYER_TRAVEL_MIN_TIME )
		m_flTravelDuration = TELEPORT_GRENADE_PLAYER_TRAVEL_MIN_TIME;
	else if( m_flTravelDuration > TELEPORT_GRENADE_PLAYER_TRAVEL_MAX_TIME )
		m_flTravelDuration = TELEPORT_GRENADE_PLAYER_TRAVEL_MAX_TIME;

	m_flTravelStartTime = gpGlobals->time;

	// Lock normal movement while the server moves the player along the route.
	pPlayer->pev->velocity = g_vecZero;
	pPlayer->pev->flags |= FL_FROZEN;
	pPlayer->pev->flags &= ~FL_ONGROUND;
	pPlayer->pev->groundentity = NULL;
	pPlayer->pev->movetype = MOVETYPE_NONE;
	pPlayer->pev->effects |= EF_NOINTERP;

	MakeProjectileInert();
	SetThink( &CTeleportProjectile::PlayerTravelThink );
	pev->nextthink = gpGlobals->time + 0.01f;
	return TRUE;
}

void CTeleportProjectile::PlayerTravelThink( void )
{
	CBaseEntity *pOwnerEntity = CBaseEntity::Instance( pev->owner );
	if( !pOwnerEntity || !pOwnerEntity->IsPlayer() )
	{
		TeleportPerformReset();
		return;
	}

	CBasePlayer *pPlayer = (CBasePlayer *)pOwnerEntity;
	if( pPlayer->pev->deadflag != DEAD_NO )
	{
		TeleportPerformReset();
		return;
	}

	float flProgress = ( gpGlobals->time - m_flTravelStartTime ) / m_flTravelDuration;
	if( flProgress >= 1.0f )
	{
		CompletePlayerTravel( pPlayer );
		return;
	}
	if( flProgress < 0.0f )
		flProgress = 0.0f;

	const float flTargetDistance = m_flTravelPathLength * flProgress;
	float flDistance = 0.0f;
	Vector vecTarget = m_vecPath[m_iPathPointCount - 1];

	for( int i = 1; i < m_iPathPointCount; ++i )
	{
		const Vector vecSegment = m_vecPath[i] - m_vecPath[i - 1];
		const float flSegmentLength = vecSegment.Length();

		if( flTargetDistance <= flDistance + flSegmentLength || i == m_iPathPointCount - 1 )
		{
			if( flSegmentLength > 0.001f )
			{
				const float flSegmentProgress = ( flTargetDistance - flDistance ) / flSegmentLength;
				vecTarget = m_vecPath[i - 1] + vecSegment * flSegmentProgress;
			}
			break;
		}

		flDistance += flSegmentLength;
	}

	pPlayer->pev->velocity = g_vecZero;
	pPlayer->pev->effects |= EF_NOINTERP;
	UTIL_SetOrigin( pPlayer->pev, vecTarget );
	pev->nextthink = gpGlobals->time + 0.01f;
}

void CTeleportProjectile::TeleportPerformTeleport( void )
{
	CBaseEntity *pOwnerEntity = CBaseEntity::Instance( pev->owner );
	if( !pOwnerEntity || !pOwnerEntity->IsPlayer() )
	{
		TeleportPerformReset();
		return;
	}

	CBasePlayer *pPlayer = (CBasePlayer *)pOwnerEntity;
	Vector vecDestination;

	if( !FindTeleportDestination( pPlayer, vecDestination ) )
	{
		// No safe player-sized landing spot: the projectile fizzles.
		TeleportPerformReset();
		return;
	}

	RecordPathPoint();
	const Vector vecOldOrigin = pPlayer->pev->origin;

	FinishWeaponState();
	SendTeleportEffect( vecOldOrigin );

	if( BeginPlayerTravel( pPlayer, vecDestination ) )
		return;

	// If the projectile barely moved, keep the safe instant-teleport fallback.
	UTIL_SetOrigin( pPlayer->pev, vecDestination );
	pPlayer->pev->velocity = g_vecZero;
	pPlayer->pev->flags &= ~FL_ONGROUND;
	pPlayer->pev->groundentity = NULL;
	pPlayer->pev->effects |= EF_NOINTERP;

	SendTeleportEffect( vecDestination );
	EMIT_SOUND( ENT( pPlayer->pev ), CHAN_ITEM, "items/suitchargeok1.wav", 1.0f, ATTN_NORM );
	UTIL_ScreenShake( vecDestination, 15.0f, 150.0f, 1.0f, 384.0f );

	MakeProjectileInert();
	SetThink( &CTeleportProjectile::DelayedRemoveThink );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CTeleportProjectile::TeleportPerformReset( void )
{
	FinishWeaponState();
	SendTeleportEffect( pev->origin );
	EMIT_SOUND( ENT( pev ), CHAN_ITEM, "buttons/blip1.wav", 0.8f, ATTN_NORM );
	UTIL_Remove( this );
}

void CTeleportProjectile::ProjectileTouch( CBaseEntity *pOther )
{
	if( !pOther || pOther->edict() == pev->owner )
		return;

	// VPhysicsCollision in the original ignores the thrower and the weapon
	// currently held by the thrower.  GoldSrc Touch needs these exclusions
	// explicitly because the projectile is not using VPhysics.
	if( pOther->pev->solid == SOLID_TRIGGER )
		return;

	CBaseEntity *pOwnerEntity = CBaseEntity::Instance( pev->owner );
	if( pOwnerEntity && pOwnerEntity->IsPlayer() )
	{
		CBasePlayer *pPlayer = (CBasePlayer *)pOwnerEntity;
		if( pPlayer->m_pActiveItem && pOther == pPlayer->m_pActiveItem )
			return;
	}

	TraceResult tr = UTIL_GetGlobalTrace();
	if( tr.fStartSolid || tr.flFraction == 1.0f )
		return;

	// The original projectile teleports on every collision except the two
	// ignored entities above; walls are not bounce surfaces.
	TeleportPerformTeleport();
}

void CTeleportProjectile::LimitThink( void )
{
	// The original grenade has no fixed fuse or distance leash. Keep only a
	// GoldSrc safety check so a projectile that leaves the playable world (or
	// exceeds the engine's valid velocity bounds) cannot remain orphaned.
	if( !IsInWorld() )
	{
		TeleportPerformReset();
		return;
	}

	CBaseEntity *pOwnerEntity = CBaseEntity::Instance( pev->owner );
	if( !pOwnerEntity || !pOwnerEntity->IsPlayer() )
	{
		TeleportPerformReset();
		return;
	}

	RecordPathPoint();
	pev->nextthink = gpGlobals->time + TELEPORT_GRENADE_PATH_THINK_INTERVAL;
}

TYPEDESCRIPTION CTeleportProjectile::m_SaveData[] =
{
	DEFINE_ARRAY( CTeleportProjectile, m_vecPath, FIELD_VECTOR, TELEPORT_GRENADE_MAX_PATH_POINTS ),
	DEFINE_FIELD( CTeleportProjectile, m_iPathPointCount, FIELD_INTEGER ),
	DEFINE_FIELD( CTeleportProjectile, m_iTravelMoveType, FIELD_INTEGER ),
	DEFINE_FIELD( CTeleportProjectile, m_iTravelWasFrozen, FIELD_INTEGER ),
	DEFINE_FIELD( CTeleportProjectile, m_flTravelStartTime, FIELD_TIME ),
	DEFINE_FIELD( CTeleportProjectile, m_flTravelDuration, FIELD_FLOAT ),
	DEFINE_FIELD( CTeleportProjectile, m_flTravelPathLength, FIELD_FLOAT ),
	// Think/touch callbacks are stored in entvars by the engine; there are no
	// separate C++ function-pointer members to register in this save table.
};

IMPLEMENT_SAVERESTORE( CTeleportProjectile, CGrenade )

static CTeleportProjectile *FindTeleportProjectile( CBasePlayer *pPlayer )
{
	CBaseEntity *pEntity = NULL;

	while( ( pEntity = UTIL_FindEntityByClassname( pEntity, "teleport_projectile" ) ) != NULL )
	{
		if( pEntity->pev->owner == pPlayer->edict() )
			return (CTeleportProjectile *)pEntity;
	}

	return NULL;
}

#endif // !CLIENT_DLL

LINK_ENTITY_TO_CLASS( weapon_teleport, CTeleportGrenade );

void CTeleportGrenade::Spawn( void )
{
	Precache();

	m_iId = WEAPON_TELEPORT;
	SET_MODEL( ENT( pev ), "models/w_grenade.mdl" );
	m_iDefaultAmmo = 0;
	m_iClip = WEAPON_NOCLIP;
	m_fInSpecialReload = FALSE;
	m_iProjectileActive = FALSE;
	m_iPrimaryAmmoType = 0;
	m_iSecondaryAmmoType = 0;

	FallInit();
}

void CTeleportGrenade::Precache( void )
{
	PRECACHE_MODEL( "models/w_grenade.mdl" );
	PRECACHE_MODEL( "models/v_grenade.mdl" );
	PRECACHE_MODEL( "models/p_grenade.mdl" );

#if !CLIENT_DLL
	UTIL_PrecacheOther( "teleport_projectile" );
#endif
}

int CTeleportGrenade::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	// Teleportation is governed by its recharge timer, not ammunition.
	// No ammo type prevents a misleading reserve count in the stock HUD.
	p->pszAmmo1 = NULL;
	p->iMaxAmmo1 = WEAPON_NOCLIP;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 4;
	p->iPosition = 1;
	p->iId = m_iId = WEAPON_TELEPORT;
	p->iWeight = 7;
	p->iFlags = ITEM_FLAG_LIMITINWORLD;

	return 1;
}


int CTeleportGrenade::AddToPlayer( CBasePlayer *pPlayer )
{
	const int iResult = CBasePlayerWeapon::AddToPlayer( pPlayer );

	// GoldSrc clientdata carries ammo indices even for ammo-less weapons.
	// Use the reserved zero slot rather than leaving an invalid -1 index.
	m_iPrimaryAmmoType = 0;
	m_iSecondaryAmmoType = 0;

	return iResult;
}

int CTeleportGrenade::UpdateClientData( CBasePlayer *pPlayer )
{
	// This weapon has no clip. Never expose a projectile-state sentinel as
	// ammunition through the stock CurWeapon message.
	m_iClip = WEAPON_NOCLIP;
	return CBasePlayerWeapon::UpdateClientData( pPlayer );
}

BOOL CTeleportGrenade::IsUseable( void )
{
	return TRUE;
}

BOOL CTeleportGrenade::Deploy( void )
{
	m_fInSpecialReload = FALSE;
	return DefaultDeploy( "models/v_grenade.mdl", "models/p_grenade.mdl", TELEPORT_GRENADE_DRAW, "crowbar" );
}

BOOL CTeleportGrenade::CanHolster( void )
{
	return !m_fInSpecialReload;
}

void CTeleportGrenade::Holster( int skiplocal )
{
	m_fInSpecialReload = FALSE;
	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5f;
	SendWeaponAnim( TELEPORT_GRENADE_HOLSTER, skiplocal );
}

void CTeleportGrenade::PrimaryAttack( void )
{
	const float flTime = UTIL_WeaponTimeBase();

	if( m_fInSpecialReload )
	{
		if( flTime >= m_flNextPrimaryAttack )
			LaunchTeleportProjectile();
		return;
	}

	if( m_iProjectileActive )
	{
		if( flTime >= m_flNextPrimaryAttack )
		{
#if !CLIENT_DLL
			CTeleportProjectile *pProjectile = FindTeleportProjectile( m_pPlayer );
			if( pProjectile )
				pProjectile->TeleportPerformReset();
#endif
			OnProjectileFinished();
			m_flNextPrimaryAttack = GetNextAttackDelay( TELEPORT_GRENADE_REVOKE_TIME );
		}
		return;
	}

	if( flTime < m_flNextSecondaryAttack )
		return;

	m_fInSpecialReload = TRUE;
	SendWeaponAnim( TELEPORT_GRENADE_PINPULL );
	m_flNextPrimaryAttack = GetNextAttackDelay( 0.45f );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.45f;
}

void CTeleportGrenade::LaunchTeleportProjectile( void )
{
	if( !m_pPlayer )
	{
		m_fInSpecialReload = FALSE;
		m_iProjectileActive = FALSE;
		return;
	}

	m_fInSpecialReload = FALSE;

	Vector angThrow = m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle;
	angThrow.x -= TELEPORT_GRENADE_ARC;

	UTIL_MakeVectors( angThrow );

	Vector vecSource = m_pPlayer->pev->origin
		+ m_pPlayer->pev->view_ofs
		+ gpGlobals->v_forward * 16.0f;

	Vector vecVelocity = gpGlobals->v_forward * TELEPORT_GRENADE_VELOCITY
		+ m_pPlayer->pev->velocity;

#if !CLIENT_DLL
	CBaseEntity *pProjectile = CBaseEntity::Create(
		"teleport_projectile",
		vecSource,
		angThrow,
		m_pPlayer->edict() );

	if( !pProjectile )
	{
		m_iProjectileActive = FALSE;
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.5f;
		SendWeaponAnim( TELEPORT_GRENADE_DRAW );
		return;
	}

	pProjectile->pev->velocity = vecVelocity;
	pProjectile->pev->avelocity = Vector(
		RANDOM_FLOAT( -1000.0f, 1000.0f ),
		RANDOM_FLOAT( -1000.0f, 1000.0f ),
		RANDOM_FLOAT( -1000.0f, 1000.0f ) );
#endif

	// Keep the actual no-clip value in m_iClip.  This state is separately
	// transferred through weapon_data_t::iuser4 for prediction.
	m_iProjectileActive = TRUE;
	SendWeaponAnim( TELEPORT_GRENADE_THROW3 );
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	m_flNextPrimaryAttack = GetNextAttackDelay( TELEPORT_GRENADE_REVOKE_TIME );
	m_flNextSecondaryAttack = GetNextAttackDelay( TELEPORT_GRENADE_RECHARGE );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.5f;
}

void CTeleportGrenade::OnProjectileFinished( void )
{
	m_iProjectileActive = FALSE;
}

void CTeleportGrenade::WeaponIdle( void )
{
	if( m_fInSpecialReload && UTIL_WeaponTimeBase() >= m_flNextPrimaryAttack )
	{
		LaunchTeleportProjectile();
		return;
	}

	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() )
		return;

	if( UTIL_SharedRandomFloat( m_pPlayer->random_seed, 0.0f, 1.0f ) <= 0.75f )
	{
		SendWeaponAnim( TELEPORT_GRENADE_IDLE );
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase()
			+ UTIL_SharedRandomFloat( m_pPlayer->random_seed, 10.0f, 15.0f );
	}
	else
	{
		SendWeaponAnim( TELEPORT_GRENADE_FIDGET );
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 75.0f / 30.0f;
	}
}

#if !CLIENT_DLL
TYPEDESCRIPTION CTeleportGrenade::m_SaveData[] =
{
	DEFINE_FIELD( CTeleportGrenade, m_iProjectileActive, FIELD_INTEGER ),
};

IMPLEMENT_SAVERESTORE( CTeleportGrenade, CBasePlayerWeapon )
#endif

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
 *   - wall/obstacle bouncing with velocity damping
 *   - teleport when the projectile contacts a sufficiently flat surface
 *   - safe landing-space search around the impact point
 *   - 30% projectile-velocity inheritance and retained player view angles
 *   - 1024-unit range limit, 3.5-second lifetime and 3-second recharge
 *   - primary-fire revoke window while the projectile is in flight
 *
 * Source-only NPC, physgun and particle-system integrations are intentionally
 * omitted.  The path-replay effect is also omitted; the actual teleport and
 * collision rules remain server authoritative.
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
#define TELEPORT_GRENADE_MAX_DISTANCE  1024.0f
#define TELEPORT_GRENADE_MAX_LIFETIME    3.5f
#define TELEPORT_GRENADE_RECHARGE        3.0f
#define TELEPORT_GRENADE_REVOKE_TIME     0.2f
#define TELEPORT_GRENADE_BOUNCE_SCALE    0.5f
#define TELEPORT_GRENADE_BOUNCE_DOWN     3.0f
#define TELEPORT_GRENADE_VELOCITY_SCALE  0.3f
#define TELEPORT_GRENADE_SEARCH_RADIUS  30.0f
#define TELEPORT_GRENADE_FLOOR_NORMAL    0.87f

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

class CTeleportProjectile : public CGrenade
{
public:
	void Spawn( void );
	void Precache( void );

	void EXPORT ProjectileTouch( CBaseEntity *pOther );
	void EXPORT LimitThink( void );

	void TeleportPerformTeleport( void );
	void TeleportPerformReset( void );

protected:
	BOOL FindTeleportDestination( CBasePlayer *pPlayer, Vector &vecDestination );
	BOOL CanTeleportPlayer( CBasePlayer *pPlayer, const Vector &vecStart, const Vector &vecOffset );
	void FinishWeaponState( void );
	void SendTeleportEffect( const Vector &vecOrigin );
};

LINK_ENTITY_TO_CLASS( teleport_projectile, CTeleportProjectile );

void CTeleportProjectile::Precache( void )
{
	PRECACHE_MODEL( "models/grenade.mdl" );

	PRECACHE_SOUND( "weapons/grenade_hit1.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit2.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit3.wav" );
	PRECACHE_SOUND( "buttons/blip1.wav" );
	PRECACHE_SOUND( "items/suitchargeok1.wav" );
}

void CTeleportProjectile::Spawn( void )
{
	Precache();

	pev->movetype = MOVETYPE_BOUNCE;
	pev->solid = SOLID_BBOX;
	pev->gravity = 1.0f;
	pev->friction = 0.5f;

	SET_MODEL( ENT( pev ), "models/grenade.mdl" );
	UTIL_SetSize( pev, Vector( -4, -4, -4 ), Vector( 4, 4, 4 ) );

	pev->avelocity = Vector(
		RANDOM_FLOAT( -1000.0f, 1000.0f ),
		RANDOM_FLOAT( -1000.0f, 1000.0f ),
		RANDOM_FLOAT( -1000.0f, 1000.0f ) );

	// vuser1 stores the owner's launch origin.  dmgtime is the hard lifetime.
	pev->vuser1 = pev->origin;
	pev->dmgtime = gpGlobals->time + TELEPORT_GRENADE_MAX_LIFETIME;

	SetTouch( &CTeleportProjectile::ProjectileTouch );
	SetThink( &CTeleportProjectile::LimitThink );
	pev->nextthink = gpGlobals->time + 0.1f;
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
	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, vecOrigin );
		WRITE_BYTE( TE_TELEPORT );
		WRITE_COORD( vecOrigin.x );
		WRITE_COORD( vecOrigin.y );
		WRITE_COORD( vecOrigin.z );
	MESSAGE_END();
}

BOOL CTeleportProjectile::CanTeleportPlayer( CBasePlayer *pPlayer, const Vector &vecStart, const Vector &vecOffset )
{
	TraceResult tr;
	const Vector vecEnd = vecStart + vecOffset;
	const int iHull = ( pPlayer->pev->flags & FL_DUCKING ) ? head_hull : human_hull;

	// Reject offsets that cross solid geometry.
	UTIL_TraceLine( vecStart, vecEnd, dont_ignore_monsters, edict(), &tr );
	if( tr.fStartSolid || tr.fAllSolid || tr.flFraction < 1.0f )
		return FALSE;

	// Then test the complete player hull at the destination.  The teleported
	// player's own hull is ignored; other players/monsters still block it.
	UTIL_TraceHull( vecEnd, vecEnd, dont_ignore_monsters, iHull, edict(), &tr );
	if( tr.fStartSolid || tr.fAllSolid )
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

	// Prefer a small step away from the impact point, just as the Source
	// implementation does before trying its wider set of candidate offsets.
	if( CanTeleportPlayer( pPlayer, pev->origin, vecAway ) )
	{
		vecDestination = pev->origin + vecAway;
		return TRUE;
	}

	const Vector vecSearch = vecAway * TELEPORT_GRENADE_SEARCH_RADIUS;
	const float flRadiansPerDegree = 0.017453292519943295f;

	for( int iZ = 1; iZ >= 0; --iZ )
	{
		for( int i = 1; i < 9; ++i )
		{
			const float flYaw = (float)( i / 2 ) * 45.0f * ( i % 2 ? 1.0f : -1.0f );
			const float flRadians = flYaw * flRadiansPerDegree;
			const float flCos = cosf( flRadians );
			const float flSin = sinf( flRadians );

			Vector vecOffset;
			vecOffset.x = vecSearch.x * flCos - vecSearch.y * flSin;
			vecOffset.y = vecSearch.x * flSin + vecSearch.y * flCos;
			vecOffset.z = 12.0f * (float)iZ;

			if( CanTeleportPlayer( pPlayer, pev->origin, vecOffset ) )
			{
				vecDestination = pev->origin + vecOffset;
				return TRUE;
			}
		}
	}

	return FALSE;
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

	const Vector vecOldOrigin = pPlayer->pev->origin;
	const Vector vecExitVelocity = pev->velocity * TELEPORT_GRENADE_VELOCITY_SCALE;

	FinishWeaponState();

	SendTeleportEffect( vecOldOrigin );
	SendTeleportEffect( vecDestination );

	UTIL_SetOrigin( pPlayer->pev, vecDestination );
	pPlayer->pev->velocity = vecExitVelocity;
	pPlayer->pev->flags &= ~FL_ONGROUND;
	pPlayer->pev->groundentity = NULL;
	pPlayer->pev->effects |= EF_NOINTERP;

	// The Source weapon preserves the player's current view angles by default.
	EMIT_SOUND( ENT( pPlayer->pev ), CHAN_ITEM, "items/suitchargeok1.wav", 1.0f, ATTN_NORM );
	UTIL_ScreenShake( vecDestination, 15.0f, 150.0f, 1.0f, 384.0f );

	UTIL_Remove( this );
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

	// Ignore trigger-only entities; the Source projectile also passes through
	// trigger volumes that cannot take damage.
	if( pOther->pev->solid == SOLID_TRIGGER )
		return;

	TraceResult tr = UTIL_GetGlobalTrace();
	if( tr.fStartSolid || tr.flFraction == 1.0f )
		return;

	if( tr.vecPlaneNormal.z > TELEPORT_GRENADE_FLOOR_NORMAL )
	{
		EMIT_SOUND( ENT( pev ), CHAN_ITEM, "buttons/blip1.wav", 0.8f, ATTN_NORM );
		TeleportPerformTeleport();
		return;
	}

	// MOVETYPE_BOUNCE has already reflected the velocity at the collision.
	// Apply the Source weapon's extra damping and downward bias.
	Vector vecVelocity = pev->velocity;
	const float flSpeed = vecVelocity.Length();

	if( flSpeed > 0.1f )
	{
		vecVelocity = vecVelocity.Normalize();
		vecVelocity.z -= TELEPORT_GRENADE_BOUNCE_DOWN;
		vecVelocity = vecVelocity.Normalize();
		pev->velocity = vecVelocity * ( flSpeed * TELEPORT_GRENADE_BOUNCE_SCALE );
	}

	pev->origin = pev->origin + tr.vecPlaneNormal * 0.5f;
	UTIL_SetOrigin( pev, pev->origin );
	BounceSound();
}

void CTeleportProjectile::LimitThink( void )
{
	if( gpGlobals->time >= pev->dmgtime )
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

	// Once the grenade exceeds the owner's 1024-unit leash, stop it in place.
	// It remains valid until its lifetime expires, matching the Source behavior.
	Vector vecFromLaunch = pev->origin - pev->vuser1;
	if( vecFromLaunch.Length() >= TELEPORT_GRENADE_MAX_DISTANCE )
		pev->velocity = g_vecZero;

	pev->nextthink = gpGlobals->time + 0.1f;
}

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
	m_iDefaultAmmo = 1;
	m_iClip = WEAPON_NOCLIP;
	m_fInSpecialReload = FALSE;

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
	p->pszAmmo1 = "Teleport Charge";
	p->iMaxAmmo1 = 1;
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

	// m_iClip == -2 is our projectile-active flag.  Since this weapon has no
	// clip, this sentinel is carried by weapon_data without displaying ammo.
	if( m_fInSpecialReload )
	{
		if( flTime >= m_flNextPrimaryAttack )
			LaunchTeleportProjectile();
		return;
	}

	if( m_iClip == WEAPON_NOCLIP - 1 )
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
		m_iClip = WEAPON_NOCLIP;
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.5f;
		SendWeaponAnim( TELEPORT_GRENADE_DRAW );
		return;
	}

	pProjectile->pev->velocity = vecVelocity;
	pProjectile->pev->vuser1 = m_pPlayer->pev->origin;
	pProjectile->pev->avelocity = Vector(
		RANDOM_FLOAT( -1000.0f, 1000.0f ),
		RANDOM_FLOAT( -1000.0f, 1000.0f ),
		RANDOM_FLOAT( -1000.0f, 1000.0f ) );
#endif

	m_iClip = WEAPON_NOCLIP - 1;
	SendWeaponAnim( TELEPORT_GRENADE_THROW3 );
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	m_flNextPrimaryAttack = GetNextAttackDelay( TELEPORT_GRENADE_REVOKE_TIME );
	m_flNextSecondaryAttack = GetNextAttackDelay( TELEPORT_GRENADE_RECHARGE );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.5f;
}

void CTeleportGrenade::OnProjectileFinished( void )
{
	m_iClip = WEAPON_NOCLIP;
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

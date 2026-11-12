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
#define TELEPORT_GRENADE_FLOOR_NORMAL    0.87f
#define TELEPORT_GRENADE_GROUND_TRACE_DOWN 512.0f
#define TELEPORT_GRENADE_GROUND_CLEARANCE 1.0f

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
	void EXPORT DelayedRemoveThink( void );

	virtual int TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType );

	void TeleportPerformTeleport( void );
	void TeleportPerformReset( void );

protected:
	BOOL FindTeleportDestination( CBasePlayer *pPlayer, Vector &vecDestination );
	BOOL CanTeleportPlayer( CBasePlayer *pPlayer, const Vector &vecCandidate, Vector &vecDestination );
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

	// vuser1 stores the owner's launch origin.  dmgtime is the hard lifetime.
	pev->vuser1 = pev->origin;
	pev->dmgtime = gpGlobals->time + TELEPORT_GRENADE_MAX_LIFETIME;

	SetTouch( &CTeleportProjectile::ProjectileTouch );
	SetThink( &CTeleportProjectile::LimitThink );
	pev->nextthink = gpGlobals->time + 0.1f;
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
	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, vecOrigin );
		WRITE_BYTE( TE_TELEPORT );
		WRITE_COORD( vecOrigin.x );
		WRITE_COORD( vecOrigin.y );
		WRITE_COORD( vecOrigin.z );
	MESSAGE_END();
}

void CTeleportProjectile::DelayedRemoveThink( void )
{
	UTIL_Remove( this );
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

	FinishWeaponState();

	SendTeleportEffect( vecOldOrigin );
	SendTeleportEffect( vecDestination );

	UTIL_SetOrigin( pPlayer->pev, vecDestination );
	// The original Source call passes vec3_origin as the teleport velocity.
	pPlayer->pev->velocity = g_vecZero;
	pPlayer->pev->flags &= ~FL_ONGROUND;
	pPlayer->pev->groundentity = NULL;
	pPlayer->pev->effects |= EF_NOINTERP;

	// The Source weapon preserves the player's current view angles by default.
	EMIT_SOUND( ENT( pPlayer->pev ), CHAN_ITEM, "items/suitchargeok1.wav", 1.0f, ATTN_NORM );
	UTIL_ScreenShake( vecDestination, 15.0f, 150.0f, 1.0f, 384.0f );

	// Match the original grenade's short post-collision lifetime: it becomes
	// harmless, non-solid and invisible immediately, then is removed 0.1s later.
	pev->takedamage = DAMAGE_NO;
	pev->solid = SOLID_NOT;
	pev->movetype = MOVETYPE_NONE;
	pev->effects |= EF_NODRAW;
	pev->velocity = g_vecZero;
	pev->avelocity = g_vecZero;
	SetTouch( NULL );
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
	pProjectile->pev->vuser1 = m_pPlayer->pev->origin;
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

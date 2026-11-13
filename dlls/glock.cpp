/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "nodes.h"
#include "player.h"

#define PISTOL_FASTEST_REFIRE_TIME		0.1f
#define PISTOL_FASTEST_DRY_REFIRE_TIME	0.2f
#define PISTOL_FIRE_RATE				0.5f
#define PISTOL_ACCURACY_SHOT_PENALTY_TIME	0.2f
#define PISTOL_ACCURACY_MAXIMUM_PENALTY_TIME	1.5f
#define PISTOL_CONE_1DEGREES			0.00873f
#define PISTOL_CONE_6DEGREES			0.05234f

enum glock_e
{
	GLOCK_IDLE1 = 0,
	GLOCK_IDLE2,
	GLOCK_IDLE3,
	GLOCK_SHOOT,
	GLOCK_SHOOT_EMPTY,
	GLOCK_RELOAD,
	GLOCK_RELOAD_NOT_EMPTY,
	GLOCK_DRAW,
	GLOCK_HOLSTER,
	GLOCK_ADD_SILENCER
};

LINK_ENTITY_TO_CLASS( weapon_glock, CGlock )
LINK_ENTITY_TO_CLASS( weapon_9mmhandgun, CGlock )

void CGlock::Spawn()
{
	pev->classname = MAKE_STRING( "weapon_9mmhandgun" ); // hack to allow for old names
	Precache();
	m_iId = WEAPON_GLOCK;
	SET_MODEL( ENT( pev ), "models/w_9mmhandgun.mdl" );

	m_iDefaultAmmo = GLOCK_DEFAULT_GIVE;

	pev->fuser1 = UTIL_WeaponTimeBase();
	pev->fuser4 = 0.0f;

	FallInit();// get ready to fall down.
}

void CGlock::Precache( void )
{
	PRECACHE_MODEL( "models/v_9mmhandgun.mdl" );
	PRECACHE_MODEL( "models/w_9mmhandgun.mdl" );
	PRECACHE_MODEL( "models/p_9mmhandgun.mdl" );

	m_iShell = PRECACHE_MODEL( "models/shell.mdl" );// brass shell

	PRECACHE_SOUND( "items/9mmclip1.wav" );
	PRECACHE_SOUND( "items/9mmclip2.wav" );

	PRECACHE_SOUND( "weapons/pl_gun1.wav" );//silenced handgun
	PRECACHE_SOUND( "weapons/pl_gun2.wav" );//silenced handgun
	PRECACHE_SOUND( "weapons/pl_gun3.wav" );//handgun

	m_usFireGlock1 = PRECACHE_EVENT( 1, "events/glock1.sc" );
	m_usFireGlock2 = PRECACHE_EVENT( 1, "events/glock2.sc" );
}

int CGlock::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "9mm";
	p->iMaxAmmo1 = _9MM_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = GLOCK_MAX_CLIP;
	p->iSlot = 1;
	p->iPosition = 0;
	p->iFlags = 0;
	p->iId = m_iId = WEAPON_GLOCK;
	p->iWeight = GLOCK_WEIGHT;

	return 1;
}

int CGlock::AddToPlayer( CBasePlayer *pPlayer )
{
	if( CBasePlayerWeapon::AddToPlayer( pPlayer ) )
	{
		MESSAGE_BEGIN( MSG_ONE, gmsgWeapPickup, NULL, pPlayer->pev );
			WRITE_BYTE( m_iId );
		MESSAGE_END();
		return TRUE;
	}
	return FALSE;
}

BOOL CGlock::Deploy()
{
	// pev->body = 1;
	return DefaultDeploy( "models/v_9mmhandgun.mdl", "models/p_9mmhandgun.mdl", GLOCK_DRAW, "onehanded", /*UseDecrement() ? 1 : 0*/ 0 );
}

void CGlock::PrimaryAttack( void )
{
	if( m_iClip <= 0 )
	{
		if( m_fFireOnEmpty )
			DryFire();

		return;
	}

	pev->fuser1 = UTIL_WeaponTimeBase() + PISTOL_FASTEST_REFIRE_TIME;

	GlockFire( TRUE );

	// Match HL2's pistol: each shot adds 0.2 seconds of inaccuracy.
	pev->fuser4 += PISTOL_ACCURACY_SHOT_PENALTY_TIME;
}

void CGlock::GlockFire( BOOL fUseAutoAim )
{
	float flRamp = pev->fuser4 / PISTOL_ACCURACY_MAXIMUM_PENALTY_TIME;
	if( flRamp < 0.0f )
		flRamp = 0.0f;
	else if( flRamp > 1.0f )
		flRamp = 1.0f;

	// GoldSrc's cone constants use the same half-angle convention as its
	// existing EV_HLDM spread definitions: 1 degree through 6 degrees.
	float flSpread = PISTOL_CONE_1DEGREES +
		( PISTOL_CONE_6DEGREES - PISTOL_CONE_1DEGREES ) * flRamp;

	m_iClip--;

	m_pPlayer->pev->effects = (int)( m_pPlayer->pev->effects ) | EF_MUZZLEFLASH;

	int flags;
#if CLIENT_WEAPONS
	flags = FEV_NOTHOST;
#else
	flags = 0;
#endif
	// player "shoot" animation
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	// silenced
	if( pev->body == 1 )
	{
		m_pPlayer->m_iWeaponVolume = QUIET_GUN_VOLUME;
		m_pPlayer->m_iWeaponFlash = DIM_GUN_FLASH;
	}
	else
	{
		// non-silenced
		m_pPlayer->m_iWeaponVolume = NORMAL_GUN_VOLUME;
		m_pPlayer->m_iWeaponFlash = NORMAL_GUN_FLASH;
	}

	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAiming;

	if( fUseAutoAim )
	{
		vecAiming = m_pPlayer->GetAutoaimVector( AUTOAIM_10DEGREES );
	}
	else
	{
		vecAiming = gpGlobals->v_forward;
	}

	Vector vecDir;
	vecDir = m_pPlayer->FireBulletsPlayer( 1, vecSrc, vecAiming, Vector( flSpread, flSpread, flSpread ), 8192, BULLET_PLAYER_9MM, 0, 0, m_pPlayer->pev, m_pPlayer->random_seed );

	PLAYBACK_EVENT_FULL( flags, m_pPlayer->edict(), fUseAutoAim ? m_usFireGlock1 : m_usFireGlock2, 0.0, g_vecZero, g_vecZero, vecDir.x, vecDir.y, 0, 0, ( m_iClip == 0 ) ? 1 : 0, 0 );

	// HL2's pistol uses a 0.5-second held-fire rate. Releasing the trigger
	// after the 0.1-second minimum lockout enables immediate click-to-fire.
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + PISTOL_FIRE_RATE;

	if( !m_iClip && m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0 )
		// HEV suit - indicate out of ammo condition
		m_pPlayer->SetSuitUpdate( "!HEV_AMO0", FALSE, 0 );

	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + UTIL_SharedRandomFloat( m_pPlayer->random_seed, 10, 15 );
}

void CGlock::DryFire( void )
{
	PlayEmptySound();

	pev->fuser1 = UTIL_WeaponTimeBase() + PISTOL_FASTEST_DRY_REFIRE_TIME;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + PISTOL_FASTEST_DRY_REFIRE_TIME;
}

void CGlock::UpdatePenaltyTime( void )
{
	if( !m_pPlayer )
		return;

	// Recover accuracy only after the trigger is released and the minimum
	// refire lockout has elapsed, as in HL2's weapon_pistol.
	if( !( m_pPlayer->pev->button & IN_ATTACK ) &&
		pev->fuser1 < UTIL_WeaponTimeBase() )
	{
		pev->fuser4 -= gpGlobals->frametime;

		if( pev->fuser4 < 0.0f )
			pev->fuser4 = 0.0f;
		else if( pev->fuser4 > PISTOL_ACCURACY_MAXIMUM_PENALTY_TIME )
			pev->fuser4 = PISTOL_ACCURACY_MAXIMUM_PENALTY_TIME;
	}
}

void CGlock::ItemPreFrame( void )
{
	UpdatePenaltyTime();
	CBasePlayerWeapon::ItemPreFrame();
}

void CGlock::ItemPostFrame( void )
{
	CBasePlayerWeapon::ItemPostFrame();

	if( m_fInReload || !m_pPlayer )
		return;

	// Do not wait for the full 0.5-second held-fire interval after a
	// deliberate trigger release. This is HL2's click-to-fire behavior.
	if( !( m_pPlayer->pev->button & IN_ATTACK ) &&
		pev->fuser1 < UTIL_WeaponTimeBase() )
	{
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() - PISTOL_FASTEST_REFIRE_TIME;
	}
	else if( ( m_pPlayer->pev->button & IN_ATTACK ) &&
		m_flNextPrimaryAttack < UTIL_WeaponTimeBase() && m_iClip <= 0 )
	{
		DryFire();
	}
}

void CGlock::Reload( void )
{
	if( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0 || m_iClip == GLOCK_MAX_CLIP )
		return;

	int iResult;

	if( m_iClip == 0 )
		iResult = DefaultReload( GLOCK_MAX_CLIP, GLOCK_RELOAD, 1.5f );
	else
		iResult = DefaultReload( GLOCK_MAX_CLIP, GLOCK_RELOAD_NOT_EMPTY, 1.5f );

	if( iResult )
	{
		pev->fuser4 = 0.0f;
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + UTIL_SharedRandomFloat( m_pPlayer->random_seed, 10, 15 );
	}
}

void CGlock::WeaponIdle( void )
{
	ResetEmptySound();

	m_pPlayer->GetAutoaimVector( AUTOAIM_10DEGREES );

	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() )
		return;

	// only idle if the slid isn't back
	if( m_iClip != 0 )
	{
		int iAnim;
		float flRand = UTIL_SharedRandomFloat( m_pPlayer->random_seed, 0.0, 1.0 );

		if( flRand <= 0.3f + 0 * 0.75f )
		{
			iAnim = GLOCK_IDLE3;
			m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 49.0f / 16.0f;
		}
		else if( flRand <= 0.6f + 0 * 0.875f )
		{
			iAnim = GLOCK_IDLE1;
			m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 60.0f / 16.0f;
		}
		else
		{
			iAnim = GLOCK_IDLE2;
			m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 40.0f / 16.0f;
		}
		SendWeaponAnim( iAnim, 1 );
	}
}

class CGlockAmmo : public CBasePlayerAmmo
{
	void Spawn( void )
	{ 
		Precache();
		SET_MODEL( ENT( pev ), "models/w_9mmclip.mdl" );
		CBasePlayerAmmo::Spawn();
	}

	void Precache( void )
	{
		PRECACHE_MODEL( "models/w_9mmclip.mdl" );
		PRECACHE_SOUND( "items/9mmclip1.wav" );
	}

	BOOL AddAmmo( CBaseEntity *pOther )
	{
		if( pOther->GiveAmmo( AMMO_GLOCKCLIP_GIVE, "9mm", _9MM_MAX_CARRY ) != -1 )
		{
			EMIT_SOUND( ENT( pev ), CHAN_ITEM, "items/9mmclip1.wav", 1, ATTN_NORM );
			return TRUE;
		}
		return FALSE;
	}
};

LINK_ENTITY_TO_CLASS( ammo_glockclip, CGlockAmmo )
LINK_ENTITY_TO_CLASS( ammo_9mmclip, CGlockAmmo )

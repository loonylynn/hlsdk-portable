/***
*
*   Panther Eye - portable SDK adaptation.
*
*   Behavioral reference:
*   Cobalt-57/Half-Life-Absolute-Zero, dlls/entity/panthereye.cpp
*
*   This first integration keeps the Panther Eye's signature traits:
*   predatory classification, fast turning, close claw attacks, a timed
*   ballistic leap, high hearing sensitivity, and optional distance-based
*   cloaking. Its original cover-selection / stalking state machine and
*   custom animation framework are not transplanted; this version uses the
*   standard Half-Life monster scheduler and sequence APIs.
*
****/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "schedule.h"
#include "activity.h"
#include "game.h"
#include "soundent.h"
#include "panthereye.h"

namespace
{
enum
{
    TASK_PANTHEREYE_LEAP = LAST_COMMON_TASK + 1
};

Task_t tlPanthereyeLeap[] =
{
    { TASK_STOP_MOVING,       0 },
    { TASK_FACE_ENEMY,        0 },
    { TASK_PANTHEREYE_LEAP,   0 },
};

Schedule_t slPanthereyeLeap[] =
{
    {
        tlPanthereyeLeap,
        ARRAYSIZE( tlPanthereyeLeap ),
        bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD | bits_COND_HEAVY_DAMAGE,
        bits_SOUND_DANGER,
        "PanthereyeLeap"
    },
};

static const char * const g_PantherIdleSounds[] =
{
    "panthereye/pa_idle1.wav",
    "panthereye/pa_idle2.wav",
    "panthereye/pa_idle3.wav",
    "panthereye/pa_idle4.wav"
};

static const char * const g_PantherAttackSounds[] =
{
    "panthereye/pa_attack1.wav"
};

static const char * const g_PantherDeathSounds[] =
{
    "panthereye/pa_death1.wav"
};

static const char * const g_PantherHitSounds[] =
{
    "zombie/claw_strike1.wav",
    "zombie/claw_strike2.wav",
    "zombie/claw_strike3.wav"
};

static const char * const g_PantherMissSounds[] =
{
    "zombie/claw_miss1.wav",
    "zombie/claw_miss2.wav"
};

static float PantherSkillValue( const char *pszCvarBase, float flFallback )
{
    int iSkill = (int)CVAR_GET_FLOAT( "skill" );
    char szCvar[64];
    float flValue;

    if( iSkill < 1 || iSkill > 3 )
        iSkill = 2;

    snprintf( szCvar, sizeof( szCvar ), "%s%d", pszCvarBase, iSkill );
    flValue = CVAR_GET_FLOAT( szCvar );

    return ( flValue > 0.0f ) ? flValue : flFallback;
}

static const char *PantherRandomSound( const char * const *ppszSounds, int iCount )
{
    return ppszSounds[RANDOM_LONG( 0, iCount - 1 )];
}
}

LINK_ENTITY_TO_CLASS( monster_panthereye, CPantherEye )

TYPEDESCRIPTION CPantherEye::m_SaveData[] =
{
    DEFINE_FIELD( CPantherEye, m_flNextLeapAttack, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flLeapLaunchTime, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flRunUntil, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flClawDamage, FIELD_FLOAT ),
    DEFINE_FIELD( CPantherEye, m_bLeapLaunched, FIELD_BOOLEAN ),
};

IMPLEMENT_SAVERESTORE( CPantherEye, CBaseMonster )

CPantherEye::CPantherEye( void )
{
    m_flNextLeapAttack = 0.0f;
    m_flLeapLaunchTime = 0.0f;
    m_flRunUntil = 0.0f;
    m_flClawDamage = 25.0f;
    m_bLeapLaunched = FALSE;
}

void CPantherEye::Precache( void )
{
    PRECACHE_MODEL( "models/panthereye.mdl" );

    PRECACHE_SOUND( "panthereye/pa_attack1.wav" );
    PRECACHE_SOUND( "panthereye/pa_idle1.wav" );
    PRECACHE_SOUND( "panthereye/pa_idle2.wav" );
    PRECACHE_SOUND( "panthereye/pa_idle3.wav" );
    PRECACHE_SOUND( "panthereye/pa_idle4.wav" );
    PRECACHE_SOUND( "panthereye/pa_death1.wav" );

    PRECACHE_SOUND( "zombie/claw_strike1.wav" );
    PRECACHE_SOUND( "zombie/claw_strike2.wav" );
    PRECACHE_SOUND( "zombie/claw_strike3.wav" );
    PRECACHE_SOUND( "zombie/claw_miss1.wav" );
    PRECACHE_SOUND( "zombie/claw_miss2.wav" );
    PRECACHE_SOUND( "zombie/zo_alert10.wav" );
    PRECACHE_SOUND( "zombie/zo_alert20.wav" );
    PRECACHE_SOUND( "zombie/zo_alert30.wav" );
    PRECACHE_SOUND( "zombie/zo_pain1.wav" );
    PRECACHE_SOUND( "zombie/zo_pain2.wav" );
}

void CPantherEye::Spawn( void )
{
    Precache();

    SET_MODEL( ENT( pev ), "models/panthereye.mdl" );
    UTIL_SetSize( pev, Vector( -28, -28, 0 ), Vector( 28, 28, 36 ) );

    pev->classname = MAKE_STRING( "monster_panthereye" );
    pev->solid = SOLID_SLIDEBOX;
    pev->movetype = MOVETYPE_STEP;
    pev->takedamage = DAMAGE_AIM;
    pev->flags |= FL_MONSTER;
    pev->effects = 0;

    m_bloodColor = BLOOD_COLOR_GREEN;
    pev->health = PantherSkillValue( "sk_panthereye_health", 100.0f );
    pev->max_health = pev->health;
    m_flClawDamage = PantherSkillValue( "sk_panthereye_dmg_claw", 25.0f );

    pev->view_ofs = VEC_VIEW;
    m_flFieldOfView = 0.5f;
    m_MonsterState = MONSTERSTATE_NONE;
    m_afCapability = bits_CAP_DOORS_GROUP | bits_CAP_MELEE_ATTACK1 | bits_CAP_MELEE_ATTACK2;

    m_flNextLeapAttack = gpGlobals->time;
    m_flLeapLaunchTime = 0.0f;
    m_flRunUntil = 0.0f;
    m_bLeapLaunched = FALSE;

    MonsterInit();
}

int CPantherEye::Classify( void )
{
    return CLASS_ALIEN_PREDATOR;
}

int CPantherEye::IRelationship( CBaseEntity *pTarget )
{
    if( !pTarget )
        return R_NO;

    if( FClassnameIs( pTarget->pev, "monster_panthereye" ) )
        return R_AL;

    // The original creature treats alien prey as a target of opportunity,
    // rather than using the standard full hate relationship.
    if( pTarget->Classify() == CLASS_ALIEN_PREY )
        return R_DL;

    return CBaseMonster::IRelationship( pTarget );
}

int CPantherEye::ISoundMask( void )
{
    return bits_SOUND_WORLD | bits_SOUND_COMBAT | bits_SOUND_PLAYER;
}

float CPantherEye::HearingSensitivity( void )
{
    return 2.2f;
}

void CPantherEye::SetYawSpeed( void )
{
    pev->yaw_speed = 200;
}

BOOL CPantherEye::SetPantherSequence( const char *pszSequence, Activity activity, float flRate )
{
    int iSequence = LookupSequence( pszSequence );

    if( iSequence < 0 )
        return FALSE;

    if( pev->sequence != iSequence )
        pev->frame = 0;

    pev->sequence = iSequence;
    ResetSequenceInfo();
    pev->framerate = flRate;
    m_Activity = activity;
    m_IdealActivity = activity;
    SetYawSpeed();

    return TRUE;
}

void CPantherEye::SetActivity( Activity NewActivity )
{
    const char *pszSequence = NULL;
    float flRate = 1.0f;

    switch( NewActivity )
    {
    case ACT_IDLE:
        if( RANDOM_LONG( 0, 4 ) == 0 )
            pszSequence = "itch";
        else if( RANDOM_LONG( 0, 2 ) == 0 )
            pszSequence = "shakes";
        else
            pszSequence = "subtle_motion";
        break;

    case ACT_WALK:
        pszSequence = "walk";
        break;

    case ACT_RUN:
        if( gpGlobals->time < m_flRunUntil )
        {
            pszSequence = "run";
            flRate = 1.05f;
        }
        else
        {
            // The original model's deliberate walk is used for normal
            // movement; the full run is reserved for alerted / hurt states.
            pszSequence = "walk";
            flRate = 1.12f;
        }
        break;

    case ACT_MELEE_ATTACK1:
        if( RANDOM_LONG( 0, 1 ) )
            pszSequence = "attack_main_claw";
        else
            pszSequence = "attack_simple_claw";
        flRate = 1.2f;
        break;

    case ACT_MELEE_ATTACK2:
        pszSequence = "crouch_to_jump";
        flRate = 0.92f;
        break;

    case ACT_DIEVIOLENT:
        pszSequence = "death_violent";
        break;

    default:
        break;
    }

    if( pszSequence && SetPantherSequence( pszSequence, NewActivity, flRate ) )
        return;

    CBaseMonster::SetActivity( NewActivity );
}

BOOL CPantherEye::FCanCheckAttacks( void )
{
    return !HasConditions( bits_COND_ENEMY_TOOFAR );
}

BOOL CPantherEye::CheckMeleeAttack1( float flDot, float flDist )
{
    float flZDist;

    if( !m_hEnemy || HasConditions( bits_COND_ENEMY_OCCLUDED ) )
        return FALSE;

    if( flDist > 80.0f || flDot < 0.7f )
        return FALSE;

    if( m_hEnemy->Classify() == CLASS_ALIEN_BIOWEAPON ||
        m_hEnemy->Classify() == CLASS_PLAYER_BIOWEAPON )
        return FALSE;

    if( m_vecEnemyLKP.z > pev->absmax.z )
        flZDist = m_vecEnemyLKP.z - pev->absmax.z;
    else if( m_vecEnemyLKP.z < pev->absmin.z )
        flZDist = pev->absmin.z - m_vecEnemyLKP.z;
    else
        flZDist = 0.0f;

    return ( flZDist < 32.0f );
}

BOOL CPantherEye::CanLeapToEnemy( void )
{
    TraceResult tr;
    Vector vecStart;
    Vector vecEnd;

    if( !m_hEnemy )
        return FALSE;

    vecStart = pev->origin + Vector( 0, 0, 30 );
    vecEnd = m_vecEnemyLKP + Vector( 0, 0, 30 );

    TRACE_MONSTER_HULL( edict(), vecStart, vecEnd, dont_ignore_monsters, edict(), &tr );

    if( tr.flFraction >= 1.0f || tr.pHit == m_hEnemy->edict() )
        return TRUE;

    // The second, higher trace permits a leap over low cover while still
    // rejecting a solid wall between the Panther Eye and its target.
    vecStart = pev->origin + Vector( 0, 0, 80 );
    vecEnd = m_vecEnemyLKP + Vector( 0, 0, 40 );
    TRACE_MONSTER_HULL( edict(), vecStart, vecEnd, dont_ignore_monsters, edict(), &tr );

    return ( tr.flFraction >= 1.0f || tr.pHit == m_hEnemy->edict() );
}

BOOL CPantherEye::CheckMeleeAttack2( float flDot, float flDist )
{
    float flZDist;

    if( !m_hEnemy || gpGlobals->time < m_flNextLeapAttack )
        return FALSE;

    if( HasConditions( bits_COND_ENEMY_OCCLUDED ) || flDist < 180.0f || flDist > 850.0f )
        return FALSE;

    if( m_hEnemy->Classify() == CLASS_ALIEN_BIOWEAPON ||
        m_hEnemy->Classify() == CLASS_PLAYER_BIOWEAPON )
        return FALSE;

    if( m_vecEnemyLKP.z > pev->absmax.z )
        flZDist = m_vecEnemyLKP.z - pev->absmax.z;
    else if( m_vecEnemyLKP.z < pev->absmin.z )
        flZDist = pev->absmin.z - m_vecEnemyLKP.z;
    else
        flZDist = 0.0f;

    if( flZDist >= 180.0f || flDot < 0.91f || !HasConditions( bits_COND_SEE_ENEMY ) )
        return FALSE;

    return CanLeapToEnemy();
}

Schedule_t *CPantherEye::GetScheduleOfType( int Type )
{
    if( Type == SCHED_MELEE_ATTACK2 )
        return &slPanthereyeLeap[0];

    return CBaseMonster::GetScheduleOfType( Type );
}

void CPantherEye::StartTask( Task_t *pTask )
{
    if( pTask->iTask == TASK_PANTHEREYE_LEAP )
    {
        SetActivity( ACT_MELEE_ATTACK2 );
        m_flLeapLaunchTime = gpGlobals->time + ( 10.35f / 15.0f );
        m_bLeapLaunched = FALSE;
        TaskBegin();
        return;
    }

    CBaseMonster::StartTask( pTask );
}

void CPantherEye::RunTask( Task_t *pTask )
{
    if( pTask->iTask == TASK_PANTHEREYE_LEAP )
    {
        if( !m_bLeapLaunched && gpGlobals->time >= m_flLeapLaunchTime )
            LaunchLeap();

        if( m_fSequenceFinished )
        {
            SetTouch( NULL );
            TaskComplete();
        }
        return;
    }

    CBaseMonster::RunTask( pTask );
}

void CPantherEye::LaunchLeap( void )
{
    Vector vecTarget;
    Vector vecDirection;
    float flGravity;
    float flHeight;
    float flSpeedExtra;
    float flVerticalSpeed;
    float flFlightTime;
    float flDistance;
    float flLength;

    if( m_bLeapLaunched )
        return;

    m_bLeapLaunched = TRUE;

    // Start cooldown at launch time, even if the leap is interrupted by a
    // collision immediately afterward.
    {
        int iSkill = (int)CVAR_GET_FLOAT( "skill" );
        if( iSkill >= 3 )
            m_flNextLeapAttack = gpGlobals->time + RANDOM_FLOAT( 2.8f, 3.2f );
        else if( iSkill == 2 )
            m_flNextLeapAttack = gpGlobals->time + RANDOM_FLOAT( 3.4f, 3.8f );
        else
            m_flNextLeapAttack = gpGlobals->time + RANDOM_FLOAT( 4.3f, 4.8f );
    }

    ClearBits( pev->flags, FL_ONGROUND );
    pev->groundentity = NULL;
    UTIL_SetOrigin( pev, pev->origin + Vector( 0, 0, 1 ) );

    if( m_hEnemy )
        vecTarget = m_vecEnemyLKP + m_hEnemy->pev->view_ofs;
    else
    {
        UTIL_MakeVectors( pev->angles );
        vecTarget = pev->origin + gpGlobals->v_forward * 320.0f;
    }

    vecDirection = vecTarget - pev->origin;
    flDistance = vecDirection.Length2D();
    vecDirection.z = 0.0f;

    if( vecDirection.Length2D() < 1.0f )
    {
        UTIL_MakeVectors( pev->angles );
        vecDirection = gpGlobals->v_forward;
    }
    vecDirection = vecDirection.Normalize();

    flGravity = CVAR_GET_FLOAT( "sv_gravity" );
    if( flGravity < 1.0f )
        flGravity = 800.0f;

    flHeight = vecTarget.z - pev->origin.z;
    if( flHeight < 18.0f )
        flHeight = 18.0f;

    flSpeedExtra = 1.0f;
    if( flHeight > 55.0f )
    {
        flSpeedExtra = 1.0f + ( flHeight - 55.0f ) / 55.0f;
        flHeight = 55.0f;
    }

    flVerticalSpeed = sqrt( 1.7f * flGravity * flHeight * flSpeedExtra );
    flFlightTime = flVerticalSpeed / flGravity;
    flDistance = max( flDistance - 80.0f, 320.0f );

    if( flFlightTime < 0.05f )
        flFlightTime = 0.05f;

    pev->velocity = vecDirection * ( flDistance / flFlightTime );
    pev->velocity.z = flVerticalSpeed;

    flLength = pev->velocity.Length();
    if( flLength > 1100.0f )
        pev->velocity = pev->velocity * ( 1100.0f / flLength );

    if( RANDOM_LONG( 0, 2 ) != 0 )
        AttackSound();

    SetTouch( &CPantherEye::LeapTouch );
}

void CPantherEye::LeapTouch( CBaseEntity *pOther )
{
    float flSpeed;
    float flDamageScale;
    int iRelationship;

    if( !pOther || !pOther->pev->takedamage || !pOther->MyMonsterPointer() )
        return;

    iRelationship = IRelationship( pOther );
    if( pOther != m_hEnemy && ( iRelationship == R_NO || iRelationship == R_AL ) )
        return;

    flSpeed = pev->velocity.Length();
    if( flSpeed < 50.0f || FBitSet( pev->flags, FL_ONGROUND ) )
        return;

    flDamageScale = ( min( flSpeed, 900.0f ) / 900.0f ) * 1.30f;
    flDamageScale = max( flDamageScale, 0.25f );

    pOther->TakeDamage( pev, pev, m_flClawDamage * flDamageScale, DMG_SLASH );

    EMIT_SOUND_DYN(
        ENT( pev ),
        CHAN_WEAPON,
        PantherRandomSound( g_PantherHitSounds, ARRAYSIZE( g_PantherHitSounds ) ),
        1.0f,
        ATTN_IDLE,
        0,
        RANDOM_LONG( 95, 105 )
    );

    SetTouch( NULL );
}

void CPantherEye::HandleAnimEvent( MonsterEvent_t *pEvent )
{
    if( pEvent->event == 1 )
    {
        CBaseEntity *pHurt = CheckTraceHullAttack( 84.0f * 1.2f, (int)m_flClawDamage, DMG_SLASH );

        if( pHurt )
        {
            EMIT_SOUND_DYN(
                ENT( pev ),
                CHAN_WEAPON,
                PantherRandomSound( g_PantherHitSounds, ARRAYSIZE( g_PantherHitSounds ) ),
                1.0f,
                ATTN_NORM,
                0,
                RANDOM_LONG( 95, 105 )
            );
        }
        else
        {
            EMIT_SOUND_DYN(
                ENT( pev ),
                CHAN_WEAPON,
                PantherRandomSound( g_PantherMissSounds, ARRAYSIZE( g_PantherMissSounds ) ),
                1.0f,
                ATTN_NORM,
                0,
                RANDOM_LONG( 95, 105 )
            );
        }

        if( RANDOM_LONG( 0, 1 ) )
            AttackSound();

        return;
    }

    CBaseMonster::HandleAnimEvent( pEvent );
}

void CPantherEye::PrescheduleThink( void )
{
    float flCloak;
    float flTargetRender;
    BOOL fCanCloak;

    if( m_afSoundTypes & ( bits_SOUND_COMBAT | bits_SOUND_PLAYER ) )
        m_flRunUntil = gpGlobals->time + 12.0f;

    CBaseMonster::PrescheduleThink();

    flCloak = CVAR_GET_FLOAT( "panthereye_cloak" );
    fCanCloak = ( flCloak > 0.0f &&
                  m_hEnemy != NULL &&
                  FBitSet( pev->flags, FL_ONGROUND ) &&
                  gpGlobals->time >= m_flRunUntil &&
                  ( m_vecEnemyLKP - pev->origin ).Length() >= 170.0f );

    if( fCanCloak )
        flTargetRender = min( flCloak, 1.0f ) * 255.0f;
    else
        flTargetRender = 255.0f;

    if( pev->renderamt > flTargetRender )
    {
        pev->renderamt = max( pev->renderamt - 50.0f, flTargetRender );
        pev->rendermode = kRenderTransTexture;
    }
    else if( pev->renderamt < flTargetRender )
    {
        pev->renderamt = min( pev->renderamt + 50.0f, flTargetRender );
        if( pev->renderamt >= 255.0f )
            pev->rendermode = kRenderNormal;
    }
}

void CPantherEye::AttackSound( void )
{
    EMIT_SOUND_DYN(
        ENT( pev ),
        CHAN_VOICE,
        PantherRandomSound( g_PantherAttackSounds, ARRAYSIZE( g_PantherAttackSounds ) ),
        1.0f,
        ATTN_NORM,
        0,
        RANDOM_LONG( 95, 105 )
    );
}

void CPantherEye::IdleSound( void )
{
    EMIT_SOUND_DYN(
        ENT( pev ),
        CHAN_VOICE,
        PantherRandomSound( g_PantherIdleSounds, ARRAYSIZE( g_PantherIdleSounds ) ),
        1.0f,
        ATTN_NORM - 0.13f,
        0,
        RANDOM_LONG( 95, 105 )
    );
}

void CPantherEye::AlertSound( void )
{
    static const char * const sounds[] =
    {
        "zombie/zo_alert10.wav",
        "zombie/zo_alert20.wav",
        "zombie/zo_alert30.wav"
    };

    EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, PantherRandomSound( sounds, ARRAYSIZE( sounds ) ),
                    1.0f, ATTN_NORM, 0, RANDOM_LONG( 95, 105 ) );
}

void CPantherEye::PainSound( void )
{
    static const char * const sounds[] =
    {
        "zombie/zo_pain1.wav",
        "zombie/zo_pain2.wav"
    };

    EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, PantherRandomSound( sounds, ARRAYSIZE( sounds ) ),
                    1.0f, ATTN_NORM, 0, RANDOM_LONG( 95, 105 ) );
}

void CPantherEye::DeathSound( void )
{
    EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, PantherRandomSound( g_PantherDeathSounds, ARRAYSIZE( g_PantherDeathSounds ) ),
                    1.0f, ATTN_IDLE, 0, RANDOM_LONG( 96, 102 ) );
}

DEFINE_CUSTOM_SCHEDULES( CPantherEye )
{
    slPanthereyeLeap,
};

IMPLEMENT_CUSTOM_SCHEDULES( CPantherEye, CBaseMonster )

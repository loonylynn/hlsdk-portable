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
    TASK_PANTHEREYE_LEAP = LAST_COMMON_TASK + 1,
    TASK_PANTHEREYE_FIND_STALK_COVER,
    TASK_PANTHEREYE_FIND_FLEE_COVER,
    TASK_PANTHEREYE_STALK_WAIT
};

enum
{
    SCHED_PANTHEREYE_STALK_WAIT = LAST_COMMON_SCHEDULE + 1,
    SCHED_PANTHEREYE_STALK_MOVE,
    SCHED_PANTHEREYE_FLEE
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

Task_t tlPanthereyeStalkWait[] =
{
    { TASK_STOP_MOVING,                 0 },
    { TASK_SET_ACTIVITY,                (float)ACT_IDLE },
    { TASK_PANTHEREYE_STALK_WAIT,       0 },
};

Schedule_t slPanthereyeStalkWait[] =
{
    {
        tlPanthereyeStalkWait,
        ARRAYSIZE( tlPanthereyeStalkWait ),
        bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
        bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2 |
        bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE,
        bits_SOUND_DANGER,
        "PanthereyeStalkWait"
    },
};

Task_t tlPanthereyeStalkMove[] =
{
    { TASK_STOP_MOVING,                 0 },
    { TASK_SET_FAIL_SCHEDULE,           (float)SCHED_PANTHEREYE_STALK_WAIT },
    { TASK_PANTHEREYE_FIND_STALK_COVER, 0 },
    { TASK_WALK_PATH,                   0 },
    { TASK_WAIT_FOR_MOVEMENT,           0 },
    { TASK_WAIT_RANDOM,                 0.8f },
};

Schedule_t slPanthereyeStalkMove[] =
{
    {
        tlPanthereyeStalkMove,
        ARRAYSIZE( tlPanthereyeStalkMove ),
        bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
        bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2 |
        bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE,
        bits_SOUND_DANGER,
        "PanthereyeStalkMove"
    },
};

Task_t tlPanthereyeFlee[] =
{
    { TASK_STOP_MOVING,                 0 },
    { TASK_SET_FAIL_SCHEDULE,           (float)SCHED_TAKE_COVER_FROM_ENEMY },
    { TASK_PANTHEREYE_FIND_FLEE_COVER,  0 },
    { TASK_RUN_PATH,                    0 },
    { TASK_WAIT_FOR_MOVEMENT,           0 },
    { TASK_WAIT,                        0.6f },
};

Schedule_t slPanthereyeFlee[] =
{
    {
        tlPanthereyeFlee,
        ARRAYSIZE( tlPanthereyeFlee ),
        bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD,
        bits_SOUND_DANGER,
        "PanthereyeFlee"
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
    DEFINE_FIELD( CPantherEye, m_flStareTime, FIELD_FLOAT ),
    DEFINE_FIELD( CPantherEye, m_flAggressiveUntil, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flFleeUntil, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flStalkWaitUntil, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flLastDamageReact, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_bLeapLaunched, FIELD_BOOLEAN ),
    DEFINE_FIELD( CPantherEye, m_bStalking, FIELD_BOOLEAN ),
    DEFINE_FIELD( CPantherEye, m_bEnemyLookingAtMe, FIELD_BOOLEAN ),
    DEFINE_FIELD( CPantherEye, m_bAmbushLeapRequested, FIELD_BOOLEAN ),
    DEFINE_FIELD( CPantherEye, m_bCornered, FIELD_BOOLEAN ),
    DEFINE_FIELD( CPantherEye, m_bPissable, FIELD_BOOLEAN ),
};

IMPLEMENT_SAVERESTORE( CPantherEye, CBaseMonster )

CPantherEye::CPantherEye( void )
{
    m_flNextLeapAttack = 0.0f;
    m_flLeapLaunchTime = 0.0f;
    m_flRunUntil = 0.0f;
    m_flClawDamage = 25.0f;
    m_flStareTime = 0.0f;
    m_flAggressiveUntil = 0.0f;
    m_flFleeUntil = 0.0f;
    m_flStalkWaitUntil = 0.0f;
    m_flLastDamageReact = -1.0f;
    m_bLeapLaunched = FALSE;
    m_bStalking = FALSE;
    m_bEnemyLookingAtMe = FALSE;
    m_bAmbushLeapRequested = FALSE;
    m_bCornered = FALSE;
    m_bPissable = FALSE;
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
    pev->renderamt = 255;
    pev->rendermode = kRenderNormal;

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
    m_flStareTime = 0.0f;
    m_flAggressiveUntil = 0.0f;
    m_flFleeUntil = 0.0f;
    m_flStalkWaitUntil = 0.0f;
    m_flLastDamageReact = -1.0f;
    m_bLeapLaunched = FALSE;
    m_bStalking = FALSE;
    m_bEnemyLookingAtMe = FALSE;
    m_bAmbushLeapRequested = FALSE;
    m_bCornered = FALSE;
    m_bPissable = FALSE;

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

    UTIL_TraceHull( vecStart, vecEnd, dont_ignore_monsters, large_hull, edict(), &tr );

    if( tr.flFraction >= 1.0f || tr.pHit == m_hEnemy->edict() )
        return TRUE;

    // The second, higher trace permits a leap over low cover while still
    // rejecting a solid wall between the Panther Eye and its target.
    vecStart = pev->origin + Vector( 0, 0, 80 );
    vecEnd = m_vecEnemyLKP + Vector( 0, 0, 40 );
    UTIL_TraceHull( vecStart, vecEnd, dont_ignore_monsters, large_hull, edict(), &tr );

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
    switch( Type )
    {
    case SCHED_MELEE_ATTACK2:
        return &slPanthereyeLeap[0];

    case SCHED_PANTHEREYE_STALK_WAIT:
        return &slPanthereyeStalkWait[0];

    case SCHED_PANTHEREYE_STALK_MOVE:
        return &slPanthereyeStalkMove[0];

    case SCHED_PANTHEREYE_FLEE:
        return &slPanthereyeFlee[0];
    }

    return CBaseMonster::GetScheduleOfType( Type );
}

Schedule_t *CPantherEye::GetSchedule( void )
{
    if( pev->deadflag != DEAD_NO ||
        m_MonsterState != MONSTERSTATE_COMBAT ||
        m_hEnemy == NULL ||
        HasConditions( bits_COND_ENEMY_DEAD ) )
    {
        return CBaseMonster::GetSchedule();
    }

    if( m_flFleeUntil > gpGlobals->time )
        return GetScheduleOfType( SCHED_PANTHEREYE_FLEE );

    if( m_flAggressiveUntil > gpGlobals->time )
        return CBaseMonster::GetSchedule();

    if( m_bAmbushLeapRequested )
    {
        m_bAmbushLeapRequested = FALSE;
        return GetScheduleOfType( SCHED_MELEE_ATTACK2 );
    }

    // If the target is already within claw range, do not sacrifice a clear
    // attack opportunity just to keep stalking.
    if( HasConditions( bits_COND_CAN_MELEE_ATTACK1 ) )
        return CBaseMonster::GetSchedule();

    if( m_bStalking )
    {
        if( m_bEnemyLookingAtMe )
            return GetScheduleOfType( SCHED_PANTHEREYE_STALK_WAIT );

        return GetScheduleOfType( SCHED_PANTHEREYE_STALK_MOVE );
    }

    return CBaseMonster::GetSchedule();
}

void CPantherEye::StartTask( Task_t *pTask )
{
    if( pTask->iTask == TASK_PANTHEREYE_FIND_STALK_COVER )
    {
        Vector vecThreat;
        Vector vecViewOffset;

        if( m_hEnemy == NULL )
        {
            TaskFail();
            return;
        }

        vecThreat = m_hEnemy->pev->origin;
        vecViewOffset = m_hEnemy->pev->view_ofs;

        // Prefer a real occlusion point: first a lateral step, then a
        // navigation-node cover position. This mirrors Absolute Zero's
        // original cover-first stalking logic.
        if( FindLateralCover( vecThreat, vecViewOffset ) ||
            FindCover( vecThreat, vecViewOffset, 0.0f, CoverRadius() * 1.8f ) )
        {
            m_movementActivity = ACT_WALK;
            m_bCornered = FALSE;
            m_bPissable = FALSE;
            TaskComplete();
            return;
        }

        // No graph cover found? Try to move behind the target, slightly
        // offset to either side, rather than walking straight at its face.
        {
            float flYaw = m_hEnemy->pev->angles.y * (float)(M_PI / 180.0);
            float flSide = RANDOM_LONG( 0, 1 ) ? 1.0f : -1.0f;
            Vector vecForward( cos( flYaw ), sin( flYaw ), 0.0f );
            Vector vecRight( -sin( flYaw ), cos( flYaw ), 0.0f );
            Vector vecGoal = vecThreat - vecForward * 280.0f + vecRight * ( 120.0f * flSide );

            if( MoveToLocation( ACT_WALK, 1.5f, vecGoal ) )
            {
                m_bCornered = FALSE;
                TaskComplete();
                return;
            }
        }

        // If no concealed route can be found, the Panther Eye is effectively
        // cornered. That makes it more likely to turn and fight when noticed.
        m_bCornered = TRUE;
        m_bPissable = TRUE;
        TaskFail();
        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_FIND_FLEE_COVER )
    {
        Vector vecThreat;
        Vector vecViewOffset;

        if( m_hEnemy == NULL )
        {
            TaskFail();
            return;
        }

        vecThreat = m_hEnemy->pev->origin;
        vecViewOffset = m_hEnemy->pev->view_ofs;

        if( FindLateralCover( vecThreat, vecViewOffset ) ||
            FindCover( vecThreat, vecViewOffset, 0.0f, CoverRadius() * 2.0f ) )
        {
            m_movementActivity = ACT_RUN;
            TaskComplete();
            return;
        }

        // As a last resort, run away from the threat along the opposite
        // horizontal vector. This still works on maps without a node graph.
        {
            Vector vecAway = pev->origin - vecThreat;
            vecAway.z = 0.0f;

            if( vecAway.Length() < 1.0f )
            {
                UTIL_MakeVectors( pev->angles );
                vecAway = -gpGlobals->v_forward;
                vecAway.z = 0.0f;
            }

            vecAway = vecAway.Normalize();
            if( MoveToLocation( ACT_RUN, 0.0f, pev->origin + vecAway * 320.0f ) )
            {
                TaskComplete();
                return;
            }
        }

        // No viable escape route. Drop the flee timer so the normal AI can
        // select its best available combat response instead of retrying forever.
        m_flFleeUntil = gpGlobals->time;
        m_bCornered = TRUE;
        TaskFail();
        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_STALK_WAIT )
    {
        m_flStalkWaitUntil = gpGlobals->time + 0.2f;
        TaskBegin();
        return;
    }

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
    if( pTask->iTask == TASK_PANTHEREYE_STALK_WAIT )
    {
        if( m_hEnemy == NULL ||
            !m_bEnemyLookingAtMe ||
            m_flAggressiveUntil > gpGlobals->time ||
            m_flFleeUntil > gpGlobals->time )
        {
            TaskComplete();
            return;
        }

        // Keep the idle/fidget sequence playing while the player watches.
        // PrescheduleThink accumulates the stare duration and flips to anger
        // after the same prolonged-look threshold used by the original.
        if( gpGlobals->time >= m_flStalkWaitUntil )
            m_flStalkWaitUntil = gpGlobals->time + 0.2f;

        return;
    }

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
    float flDistance = 0.0f;
    BOOL fEnemyVisible = FALSE;
    BOOL fCanCloak = FALSE;

    if( m_afSoundTypes & ( bits_SOUND_COMBAT | bits_SOUND_PLAYER ) )
        m_flRunUntil = gpGlobals->time + 12.0f;

    if( pev->deadflag == DEAD_NO && m_hEnemy != NULL )
    {
        Vector vecToPanther = pev->origin - m_hEnemy->pev->origin;
        float flFacingDot = -1.0f;

        flDistance = ( m_vecEnemyLKP - pev->origin ).Length();
        fEnemyVisible = HasConditions( bits_COND_SEE_ENEMY ) &&
                        !HasConditions( bits_COND_ENEMY_OCCLUDED );

        if( fEnemyVisible )
        {
            float flYaw = m_hEnemy->pev->angles.y * (float)(M_PI / 180.0);
            Vector vecEnemyForward( cos( flYaw ), sin( flYaw ), 0.0f );

            vecToPanther.z = 0.0f;
            if( vecToPanther.Length() > 1.0f )
            {
                vecToPanther = vecToPanther.Normalize();
                flFacingDot = DotProduct( vecEnemyForward, vecToPanther );
            }
        }

        m_bEnemyLookingAtMe = fEnemyVisible && ( flFacingDot > 0.30f );

        if( m_bEnemyLookingAtMe )
            m_flStareTime += 0.1f;
        else
            m_flStareTime = 0.0f;

        // If the player keeps the Panther Eye in view for long enough, its
        // cautious stalking state breaks and it commits to a short attack run.
        if( m_bEnemyLookingAtMe &&
            m_flAggressiveUntil <= gpGlobals->time &&
            flDistance < ( m_bCornered ? 2100.0f : 1600.0f ) )
        {
            float flRequiredStare = m_bCornered ? 1.2f : ( m_bPissable ? 1.8f : 2.6f );

            if( m_flStareTime >= flRequiredStare )
            {
                m_flAggressiveUntil = gpGlobals->time + ( m_bCornered ? 8.0f : 5.0f );
                m_flFleeUntil = 0.0f;
                m_flRunUntil = gpGlobals->time + 12.0f;
                m_bAmbushLeapRequested = FALSE;
            }
        }

        // At very close range the creature stops playing games. When the
        // player turns away at leap distance, it uses the model's crouch-to-
        // jump attack as an ambush instead of announcing a conventional chase.
        if( m_flAggressiveUntil <= gpGlobals->time &&
            m_flFleeUntil <= gpGlobals->time )
        {
            if( flDistance < 220.0f )
            {
                m_flAggressiveUntil = gpGlobals->time + 4.0f;
                m_flRunUntil = gpGlobals->time + 12.0f;
            }
            else if( fEnemyVisible &&
                     !m_bEnemyLookingAtMe &&
                     flFacingDot < 0.15f &&
                     flDistance >= 180.0f &&
                     flDistance <= 850.0f &&
                     gpGlobals->time >= m_flNextLeapAttack &&
                     CanLeapToEnemy() )
            {
                m_bAmbushLeapRequested = TRUE;
            }
        }

        // Once enraged, keep the pursuit alive while the target remains close
        // and visible. If it has escaped, the Panther Eye can return to stalking.
        if( m_flAggressiveUntil > gpGlobals->time &&
            fEnemyVisible &&
            flDistance < 500.0f )
        {
            m_flAggressiveUntil = gpGlobals->time + 2.0f;
        }

        if( m_flFleeUntil <= gpGlobals->time )
            m_flFleeUntil = 0.0f;

        m_bStalking = ( m_flAggressiveUntil <= gpGlobals->time &&
                        m_flFleeUntil <= gpGlobals->time &&
                        FBitSet( pev->flags, FL_ONGROUND ) &&
                        !m_bAmbushLeapRequested );

        // Break a movement-to-cover task as soon as the target notices the
        // Panther Eye. The next schedule becomes a still, non-threatening idle.
        if( m_pSchedule == &slPanthereyeStalkMove[0] &&
            ( m_bEnemyLookingAtMe ||
              m_flAggressiveUntil > gpGlobals->time ||
              m_bAmbushLeapRequested ) )
        {
            TaskFail();
        }

        if( m_flAggressiveUntil <= gpGlobals->time &&
            m_flFleeUntil <= gpGlobals->time &&
            !m_bStalking )
        {
            m_bCornered = FALSE;
            m_bPissable = FALSE;
        }
    }
    else
    {
        m_bEnemyLookingAtMe = FALSE;
        m_bStalking = FALSE;
        m_bAmbushLeapRequested = FALSE;
        m_flStareTime = 0.0f;

        if( m_flAggressiveUntil <= gpGlobals->time )
            m_flAggressiveUntil = 0.0f;
        if( m_flFleeUntil <= gpGlobals->time )
            m_flFleeUntil = 0.0f;
    }

    CBaseMonster::PrescheduleThink();

    flCloak = CVAR_GET_FLOAT( "panthereye_cloak" );
    fCanCloak = ( flCloak > 0.0f &&
                  m_bStalking &&
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

int CPantherEye::TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType )
{
    int iResult = CBaseMonster::TakeDamage( pevInflictor, pevAttacker, flDamage, bitsDamageType );

    if( iResult > 0 && pev->deadflag == DEAD_NO &&
        flDamage > 0.0f && gpGlobals->time >= m_flLastDamageReact )
    {
        CBaseEntity *pThreat = NULL;
        float flThreatDistance = 99999.0f;

        if( pevAttacker != NULL )
            pThreat = CBaseEntity::Instance( pevAttacker );

        if( pThreat == this )
            pThreat = NULL;

        if( pThreat != NULL )
            flThreatDistance = ( pThreat->pev->origin - pev->origin ).Length();
        else if( m_hEnemy != NULL )
        {
            pThreat = m_hEnemy;
            flThreatDistance = ( pThreat->pev->origin - pev->origin ).Length();
        }

        if( pThreat != NULL )
        {
            m_flLastDamageReact = gpGlobals->time + 0.75f;
            m_flRunUntil = gpGlobals->time + 12.0f;

            // Wounded at close range: fight back. Wounded at range: vanish
            // into cover and try to break line of sight before re-engaging.
            if( flThreatDistance < ( m_bCornered ? 1100.0f : 850.0f ) )
            {
                m_flAggressiveUntil = gpGlobals->time + 8.0f;
                m_flFleeUntil = 0.0f;
            }
            else
            {
                m_flFleeUntil = gpGlobals->time + 7.0f;
                m_flAggressiveUntil = 0.0f;
            }

            m_bStalking = FALSE;
            m_bAmbushLeapRequested = FALSE;
        }
    }

    return iResult;
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
    slPanthereyeStalkWait,
    slPanthereyeStalkMove,
    slPanthereyeFlee,
};

IMPLEMENT_CUSTOM_SCHEDULES( CPantherEye, CBaseMonster )

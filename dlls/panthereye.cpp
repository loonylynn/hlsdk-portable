/***
*
*   Panther Eye - portable SDK adaptation.
*
*   Behavioral reference:
*   Cobalt-57/Half-Life-Absolute-Zero, dlls/entity/panthereye.cpp
*
*   This port restores the original Panther Eye's stalking, cover-seeking,
*   anger / retreat decisions, leap recovery, animation variants, and
*   cockroach-feeding behavior using the stock GoldSrc monster scheduler.
*   Absolute Zero's custom base-monster hooks and animation-event queue are
*   replaced with local tasks and timed sequence events.
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
    TASK_PANTHEREYE_STALK_WAIT,
    TASK_PANTHEREYE_FIND_UNSTUCK_DIRECTION,
    TASK_PANTHEREYE_UNSTUCK_LEAP,
    TASK_PANTHEREYE_GET_BUG_ANIM,
    TASK_PANTHEREYE_COVER_FAIL_WAIT,
    TASK_PANTHEREYE_NORMALMOVE
};

enum
{
    SCHED_PANTHEREYE_STALK_WAIT = LAST_COMMON_SCHEDULE + 1,
    SCHED_PANTHEREYE_STALK_MOVE,
    SCHED_PANTHEREYE_CHASE,
    SCHED_PANTHEREYE_UNSTUCK_PRECHECK,
    SCHED_PANTHEREYE_UNSTUCK,
    SCHED_PANTHEREYE_GET_BUG,
    SCHED_PANTHEREYE_COVER_FAIL,
    SCHED_PANTHEREYE_GENERIC_FAIL
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
    { TASK_SET_FAIL_SCHEDULE,           (float)SCHED_PANTHEREYE_COVER_FAIL },
    { TASK_PANTHEREYE_FIND_STALK_COVER, 0 },
    { TASK_WALK_PATH,                   0 },
    { TASK_WAIT_FOR_MOVEMENT,           0 },
    { TASK_PANTHEREYE_STALK_WAIT,       0 },
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

Task_t tlPanthereyeChase[] =
{
    { TASK_SET_ACTIVITY,       (float)ACT_IDLE },
    { TASK_SET_FAIL_SCHEDULE,  (float)SCHED_PANTHEREYE_UNSTUCK_PRECHECK },
    { TASK_GET_PATH_TO_ENEMY,  0 },
    { TASK_RUN_PATH,           0 },
    { TASK_WAIT_FOR_MOVEMENT,  0 },
    { TASK_WAIT,               0.15f },
};

Schedule_t slPanthereyeChase[] =
{
    {
        tlPanthereyeChase,
        ARRAYSIZE( tlPanthereyeChase ),
        bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
        bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2 |
        bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE,
        bits_SOUND_DANGER,
        "PanthereyeChase"
    },
};

Task_t tlPanthereyeUnstuckPrecheck[] =
{
    { TASK_STOP_MOVING,                         0 },
    { TASK_SET_FAIL_SCHEDULE,                   (float)SCHED_PANTHEREYE_UNSTUCK },
    { TASK_WAIT_FACE_ENEMY,                     1.8f },
    { TASK_GET_PATH_TO_ENEMY,                   0 },
    { TASK_RUN_PATH,                            0 },
    { TASK_WAIT_FOR_MOVEMENT,                   0 },
};

Schedule_t slPanthereyeUnstuckPrecheck[] =
{
    {
        tlPanthereyeUnstuckPrecheck,
        ARRAYSIZE( tlPanthereyeUnstuckPrecheck ),
        bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
        bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2 |
        bits_COND_HEAVY_DAMAGE,
        bits_SOUND_DANGER,
        "PanthereyeUnstuckPrecheck"
    },
};

Task_t tlPanthereyeUnstuck[] =
{
    { TASK_STOP_MOVING,                         0 },
    { TASK_SET_FAIL_SCHEDULE,                   (float)SCHED_COMBAT_FACE },
    { TASK_PANTHEREYE_FIND_UNSTUCK_DIRECTION,   0 },
    { TASK_FACE_IDEAL,                          0 },
    { TASK_PANTHEREYE_UNSTUCK_LEAP,             0 },
};

Schedule_t slPanthereyeUnstuck[] =
{
    {
        tlPanthereyeUnstuck,
        ARRAYSIZE( tlPanthereyeUnstuck ),
        bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
        bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2,
        bits_SOUND_DANGER,
        "PanthereyeUnstuckLeap"
    },
};

Task_t tlPanthereyeGetBug[] =
{
    { TASK_SET_FAIL_SCHEDULE,       (float)SCHED_COMBAT_FACE },
    { TASK_STOP_MOVING,             0 },
    { TASK_FACE_IDEAL,              0 },
    { TASK_PANTHEREYE_GET_BUG_ANIM, 0 },
};

Schedule_t slPanthereyeGetBug[] =
{
    {
        tlPanthereyeGetBug,
        ARRAYSIZE( tlPanthereyeGetBug ),
        bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2 |
        bits_COND_HEAVY_DAMAGE,
        0,
        "PanthereyeGetBug"
    },
};

Task_t tlPanthereyeGetIntoCirclingRange[] =
{
    { TASK_SET_FAIL_SCHEDULE,       (float)SCHED_PANTHEREYE_GENERIC_FAIL },
    { TASK_PANTHEREYE_NORMALMOVE,   260.0f },
    { TASK_RUN_PATH,                0 },
    { TASK_WAIT_FOR_MOVEMENT,       0 },
};

Schedule_t slPanthereyeGetIntoCirclingRange[] =
{
    {
        tlPanthereyeGetIntoCirclingRange,
        ARRAYSIZE( tlPanthereyeGetIntoCirclingRange ),
        bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
        bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK1 |
        bits_COND_CAN_RANGE_ATTACK2 | bits_COND_CAN_MELEE_ATTACK2 |
        bits_COND_TASK_FAILED | bits_COND_HEAR_SOUND |
        bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE,
        bits_SOUND_DANGER,
        "PanthereyeGetIntoCirclingRange"
    },
};

Task_t tlPanthereyeCoverFail[] =
{
    { TASK_PANTHEREYE_COVER_FAIL_WAIT, 0 },
};

Schedule_t slPanthereyeCoverFail[] =
{
    {
        tlPanthereyeCoverFail,
        ARRAYSIZE( tlPanthereyeCoverFail ),
        bits_COND_CAN_ATTACK | bits_COND_HEAVY_DAMAGE,
        0,
        "PanthereyeCoverFail"
    },
};

Task_t tlPanthereyeGenericFail[] =
{
    { TASK_SET_FAIL_SCHEDULE, (float)SCHED_PANTHEREYE_GENERIC_FAIL },
    { TASK_STOP_MOVING,       0 },
    { TASK_WAIT,              0.4f },
    { TASK_WAIT_PVS,           0 },
};

Schedule_t slPanthereyeGenericFail[] =
{
    {
        tlPanthereyeGenericFail,
        ARRAYSIZE( tlPanthereyeGenericFail ),
        bits_COND_HEAVY_DAMAGE,
        0,
        "PanthereyeGenericFail"
    },
};

// The original replaces the stock cover schedules so that its own fail and
// wait behavior remains consistent across enemy-, origin-, and sound-cover.
Task_t tlPanthereyeTakeCoverFromEnemy[] =
{
    { TASK_SET_FAIL_SCHEDULE,       (float)SCHED_PANTHEREYE_COVER_FAIL },
    { TASK_STOP_MOVING,             0 },
    { TASK_WAIT,                    0.2f },
    { TASK_FIND_COVER_FROM_ENEMY,   0 },
    { TASK_RUN_PATH,                0 },
    { TASK_WAIT_FOR_MOVEMENT,       0 },
    { TASK_REMEMBER,                (float)bits_MEMORY_INCOVER },
    { TASK_FACE_ENEMY,              0 },
    { TASK_WAIT,                    1.0f },
};

Schedule_t slPanthereyeTakeCoverFromEnemy[] =
{
    {
        tlPanthereyeTakeCoverFromEnemy,
        ARRAYSIZE( tlPanthereyeTakeCoverFromEnemy ),
        bits_COND_NEW_ENEMY | bits_COND_HEAVY_DAMAGE,
        0,
        "PanthereyeTakeCoverFromEnemy"
    },
};

Task_t tlPanthereyeTakeCoverFromOrigin[] =
{
    { TASK_SET_FAIL_SCHEDULE,       (float)SCHED_PANTHEREYE_COVER_FAIL },
    { TASK_STOP_MOVING,             0 },
    { TASK_FIND_COVER_FROM_ORIGIN,  0 },
    { TASK_RUN_PATH,                0 },
    { TASK_WAIT_FOR_MOVEMENT,       0 },
    { TASK_REMEMBER,                (float)bits_MEMORY_INCOVER },
    { TASK_TURN_LEFT,               179.0f },
};

Schedule_t slPanthereyeTakeCoverFromOrigin[] =
{
    {
        tlPanthereyeTakeCoverFromOrigin,
        ARRAYSIZE( tlPanthereyeTakeCoverFromOrigin ),
        bits_COND_NEW_ENEMY | bits_COND_HEAVY_DAMAGE,
        0,
        "PanthereyeTakeCoverFromOrigin"
    },
};

Task_t tlPanthereyeTakeCoverFromBestSound[] =
{
    { TASK_SET_FAIL_SCHEDULE,           (float)SCHED_PANTHEREYE_COVER_FAIL },
    { TASK_STOP_MOVING,                 0 },
    { TASK_FIND_COVER_FROM_BEST_SOUND,  0 },
    { TASK_RUN_PATH,                    0 },
    { TASK_WAIT_FOR_MOVEMENT,           0 },
    { TASK_REMEMBER,                    (float)bits_MEMORY_INCOVER },
    { TASK_TURN_LEFT,                   179.0f },
};

Schedule_t slPanthereyeTakeCoverFromBestSound[] =
{
    {
        tlPanthereyeTakeCoverFromBestSound,
        ARRAYSIZE( tlPanthereyeTakeCoverFromBestSound ),
        bits_COND_NEW_ENEMY | bits_COND_HEAVY_DAMAGE,
        0,
        "PanthereyeTakeCoverFromBestSound"
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

static float PantherFacingDot( CBaseEntity *pObserver, const Vector &vecLookAt )
{
    Vector vecDirection;
    float flYaw;
    float flRadians;

    if( !pObserver )
        return -1.0f;

    vecDirection = vecLookAt - pObserver->pev->origin;
    vecDirection.z = 0.0f;

    if( vecDirection.Length() < 1.0f )
        return 0.0f;

    vecDirection = vecDirection.Normalize();

    // Absolute Zero's UTIL_IsFacing uses v_angle for players and angles for
    // non-player entities. FL_CLIENT is the stock GoldSrc player marker.
    flYaw = ( pObserver->pev->flags & FL_CLIENT ) ?
        pObserver->pev->v_angle.y : pObserver->pev->angles.y;
    flRadians = flYaw * 0.0174532925199433f;

    return cos( flRadians ) * vecDirection.x +
           sin( flRadians ) * vecDirection.y;
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
    DEFINE_FIELD( CPantherEye, m_flNextStalkMove, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flLastDamageReact, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flPissedRunUntil, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flSecondClawHitTime, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flBugEventTime, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flBugCooldown, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_flNextPathTry, FIELD_TIME ),
    DEFINE_FIELD( CPantherEye, m_bWaitingForPath, FIELD_BOOLEAN ),
    DEFINE_FIELD( CPantherEye, m_bGetBugRequested, FIELD_BOOLEAN ),
    DEFINE_FIELD( CPantherEye, m_bUnstuckLeap, FIELD_BOOLEAN ),
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
    m_flNextStalkMove = 0.0f;
    m_flLastDamageReact = -1.0f;
    m_flPissedRunUntil = -1.0f;
    m_flSecondClawHitTime = 0.0f;
    m_flBugEventTime = 0.0f;
    m_flBugCooldown = 0.0f;
    m_flNextPathTry = 0.0f;
    m_bWaitingForPath = FALSE;
    m_bGetBugRequested = FALSE;
    m_bUnstuckLeap = FALSE;
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
    m_flNextStalkMove = 0.0f;
    m_flLastDamageReact = -1.0f;
    m_flPissedRunUntil = -1.0f;
    m_flSecondClawHitTime = 0.0f;
    m_flBugEventTime = 0.0f;
    m_flBugCooldown = 0.0f;
    m_flNextPathTry = gpGlobals->time;
    m_bWaitingForPath = FALSE;
    m_bGetBugRequested = FALSE;
    m_bUnstuckLeap = FALSE;
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

void CPantherEye::SetObjectCollisionBox( void )
{
    // Original model-specific bounds; the death animation sprawls much wider.
    if( pev->deadflag != DEAD_NO )
    {
        pev->absmin = pev->origin + Vector( -76, -76, 0 );
        pev->absmax = pev->origin + Vector( 76, 76, 60 );
    }
    else
    {
        pev->absmin = pev->origin + Vector( -35, -35, 0 );
        pev->absmax = pev->origin + Vector( 35, 35, 42 );
    }
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
    int iSkill = (int)CVAR_GET_FLOAT( "skill" );

    if( NewActivity != ACT_MELEE_ATTACK1 )
        m_flSecondClawHitTime = 0.0f;

    switch( NewActivity )
    {
    case ACT_IDLE:
        // Original weights are 7:3. Sneaking uses subtle_motion / idle_figit;
        // an ordinary idle uses itch / shakes.
        if( m_bStalking )
            pszSequence = ( RANDOM_LONG( 1, 10 ) <= 7 ) ? "subtle_motion" : "idle_figit";
        else
            pszSequence = ( RANDOM_LONG( 1, 10 ) <= 7 ) ? "itch" : "shakes";
        break;

    case ACT_WALK:
        pszSequence = "walk";
        break;

    case ACT_RUN:
        if( gpGlobals->time < m_flRunUntil )
        {
            pszSequence = "run";
            flRate = ( iSkill >= 3 ) ? 1.10f : ( iSkill == 2 ) ? 1.05f : 1.00f;
        }
        else
        {
            pszSequence = "walk";
            flRate = ( iSkill >= 3 ) ? 1.19f : ( iSkill == 2 ) ? 1.13f : 1.08f;
        }
        break;

    case ACT_MELEE_ATTACK1:
    {
        int iAttack = RANDOM_LONG( 0, 2 );
        if( iAttack == 0 )
        {
            pszSequence = "attack_primary";
            m_flSecondClawHitTime = gpGlobals->time + ( 12.0f / 15.0f );
        }
        else if( iAttack == 1 )
            pszSequence = "attack_main_claw";
        else
            pszSequence = "attack_simple_claw";

        flRate = ( iSkill >= 3 ) ? 1.45f : ( iSkill == 2 ) ? 1.30f : 1.22f;
        break;
    }

    case ACT_MELEE_ATTACK2:
        pszSequence = "crouch_to_jump";
        flRate = 0.92f;
        break;

    case ACT_SMALL_FLINCH:
    case ACT_BIG_FLINCH:
        CBaseMonster::SetActivity( NewActivity );
        pev->framerate = ( iSkill >= 3 ) ? 2.0f : ( iSkill == 2 ) ? 1.6f : 1.0f;
        return;

    case ACT_TURN_LEFT:
    case ACT_TURN_RIGHT:
        CBaseMonster::SetActivity( NewActivity );
        pev->framerate = 2.1f;
        return;

    case ACT_DIEVIOLENT:
        pszSequence = "death_violent";
        break;

    default:
        break;
    }

    if( pszSequence && SetPantherSequence( pszSequence, NewActivity, flRate ) )
        return;

    // Some model conversions omit optional variants. Fall back to known
    // Panthereye sequences instead of leaving the monster without animation.
    if( NewActivity == ACT_IDLE && m_bStalking &&
        SetPantherSequence( "subtle_motion", NewActivity, 1.0f ) )
        return;

    if( NewActivity == ACT_MELEE_ATTACK1 &&
        SetPantherSequence( "attack_main_claw", NewActivity, flRate ) )
    {
        m_flSecondClawHitTime = 0.0f;
        return;
    }

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

    if( tr.pHit )
    {
        CBaseEntity *pHit = CBaseEntity::Instance( tr.pHit );
        if( pHit && pHit != this && IRelationship( pHit ) > R_NO )
            return TRUE;
    }

    // The second, higher trace permits a leap over low cover while still
    // rejecting a solid wall between the Panther Eye and its target.
    vecStart = pev->origin + Vector( 0, 0, 80 );
    vecEnd = m_vecEnemyLKP + Vector( 0, 0, 40 );
    UTIL_TraceHull( vecStart, vecEnd, dont_ignore_monsters, large_hull, edict(), &tr );

    if( tr.flFraction >= 1.0f || tr.pHit == m_hEnemy->edict() )
        return TRUE;

    if( tr.pHit )
    {
        CBaseEntity *pHit = CBaseEntity::Instance( tr.pHit );
        if( pHit && pHit != this && IRelationship( pHit ) > R_NO )
            return TRUE;
    }

    return FALSE;
}

BOOL CPantherEye::CanLeapForward( void )
{
    TraceResult tr;
    Vector vecStart;
    Vector vecEnd;

    UTIL_MakeVectors( pev->angles );
    vecStart = pev->origin + pev->view_ofs;
    vecEnd = vecStart + gpGlobals->v_forward * 400.0f;

    UTIL_TraceHull( vecStart, vecEnd, dont_ignore_monsters, large_hull, edict(), &tr );

    if( tr.flFraction >= 1.0f )
        return TRUE;

    if( tr.pHit )
    {
        CBaseEntity *pHit = CBaseEntity::Instance( tr.pHit );
        if( pHit && pHit != this && IRelationship( pHit ) > R_NO )
            return TRUE;
    }

    return FALSE;
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

    case SCHED_CHASE_ENEMY:
    case SCHED_PANTHEREYE_CHASE:
    {
        // Absolute Zero's ChaseOrCircle(): when a floor trace below the
        // enemy's last-known position is on roughly the same level, use the
        // 260-unit move-to-enemy-range schedule. Otherwise use the chase.
        if( m_hEnemy != NULL )
        {
            TraceResult trFloor;
            Vector vecFloorStart = m_vecEnemyLKP + Vector( 0, 0, 1 );
            Vector vecFloorEnd = vecFloorStart - Vector( 0, 0, 800 );

            UTIL_TraceLine( vecFloorStart, vecFloorEnd, ignore_monsters,
                            m_hEnemy->edict(), &trFloor );

            if( trFloor.flFraction < 1.0f &&
                ( trFloor.vecEndPos.z - pev->origin.z ) <= 18.0f )
                return &slPanthereyeGetIntoCirclingRange[0];
        }

        return &slPanthereyeChase[0];
    }

    case SCHED_CHASE_ENEMY_FAILED:
    case SCHED_PANTHEREYE_UNSTUCK_PRECHECK:
        return &slPanthereyeUnstuckPrecheck[0];

    case SCHED_PANTHEREYE_UNSTUCK:
        return &slPanthereyeUnstuck[0];

    case SCHED_PANTHEREYE_GET_BUG:
        return &slPanthereyeGetBug[0];

    case SCHED_PANTHEREYE_STALK_WAIT:
        return &slPanthereyeStalkWait[0];

    case SCHED_PANTHEREYE_STALK_MOVE:
        return &slPanthereyeStalkMove[0];

    case SCHED_PANTHEREYE_COVER_FAIL:
        return &slPanthereyeCoverFail[0];

    case SCHED_PANTHEREYE_GENERIC_FAIL:
        return &slPanthereyeGenericFail[0];

    case SCHED_TAKE_COVER_FROM_ENEMY:
        return &slPanthereyeTakeCoverFromEnemy[0];

    case SCHED_TAKE_COVER_FROM_ORIGIN:
        return &slPanthereyeTakeCoverFromOrigin[0];

    case SCHED_TAKE_COVER_FROM_BEST_SOUND:
        return &slPanthereyeTakeCoverFromBestSound[0];

    }

    return CBaseMonster::GetScheduleOfType( Type );
}

Schedule_t *CPantherEye::GetSchedule( void )
{
    if( pev->deadflag == DEAD_NO &&
        m_bGetBugRequested &&
        gpGlobals->time >= m_flBugCooldown &&
        !HasConditions( bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2 |
                        bits_COND_HEAVY_DAMAGE ) )
    {
        m_bGetBugRequested = FALSE;
        return GetScheduleOfType( SCHED_PANTHEREYE_GET_BUG );
    }

    if( pev->deadflag != DEAD_NO ||
        m_MonsterState != MONSTERSTATE_COMBAT ||
        m_hEnemy == NULL ||
        HasConditions( bits_COND_ENEMY_DEAD | bits_COND_NEW_ENEMY ) )
    {
        return CBaseMonster::GetSchedule();
    }

    // A danger sound takes precedence over stalking and ordinary pursuit.
    // Its source is selected by the shared sound system; the custom schedule
    // then preserves the Panthereye-specific cover-failure response.
    if( HasConditions( bits_COND_HEAR_SOUND ) &&
        ( m_afSoundTypes & bits_SOUND_DANGER ) )
        return GetScheduleOfType( SCHED_TAKE_COVER_FROM_BEST_SOUND );

    if( m_bAmbushLeapRequested )
    {
        m_bAmbushLeapRequested = FALSE;
        return GetScheduleOfType( SCHED_MELEE_ATTACK2 );
    }

    // Preserve the original combat decision order: attacks take priority
    // over chase/retreat schedules when an attack is actually available.
    if( HasConditions( bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_RANGE_ATTACK2 |
                       bits_COND_CAN_MELEE_ATTACK1 ) )
        return CBaseMonster::GetSchedule();

    if( HasConditions( bits_COND_CAN_MELEE_ATTACK2 ) )
        return GetScheduleOfType( SCHED_MELEE_ATTACK2 );

    // Absolute Zero checks the anger timer before the retreat timer.
    if( m_flAggressiveUntil > gpGlobals->time )
        return GetScheduleOfType( SCHED_PANTHEREYE_CHASE );

    if( m_flFleeUntil > gpGlobals->time )
        return GetScheduleOfType( SCHED_TAKE_COVER_FROM_ENEMY );

    if( m_bStalking )
    {
        // Stay hidden while the target is not looking and the move timer has
        // not expired. If noticed, seek another lateral / node cover point.
        if( m_bEnemyLookingAtMe || gpGlobals->time < m_flNextStalkMove )
            return GetScheduleOfType( SCHED_PANTHEREYE_STALK_WAIT );

        return GetScheduleOfType( SCHED_PANTHEREYE_STALK_MOVE );
    }

    return CBaseMonster::GetSchedule();
}

void CPantherEye::StartTask( Task_t *pTask )
{
    // These flags describe the currently running task, not permanent AI state.
    m_bPissable = FALSE;
    m_bCornered = FALSE;

    if( pTask->iTask == TASK_PANTHEREYE_NORMALMOVE )
    {
        // Absolute Zero's TASK_PANTHER_NORMALMOVE follows the live enemy
        // entity instead of treating the 260-unit task value as a stopping
        // distance. The value is used by RunTask as a route-refresh threshold.
        m_hTargetEnt = m_hEnemy;
        if( m_hTargetEnt == NULL )
        {
            TaskFail();
            return;
        }

        if( ( m_hTargetEnt->pev->origin - pev->origin ).Length() < 1.0f )
        {
            TaskComplete();
            return;
        }

        m_vecMoveGoal = m_hTargetEnt->pev->origin;
        if( !MoveToTarget( ACT_WALK, 2.0f ) )
            TaskFail();

        return;
    }

    if( pTask->iTask != TASK_GET_PATH_TO_ENEMY )
        m_bWaitingForPath = FALSE;

    // The original throttles route-building to one attempt every 0.5 seconds.
    // Keep the stock task alive while waiting, then invoke the base task once.
    if( pTask->iTask == TASK_GET_PATH_TO_ENEMY )
    {
        if( gpGlobals->time < m_flNextPathTry )
        {
            m_bWaitingForPath = TRUE;
            TaskBegin();
            return;
        }

        m_bWaitingForPath = FALSE;
        CBaseMonster::StartTask( pTask );
        m_flNextPathTry = gpGlobals->time + 0.5f;
        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_FIND_UNSTUCK_DIRECTION )
    {
        static const float flYawOffsets[8] =
            { 0.0f, 45.0f, -45.0f, 90.0f, -90.0f, 135.0f, -135.0f, 180.0f };
        Vector vecStart = pev->origin + pev->view_ofs;
        Vector vecAngles = pev->angles;
        TraceResult tr;
        int i;

        // Match the source's 0, +45, -45, +90, -90, +135, -135, 180 scan.
        for( i = 0; i < ARRAYSIZE( flYawOffsets ); ++i )
        {
            vecAngles.y = pev->angles.y + flYawOffsets[i];
            while( vecAngles.y < 0.0f ) vecAngles.y += 360.0f;
            while( vecAngles.y >= 360.0f ) vecAngles.y -= 360.0f;

            UTIL_MakeVectors( vecAngles );
            UTIL_TraceLine( vecStart, vecStart + gpGlobals->v_forward * 300.0f,
                            ignore_monsters, edict(), &tr );

            if( tr.flFraction >= 1.0f )
            {
                pev->ideal_yaw = vecAngles.y;
                TaskComplete();
                return;
            }
        }

        TaskFail();
        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_GET_BUG_ANIM )
    {
        if( !SetPantherSequence( "get_bug", ACT_IDLE, 2.3f ) )
        {
            m_flBugCooldown = gpGlobals->time + RANDOM_FLOAT( 6.0f, 15.0f );
            TaskComplete();
            return;
        }

        m_flBugEventTime = gpGlobals->time + ( 36.0f / 15.0f );
        TaskBegin();
        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_UNSTUCK_LEAP )
    {
        if( !CanLeapForward() )
        {
            TaskFail();
            return;
        }

        SetActivity( ACT_MELEE_ATTACK2 );
        m_flLeapLaunchTime = gpGlobals->time + ( 10.35f / 15.0f );
        m_bLeapLaunched = FALSE;
        m_bUnstuckLeap = TRUE;
        TaskBegin();
        return;
    }

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
            FindCover( vecThreat, vecViewOffset, 0.0f, CoverRadius() ) )
        {
            m_flNextStalkMove = gpGlobals->time + RANDOM_FLOAT( 5.0f, 11.0f );
            m_movementActivity = ACT_WALK;
            m_bCornered = FALSE;
            m_bPissable = FALSE;
            TaskComplete();
            return;
        }

        // As in Absolute Zero, failure to find either lateral or node cover
        // routes to the cornered wait rather than inventing a third route.
        // If no concealed route can be found, the Panther Eye is effectively
        // cornered. That makes it more likely to turn and fight when noticed.
        m_flNextStalkMove = gpGlobals->time + RANDOM_FLOAT( 4.0f, 8.0f );
        m_bCornered = TRUE;
        m_bPissable = TRUE;
        TaskFail();
        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_STALK_WAIT ||
        pTask->iTask == TASK_PANTHEREYE_COVER_FAIL_WAIT )
    {
        float flDistance = m_hEnemy ?
            ( m_vecEnemyLKP - pev->origin ).Length() : 99999.0f;

        if( pTask->iTask == TASK_PANTHEREYE_COVER_FAIL_WAIT )
            m_bCornered = TRUE;

        // Source behavior: start with a normal 5-11 second pause. If the
        // target has stared for >2.6 seconds (or the Panther is cornered)
        // inside 350 units, it becomes aggressive immediately.
        m_flStalkWaitUntil = gpGlobals->time + RANDOM_FLOAT( 5.0f, 11.0f );

        if( m_hEnemy && !HasConditions( bits_COND_ENEMY_OCCLUDED ) &&
            m_bEnemyLookingAtMe &&
            ( ( m_bCornered && flDistance < 7000.0f ) ||
              ( m_flStareTime > 2.6f && flDistance < 7000.0f ) ) )
        {
            m_flAggressiveUntil = gpGlobals->time + 5.0f;
            m_flFleeUntil = 0.0f;
            m_flRunUntil = gpGlobals->time + 12.0f;
            m_bAmbushLeapRequested = FALSE;
            ChangeSchedule( GetSchedule() );
            return;
        }

        m_bPissable = TRUE;
        m_flStalkWaitUntil = gpGlobals->time + RANDOM_FLOAT( 4.0f, 8.0f );
        m_flNextStalkMove = m_flStalkWaitUntil;
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
    if( pTask->iTask == TASK_GET_PATH_TO_ENEMY && m_bWaitingForPath )
    {
        if( gpGlobals->time < m_flNextPathTry )
            return;

        m_bWaitingForPath = FALSE;
        CBaseMonster::StartTask( pTask );
        m_flNextPathTry = gpGlobals->time + 0.5f;
        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_NORMALMOVE )
    {
        float flDistance;
        float flTargetDiscrepancy;

        // The source updates the target handle every think because enemies
        // may move while this schedule is active. Keep the route aimed at the
        // current enemy position instead of following a stale point.
        m_hTargetEnt = m_hEnemy;
        if( m_hTargetEnt == NULL )
        {
            TaskFail();
            return;
        }

        flDistance = ( m_vecMoveGoal - pev->origin ).Length2D();
        flTargetDiscrepancy =
            ( m_vecMoveGoal - m_hTargetEnt->pev->origin ).Length();

        m_vecMoveGoal = m_hTargetEnt->pev->origin;
        FRefreshRoute();

        // Preserve Absolute Zero's 230-unit near-goal refresh and its
        // task-data-based moved-target threshold (260 * 0.5 by default).
        if( flDistance < 230.0f ||
            flTargetDiscrepancy > pTask->flData * 0.5f )
        {
            m_vecMoveGoal = m_hTargetEnt->pev->origin;
            flDistance = ( m_vecMoveGoal - pev->origin ).Length2D();
        }

        m_movementActivity = ACT_RUN;
        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_GET_BUG_ANIM )
    {
        if( m_flBugEventTime > 0.0f && gpGlobals->time >= m_flBugEventTime )
        {
            Vector vecSearchStart;
            CBaseEntity *pEntityScan = NULL;

            UTIL_MakeVectors( pev->angles );
            vecSearchStart = pev->origin + gpGlobals->v_forward * 40.0f;

            while( ( pEntityScan = UTIL_FindEntityInSphere( pEntityScan, vecSearchStart, 40.0f ) ) != NULL )
            {
                CBaseMonster *pBug = pEntityScan->MyMonsterPointer();
                if( pBug && FClassnameIs( pBug->pev, "monster_cockroach" ) )
                {
                    pBug->TakeDamage( pev, pev, pBug->pev->health, DMG_CRUSH );
                    break;
                }
            }

            m_flBugEventTime = 0.0f;
            m_flBugCooldown = gpGlobals->time + RANDOM_FLOAT( 6.0f, 15.0f );
        }

        if( m_fSequenceFinished )
        {
            TaskComplete();
            return;
        }

        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_STALK_WAIT ||
        pTask->iTask == TASK_PANTHEREYE_COVER_FAIL_WAIT )
    {
        if( m_hEnemy == NULL ||
            m_flAggressiveUntil > gpGlobals->time ||
            m_flFleeUntil > gpGlobals->time ||
            ( !m_bCornered && m_bEnemyLookingAtMe ) ||
            gpGlobals->time >= m_flStalkWaitUntil )
        {
            TaskComplete();
            return;
        }

        return;
    }

    if( pTask->iTask == TASK_PANTHEREYE_LEAP ||
        pTask->iTask == TASK_PANTHEREYE_UNSTUCK_LEAP )
    {
        if( !m_bLeapLaunched && gpGlobals->time >= m_flLeapLaunchTime )
            LaunchLeap( pTask->iTask == TASK_PANTHEREYE_LEAP );

        // The original remains in the crouch-to-jump activity until it has
        // landed. A small forward nudge helps it catch low ledges in midair.
        if( !FBitSet( pev->flags, FL_ONGROUND ) )
        {
            UTIL_MakeVectors( pev->angles );
            if( pev->velocity.Length2D() < 24.0f )
                pev->velocity = pev->velocity + gpGlobals->v_forward * 8.0f;
        }
        else if( m_fSequenceFinished )
        {
            SetTouch( NULL );
            m_bUnstuckLeap = FALSE;
            TaskComplete();
        }
        return;
    }

    CBaseMonster::RunTask( pTask );
}

void CPantherEye::LaunchLeap( BOOL bEnemyAccurate )
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

    if( bEnemyAccurate && m_hEnemy )
        vecTarget = m_vecEnemyLKP + m_hEnemy->pev->view_ofs;
    else
    {
        // The original unstuck jump follows the selected ideal yaw and probes
        // a point roughly 400 units ahead instead of tracking enemy position.
        UTIL_MakeVectors( pev->angles );
        vecTarget = pev->origin + gpGlobals->v_forward * 400.0f;
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

    m_flRunUntil = gpGlobals->time + 12.0f;

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

void CPantherEye::DoClawHit( float flDamageScale, BOOL bKnockback )
{
    CBaseEntity *pHurt;

    UTIL_MakeVectors( pev->angles );
    pHurt = CheckTraceHullAttack( 84.0f * 1.2f,
                                  (int)( m_flClawDamage * flDamageScale ),
                                  DMG_SLASH );

    if( pHurt )
    {
        // Stock GoldSrc equivalent of the original punch / lateral knockback.
        if( bKnockback && ( pHurt->pev->flags & ( FL_MONSTER | FL_CLIENT ) ) )
        {
            pHurt->pev->punchangle.z = -18;
            pHurt->pev->punchangle.x = 5;
            pHurt->pev->velocity = pHurt->pev->velocity - gpGlobals->v_right * 100.0f;
        }

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
}

void CPantherEye::HandleAnimEvent( MonsterEvent_t *pEvent )
{
    if( pEvent->event == 1 )
    {
        DoClawHit( 1.0f, TRUE );
        return;
    }

    CBaseMonster::HandleAnimEvent( pEvent );
}

void CPantherEye::PrescheduleThink( void )
{
    float flCloak;
    float flTargetRender;
    BOOL fCanCloak = FALSE;
    BOOL fForceScheduleChange = FALSE;
    BOOL fAggressionExpired = FALSE;
    BOOL fFleeExpired = FALSE;

    // The original starts or refreshes a 12-second alert/run memory whenever
    // combat or player sounds are heard.
    if( m_afSoundTypes & ( bits_SOUND_COMBAT | bits_SOUND_PLAYER ) )
    {
        m_flPissedRunUntil = gpGlobals->time + 12.0f;
        m_flRunUntil = gpGlobals->time + 12.0f;

        if( pev->sequence == LookupSequence( "walk" ) )
            SetActivity( ACT_RUN );
    }

    if( m_hEnemy != NULL )
    {
        if( m_flPissedRunUntil != -1.0f &&
            gpGlobals->time >= m_flPissedRunUntil &&
            !HasConditions( bits_COND_SEE_ENEMY | bits_COND_NEW_ENEMY ) )
        {
            // Stock GoldSrc has no matching ForgetEnemy() helper. Clear the
            // active enemy and its visibility conditions, then return to alert.
            m_flPissedRunUntil = -1.0f;
            m_hEnemy = NULL;
            ClearConditions( bits_COND_SEE_ENEMY | bits_COND_ENEMY_OCCLUDED |
                             bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD );
            m_flAggressiveUntil = 0.0f;
            m_flFleeUntil = 0.0f;
            m_flStareTime = 0.0f;
            m_bEnemyLookingAtMe = FALSE;
            m_bStalking = FALSE;
            m_bAmbushLeapRequested = FALSE;
            m_bCornered = FALSE;
            m_bPissable = FALSE;
            SetState( MONSTERSTATE_ALERT );
            CBaseMonster::PrescheduleThink();
            return;
        }

        if( gpGlobals->time < m_flPissedRunUntil )
        {
            if( HasConditions( bits_COND_SEE_ENEMY ) )
                m_flPissedRunUntil = gpGlobals->time + 12.0f;
        }
        else if( ( m_vecEnemyLKP - pev->origin ).Length() < 500.0f )
        {
            m_flPissedRunUntil = gpGlobals->time + 12.0f;
        }
    }

    // Timed follow-up swipe from attack_primary.
    if( m_flSecondClawHitTime > 0.0f && gpGlobals->time >= m_flSecondClawHitTime )
    {
        if( pev->deadflag == DEAD_NO &&
            m_Activity == ACT_MELEE_ATTACK1 &&
            pev->sequence == LookupSequence( "attack_primary" ) )
        {
            DoClawHit( 0.5f, TRUE );
        }

        m_flSecondClawHitTime = 0.0f;
    }

    // The original recalculates this every think. Facing is evaluated against
    // v_angle for players and angles for other entities.
    m_bEnemyLookingAtMe = FALSE;

    // The threat/stalking state machine only runs while alive, grounded, and
    // outside the crouch-to-jump sequence. Airborne leap state is left alone.
    if( pev->deadflag == DEAD_NO &&
        FBitSet( pev->flags, FL_ONGROUND ) &&
        pev->sequence != LookupSequence( "crouch_to_jump" ) )
    {
        const float flNow = gpGlobals->time;
        BOOL fPissOff = FALSE;
        BOOL fPissOffHard = FALSE;
        BOOL fTryRunAway = FALSE;
        BOOL fEnemyFacingMe = FALSE;
        BOOL fEnemyFacingAway = FALSE;
        BOOL fCanObserveEnemy = ( m_hEnemy != NULL &&
                                  !HasConditions( bits_COND_ENEMY_OCCLUDED ) );
        float flDistance = 99999.0f;
        float flLookRange = 115.0f * 20.0f;
        float flDistancePissRange = 70.0f * 20.0f;
        float flFightOrFlightRange = 400.0f * 20.0f;
        float flFacingDot = -1.0f;
        float flHealthFactor = 1.0f;

        fAggressionExpired = ( m_flAggressiveUntil > 0.0f &&
                               m_flAggressiveUntil <= flNow );
        fFleeExpired = ( m_flFleeUntil > 0.0f &&
                         m_flFleeUntil <= flNow );

        if( fAggressionExpired )
            m_flAggressiveUntil = 0.0f;
        if( fFleeExpired )
            m_flFleeUntil = 0.0f;

        m_bStalking = ( m_hEnemy != NULL &&
                        m_flAggressiveUntil <= flNow &&
                        m_flFleeUntil <= flNow );

        if( m_hEnemy != NULL )
        {
            flDistance = ( m_vecEnemyLKP - pev->origin ).Length();

            if( m_bCornered )
            {
                flLookRange = 340.0f * 20.0f;
                flDistancePissRange = 230.0f * 20.0f;
                flFightOrFlightRange = 1600.0f * 20.0f;
            }
            else if( m_bPissable )
            {
                flLookRange = 200.0f * 20.0f;
                flDistancePissRange = 80.0f * 20.0f;
                flFightOrFlightRange = 500.0f * 20.0f;
            }

            if( pev->max_health > 0.0f )
                flHealthFactor += ( 1.0f - pev->health / pev->max_health ) * 0.6f;
            flLookRange *= flHealthFactor;

            if( fCanObserveEnemy )
            {
                flFacingDot = PantherFacingDot( m_hEnemy, pev->origin );
                m_bEnemyLookingAtMe = ( flFacingDot >= 0.70f ); // 1 - 0.30
                fEnemyFacingMe = ( flFacingDot >= 0.80f );       // 1 - 0.20

                // Absolute Zero's panthereyeJumpDotTol default is 1.2:
                // dot <= -1 + 1.2 means facing away or sufficiently sideways.
                fEnemyFacingAway = ( flFacingDot <= 0.20f );

                if( m_bEnemyLookingAtMe )
                {
                    m_flStareTime += 0.1f;

                    if( m_bCornered ||
                        m_pSchedule == &slPanthereyeStalkWait[0] )
                    {
                        MakeIdealYaw( m_vecEnemyLKP );
                    }
                }
                else
                {
                    m_flStareTime = 0.0f;
                }
            }
            else
            {
                m_flStareTime = 0.0f;
            }

            if( m_flAggressiveUntil <= flNow )
            {
                if( fCanObserveEnemy && fEnemyFacingMe )
                {
                    if( flDistance < flLookRange )
                        fPissOff = TRUE;
                }
                else if( fCanObserveEnemy && fEnemyFacingAway )
                {
                    // The original jumps when the target's back is turned and
                    // it is between 130 and 360 units away.
                    if( flDistance < 360.0f * 20.0f )
                    {
                        if( flDistance < 130.0f * 20.0f )
                        {
                            fPissOff = TRUE;
                        }
                        else if( gpGlobals->time >= m_flNextLeapAttack &&
                                 CanLeapToEnemy() )
                        {
                            if( m_pSchedule != &slPanthereyeLeap[0] )
                            {
                                RouteClear();
                                MakeIdealYaw( m_vecEnemyLKP );
                                ChangeSchedule( &slPanthereyeLeap[0] );
                                CBaseMonster::PrescheduleThink();
                                return;
                            }
                        }
                    }
                }
                else if( flDistance < flDistancePissRange )
                {
                    fPissOff = TRUE;
                }
            }
            else if( fCanObserveEnemy &&
                     HasConditions( bits_COND_SEE_ENEMY ) &&
                     flDistance < 300.0f * 20.0f )
            {
                // Once angered, seeing the target within 300 units sustains it.
                fPissOff = TRUE;
            }

            // Damage causes fight-or-flight. Keep the original condition
            // precedence: heavy damage always qualifies; light damage does
            // so only when the ordinary proximity/facing test did not.
            if( ( !fPissOff && HasConditions( bits_COND_LIGHT_DAMAGE ) ) ||
                HasConditions( bits_COND_HEAVY_DAMAGE ) )
            {
                if( flDistance < flFightOrFlightRange )
                    fPissOffHard = TRUE;
                else
                    fTryRunAway = TRUE;
            }

            if( fTryRunAway )
            {
                BOOL fStartedRunningAway = ( m_flFleeUntil <= flNow );
                m_flFleeUntil = flNow + 7.0f;

                // As in the original, a new retreat only changes schedules
                // immediately when there is no active anger timer.
                if( fStartedRunningAway && m_flAggressiveUntil <= flNow )
                    fForceScheduleChange = TRUE;
            }

            if( fPissOffHard || fPissOff )
            {
                BOOL fStartedAnger = ( m_flAggressiveUntil <= flNow );

                if( fPissOffHard )
                    m_flAggressiveUntil = max( m_flAggressiveUntil, flNow + 8.0f );
                else
                    m_flAggressiveUntil = max( m_flAggressiveUntil, flNow + 3.0f );

                m_flFleeUntil = 0.0f;
                m_flRunUntil = flNow + 12.0f;
                m_bAmbushLeapRequested = FALSE;

                if( fStartedAnger )
                    fForceScheduleChange = TRUE;
            }

            if( m_flFleeUntil <= flNow )
                m_flFleeUntil = 0.0f;
            if( m_flAggressiveUntil <= flNow )
                m_flAggressiveUntil = 0.0f;

            m_bStalking = ( m_flAggressiveUntil <= flNow &&
                            m_flFleeUntil <= flNow );

            // If the target notices the Panther while it is moving between
            // cover positions, abandon that route. Its next schedule is
            // selected from the state just computed above.
            if( m_pSchedule == &slPanthereyeStalkMove[0] &&
                ( m_bEnemyLookingAtMe ||
                  m_flAggressiveUntil > flNow ||
                  m_flFleeUntil > flNow ) )
            {
                TaskFail();
            }
        }
        else
        {
            // No enemy: the original clears both threat timers and stare memory.
            m_flAggressiveUntil = 0.0f;
            m_flFleeUntil = 0.0f;
            m_flStareTime = 0.0f;
            m_bStalking = FALSE;
            m_bAmbushLeapRequested = FALSE;
        }

        // Expired anger/retreat timers explicitly release their corresponding
        // schedules in the source; don't make the monster finish a stale chase
        // or cover route before it returns to stalking.
        if( fAggressionExpired && m_pSchedule == &slPanthereyeChase[0] )
            fForceScheduleChange = TRUE;

        if( fFleeExpired &&
            ( m_pSchedule == &slPanthereyeTakeCoverFromEnemy[0] ||
              m_pSchedule == &slPanthereyeTakeCoverFromBestSound[0] ) )
        {
            fForceScheduleChange = TRUE;
        }

        if( fForceScheduleChange )
            ChangeSchedule( GetSchedule() );
    }
    else if( pev->deadflag != DEAD_NO )
    {
        m_bEnemyLookingAtMe = FALSE;
    }
    else if( m_hEnemy == NULL )
    {
        m_bStalking = FALSE;
        m_bAmbushLeapRequested = FALSE;
        m_flStareTime = 0.0f;
        m_flAggressiveUntil = 0.0f;
        m_flFleeUntil = 0.0f;
    }

    CBaseMonster::PrescheduleThink();

    // Idle cockroach interaction from Absolute Zero.
    if( pev->deadflag == DEAD_NO &&
        m_IdealActivity == ACT_IDLE &&
        m_pSchedule != &slPanthereyeGetBug[0] &&
        gpGlobals->time >= m_flBugCooldown &&
        !HasConditions( bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2 |
                        bits_COND_HEAVY_DAMAGE ) )
    {
        CBaseEntity *pEntityScan = NULL;

        while( ( pEntityScan = UTIL_FindEntityInSphere( pEntityScan, pev->origin, 68.0f ) ) != NULL )
        {
            CBaseMonster *pBug = pEntityScan->MyMonsterPointer();
            if( pBug && FClassnameIs( pBug->pev, "monster_cockroach" ) )
            {
                MakeIdealYaw( pBug->pev->origin );
                m_bGetBugRequested = TRUE;
                break;
            }
        }
    }

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
    // Preserve stock damage/condition handling. PrescheduleThink consumes
    // LIGHT_DAMAGE / HEAVY_DAMAGE and applies the original fight-or-flight rules.
    return CBaseMonster::TakeDamage( pevInflictor, pevAttacker, flDamage, bitsDamageType );
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
    slPanthereyeChase,
    slPanthereyeUnstuckPrecheck,
    slPanthereyeUnstuck,
    slPanthereyeGetBug,
    slPanthereyeGetIntoCirclingRange,
    slPanthereyeCoverFail,
    slPanthereyeGenericFail,
    slPanthereyeTakeCoverFromEnemy,
    slPanthereyeTakeCoverFromOrigin,
    slPanthereyeTakeCoverFromBestSound,
};

IMPLEMENT_CUSTOM_SCHEDULES( CPantherEye, CBaseMonster )

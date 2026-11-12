/***
*
*   Panther Eye - portable SDK adaptation.
*
*   Behavioral reference:
*   Cobalt-57/Half-Life-Absolute-Zero, dlls/entity/panthereye.*
*
*   This implementation deliberately targets the stock CBaseMonster API
*   provided by hlsdk-portable. It does not require Absolute Zero's
*   modified CBaseMonster, hidden-CVar framework, or animation-event queue.
*
****/

#ifndef PANTHEREYE_H
#define PANTHEREYE_H

#include "extdll.h"
#include "cbase.h"
#include "basemonster.h"
#include "monsters.h"

class CPantherEye : public CBaseMonster
{
public:
    CPantherEye( void );

    void Spawn( void );
    void Precache( void );

    int Classify( void );
    int IRelationship( CBaseEntity *pTarget );
    int ISoundMask( void );
    float HearingSensitivity( void );

    void SetYawSpeed( void );
    void SetActivity( Activity NewActivity );
    void HandleAnimEvent( MonsterEvent_t *pEvent );

    BOOL FCanCheckAttacks( void );
    BOOL CheckMeleeAttack1( float flDot, float flDist );
    BOOL CheckMeleeAttack2( float flDot, float flDist );

    void PrescheduleThink( void );
    Schedule_t *GetScheduleOfType( int Type );
    void StartTask( Task_t *pTask );
    void RunTask( Task_t *pTask );

    void IdleSound( void );
    void AlertSound( void );
    void PainSound( void );
    void DeathSound( void );

    void EXPORT LeapTouch( CBaseEntity *pOther );

    virtual int Save( CSave &save );
    virtual int Restore( CRestore &restore );
    static TYPEDESCRIPTION m_SaveData[];

    CUSTOM_SCHEDULES;

private:
    BOOL SetPantherSequence( const char *pszSequence, Activity activity, float flRate );
    BOOL CanLeapToEnemy( void );
    void LaunchLeap( void );
    void AttackSound( void );

    float m_flNextLeapAttack;
    float m_flLeapLaunchTime;
    float m_flRunUntil;
    float m_flClawDamage;
    BOOL m_bLeapLaunched;
};

#endif // PANTHEREYE_H

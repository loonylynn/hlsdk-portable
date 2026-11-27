#ifndef ENV_FOG_H
#define ENV_FOG_H
#include "extdll.h"
void EnvFog_Init( void );
void EnvFog_ResetState( void );
void EnvFog_SendToClient( edict_t *pClient );
#endif // ENV_FOG_H

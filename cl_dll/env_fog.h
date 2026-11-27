#ifndef CLIENT_ENV_FOG_H
#define CLIENT_ENV_FOG_H
int EnvFog_MsgFunc( const char *pszName, int iSize, void *pbuf );
void EnvFog_Apply( void );
void EnvFog_Clear( void );
void EnvFog_RegisterCommands( void );
#endif // CLIENT_ENV_FOG_H

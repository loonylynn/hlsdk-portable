#pragma once
#ifndef WEAPON_FRAMEWORK_WEAPON_H
#define WEAPON_FRAMEWORK_WEAPON_H
#include "weapons.h"
#include "weapon_registry.h"

class CFrameworkWeapon : public CBasePlayerWeapon
{
public:
	CFrameworkWeapon();
	void SetDefinitionId(int id);
	const WeaponFramework::WeaponDefinition *GetDefinition() const;
	void KeyValue(KeyValueData *pkvd);
	void Precache(void);
	void Spawn(void);
	int GetItemInfo(ItemInfo *p);
	int AddToPlayer(CBasePlayer *pPlayer);
	int iItemSlot(void);
	BOOL Deploy(void);
	void PrimaryAttack(void);
	void Reload(void);
	void WeaponIdle(void);
	virtual BOOL UseDecrement(void)
	{
#if CLIENT_WEAPONS
		return TRUE;
#else
		return FALSE;
#endif
	}
private:
	char m_definitionName[WeaponFramework::WeaponNameCapacity];
};

#ifndef CLIENT_DLL
void WeaponFramework_PrecacheAddonWeapons(void);
#endif
#endif

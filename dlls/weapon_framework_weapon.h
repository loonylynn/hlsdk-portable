#pragma once
#ifndef WEAPON_FRAMEWORK_WEAPON_H
#define WEAPON_FRAMEWORK_WEAPON_H
#include "weapons.h"
#include "weapon_registry.h"
#include "weapon_runtime.h"

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
	void Holster(int skiplocal = 0);
	void ItemPostFrame(void);
	void PrimaryAttack(void);
	void Reload(void);
	void WeaponIdle(void);

	// Save-game field descriptors are only compiled into the server DLL.
	virtual int Save(CSave &save);
	virtual int Restore(CRestore &restore);
#ifndef CLIENT_DLL
	static TYPEDESCRIPTION m_SaveData[];
#endif
	virtual BOOL UseDecrement(void)
	{
#if CLIENT_WEAPONS
		return TRUE;
#else
		return FALSE;
#endif
	}
private:
	void ResetRuntimeState(void);
	void SyncRuntimeStateFromLegacy(void);
	void SyncLegacyStateFromRuntime(void);

	char m_definitionName[WeaponFramework::WeaponNameCapacity];
	WeaponFramework::WeaponInstanceState m_runtimeState;
};

#ifndef CLIENT_DLL
void WeaponFramework_PrecacheAddonWeapons(void);
#endif
#endif

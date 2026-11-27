/*
 * Shared weapon identity and metadata definition.
 * Compiled into both the server and client game DLLs.
 */
#pragma once
#ifndef WEAPON_FRAMEWORK_DEFINITION_H
#define WEAPON_FRAMEWORK_DEFINITION_H

#include "cdll_dll.h"

namespace WeaponFramework
{
	enum WeaponActionType { WeaponActionNone = 0, WeaponActionHitscan = 1 };

	enum
	{
		WeaponNameCapacity = 64,
		EntityClassnameCapacity = 64,
		AmmoNameCapacity = 32,
		ModelPathCapacity = 128,
		WeaponBehaviorNameCapacity = 64,
		FirstLegacyWeaponId = 1,
		LastLegacyWeaponId = 16,
		FirstAddonWeaponId = 17,
		LastAddonWeaponId = WEAPON_SUIT - 1
	};

	// Input data used by built-in adapters and the JSON loader.
	struct WeaponDefinitionData
	{
		int id;
		const char *name;
		const char *entityClassname;
		const char *behaviorName;
		int slot;
		int position;
		int weight;
		int flags;
		int clipSize;
		int defaultAmmo;
		const char *primaryAmmoType;
		int primaryAmmoMax;
		int primaryAmmoPickupAmount;
		const char *secondaryAmmoType;
		int secondaryAmmoMax;
		const char *viewModel;
		const char *worldModel;
		const char *playerModel;
		int actionType, bulletType, damage, pellets, range;
		float spread, fireRate, reloadTime;
		bool automatic;
		int drawAnimation, fireAnimation, reloadAnimation, idleAnimation;
		const char *fireSound;
	};

	class WeaponDefinition
	{
	public:
		WeaponDefinition();
		WeaponDefinition(int id, const char *name, const char *entityClassname);
		explicit WeaponDefinition(const WeaponDefinitionData &data);
		int GetId() const;
		const char *GetName() const;
		const char *GetEntityClassname() const;
		const char *GetBehaviorName() const;
		bool IsValid() const;
		int GetSlot() const;
		int GetPosition() const;
		int GetWeight() const;
		int GetFlags() const;
		int GetClipSize() const;
		int GetDefaultAmmo() const;
		const char *GetPrimaryAmmoType() const;
		int GetPrimaryAmmoMax() const;
		int GetPrimaryAmmoPickupAmount() const;
		const char *GetSecondaryAmmoType() const;
		int GetSecondaryAmmoMax() const;
		const char *GetViewModel() const;
		const char *GetWorldModel() const;
		const char *GetPlayerModel() const;
		int GetActionType() const;
		int GetBulletType() const;
		int GetDamage() const;
		int GetPellets() const;
		int GetRange() const;
		float GetSpread() const;
		float GetFireRate() const;
		float GetReloadTime() const;
		bool IsAutomatic() const;
		int GetDrawAnimation() const;
		int GetFireAnimation() const;
		int GetReloadAnimation() const;
		int GetIdleAnimation() const;
		const char *GetFireSound() const;

	private:
		friend class WeaponRegistry;
		int m_id;
		char m_name[WeaponNameCapacity];
		char m_entityClassname[EntityClassnameCapacity];
		char m_behaviorName[WeaponBehaviorNameCapacity];
		int m_slot;
		int m_position;
		int m_weight;
		int m_flags;
		int m_clipSize;
		int m_defaultAmmo;
		char m_primaryAmmoType[AmmoNameCapacity];
		int m_primaryAmmoMax;
		int m_primaryAmmoPickupAmount;
		char m_secondaryAmmoType[AmmoNameCapacity];
		int m_secondaryAmmoMax;
		char m_viewModel[ModelPathCapacity];
		char m_worldModel[ModelPathCapacity];
		char m_playerModel[ModelPathCapacity];
		int m_actionType, m_bulletType, m_damage, m_pellets, m_range;
		float m_spread, m_fireRate, m_reloadTime;
		bool m_automatic;
		int m_drawAnimation, m_fireAnimation, m_reloadAnimation, m_idleAnimation;
		char m_fireSound[ModelPathCapacity];
		bool m_valid;
	};
}

#endif // WEAPON_FRAMEWORK_DEFINITION_H

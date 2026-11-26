/*
 * Shared weapon registry.
 */
#include "weapon_registry.h"

#include <stddef.h>
#include <string.h>
#include "weapons.h"

namespace WeaponFramework
{
	WeaponRegistry::WeaponRegistry()
	{
		Clear();
	}

	bool WeaponRegistry::RegisterLegacy(const WeaponDefinition &definition)
	{
		const int id = definition.GetId();

		if (!definition.IsValid() || id < FirstLegacyWeaponId || id > LastLegacyWeaponId)
			return false;

		return RegisterAtId(definition, id);
	}

	int WeaponRegistry::RegisterAddon(const WeaponDefinition &definition)
	{
		const int id = FindFreeAddonId();

		if (!definition.IsValid() || id == 0 || !IsUnique(definition))
			return 0;

		if (!RegisterAtId(definition, id))
			return 0;

		return id;
	}

	bool WeaponRegistry::RegisterAtId(const WeaponDefinition &definition, int id)
	{
		if (!definition.IsValid() || id <= 0 || id >= MAX_WEAPONS || id == WEAPON_SUIT)
			return false;

		if (m_registered[id] || !IsUnique(definition))
			return false;

		m_definitions[id] = definition;
		m_definitions[id].m_id = id;
		if (!m_definitions[id].IsValid())
			return false;

		m_registered[id] = true;
		return true;
	}

	bool WeaponRegistry::IsUnique(const WeaponDefinition &definition) const
	{
		int id;

		for (id = 1; id < MAX_WEAPONS; ++id)
		{
			if (!m_registered[id])
				continue;

			if (strcmp(m_definitions[id].GetName(), definition.GetName()) == 0)
				return false;

			if (strcmp(m_definitions[id].GetEntityClassname(), definition.GetEntityClassname()) == 0)
				return false;
		}

		return true;
	}

	const WeaponDefinition *WeaponRegistry::FindById(int id) const
	{
		if (id <= 0 || id >= MAX_WEAPONS || !m_registered[id])
			return NULL;

		return &m_definitions[id];
	}

	const WeaponDefinition *WeaponRegistry::FindByName(const char *name) const
	{
		int id;

		if (!name || name[0] == '\0')
			return NULL;

		for (id = 1; id < MAX_WEAPONS; ++id)
			if (m_registered[id] && strcmp(m_definitions[id].GetName(), name) == 0)
				return &m_definitions[id];

		return NULL;
	}

	const WeaponDefinition *WeaponRegistry::FindByEntityClassname(const char *entityClassname) const
	{
		int id;

		if (!entityClassname || entityClassname[0] == '\0')
			return NULL;

		for (id = 1; id < MAX_WEAPONS; ++id)
			if (m_registered[id] && strcmp(m_definitions[id].GetEntityClassname(), entityClassname) == 0)
				return &m_definitions[id];

		return NULL;
	}

	bool WeaponRegistry::IsRegistered(int id) const
	{
		return id > 0 && id < MAX_WEAPONS && id != WEAPON_SUIT && m_registered[id];
	}

	int WeaponRegistry::FindFreeAddonId() const
	{
		int id;

		for (id = FirstAddonWeaponId; id <= LastAddonWeaponId; ++id)
			if (!m_registered[id])
				return id;

		return 0;
	}

	void WeaponRegistry::Clear()
	{
		int id;

		for (id = 0; id < MAX_WEAPONS; ++id)
		{
			m_definitions[id] = WeaponDefinition();
			m_registered[id] = false;
		}
	}

	void WeaponRegistry::ReplaceWith(const WeaponRegistry &other)
	{
		int id;

		for (id = 0; id < MAX_WEAPONS; ++id)
		{
			m_definitions[id] = other.m_definitions[id];
			m_registered[id] = other.m_registered[id];
		}
	}

	WeaponRegistry &GetRegistry()
	{
		static WeaponRegistry registry;
		return registry;
	}

	bool RegisterBuiltinWeapons()
	{
		WeaponDefinitionData glock = {};
		glock.id = WEAPON_GLOCK;
		glock.name = "goldsrc:weapon/glock";
		glock.entityClassname = "weapon_9mmhandgun";
		glock.slot = 1;
		glock.position = 0;
		glock.weight = GLOCK_WEIGHT;
		glock.flags = 0;
		glock.clipSize = GLOCK_MAX_CLIP;
		glock.defaultAmmo = GLOCK_DEFAULT_GIVE;
		glock.primaryAmmoType = "9mm";
		glock.primaryAmmoMax = _9MM_MAX_CARRY;
		glock.primaryAmmoPickupAmount = AMMO_GLOCKCLIP_GIVE;
		glock.secondaryAmmoType = NULL;
		glock.secondaryAmmoMax = -1;
		glock.viewModel = "models/v_9mmhandgun.mdl";
		glock.worldModel = "models/w_9mmhandgun.mdl";
		glock.playerModel = "models/p_9mmhandgun.mdl";
		return GetRegistry().RegisterLegacy(WeaponDefinition(glock));
	}
}

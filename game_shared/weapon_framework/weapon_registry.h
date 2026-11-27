/*
 * Shared weapon registry.
 * Identity metadata only; not weapon execution or networking.
 */
#pragma once
#ifndef WEAPON_FRAMEWORK_REGISTRY_H
#define WEAPON_FRAMEWORK_REGISTRY_H

#include "weapon_definition.h"

namespace WeaponFramework
{
	class WeaponRegistry
	{
	public:
		WeaponRegistry();

		// Register a built-in weapon using its existing ID (1-16).
		bool RegisterLegacy(const WeaponDefinition &definition);

		// Register an add-on weapon and assign an available ID (17-30).
		// Returns its assigned ID, or 0 if registration fails or the range is full.
		int RegisterAddon(const WeaponDefinition &definition);

		const WeaponDefinition *FindById(int id) const;
		const WeaponDefinition *FindByName(const char *name) const;
		const WeaponDefinition *FindByEntityClassname(const char *entityClassname) const;

		bool IsRegistered(int id) const;
		int FindFreeAddonId() const;
		void Clear();
		void ReplaceWith(const WeaponRegistry &other);

	private:
		bool IsUnique(const WeaponDefinition &definition) const;
		bool RegisterAtId(const WeaponDefinition &definition, int id);

		WeaponDefinition m_definitions[MAX_WEAPONS];
		bool m_registered[MAX_WEAPONS];
	};

	// The client and server DLLs each own an instance. Both must load add-ons
	// in the same deterministic order so numeric IDs agree.
	WeaponRegistry &GetRegistry();

	// Register built-in fallback metadata when a definition file is unavailable.
	bool RegisterBuiltinWeapons();
}

#endif // WEAPON_FRAMEWORK_REGISTRY_H

/*
 * Shared weapon identity definition.
 * Compiled into both the server and client game DLLs.
 */
#pragma once
#ifndef WEAPON_FRAMEWORK_DEFINITION_H
#define WEAPON_FRAMEWORK_DEFINITION_H

#include "cdll_dll.h"

namespace WeaponFramework
{
	enum
	{
		WeaponNameCapacity = 64,
		EntityClassnameCapacity = 64,
		FirstLegacyWeaponId = 1,
		LastLegacyWeaponId = 16,
		FirstAddonWeaponId = 17,
		LastAddonWeaponId = WEAPON_SUIT - 1
	};

	class WeaponDefinition
	{
	public:
		WeaponDefinition();
		WeaponDefinition(int id, const char *name, const char *entityClassname);

		int GetId() const;
		const char *GetName() const;
		const char *GetEntityClassname() const;
		bool IsValid() const;

	private:
		int m_id;
		char m_name[WeaponNameCapacity];
		char m_entityClassname[EntityClassnameCapacity];
		bool m_valid;
	};
}

#endif // WEAPON_FRAMEWORK_DEFINITION_H

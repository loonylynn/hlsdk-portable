/*
 * Shared weapon identity definition.
 */
#include "weapon_definition.h"

#include <string.h>

namespace
{
	bool CopyText(char *destination, size_t capacity, const char *source)
	{
		size_t length;

		if (!destination || capacity == 0)
			return false;

		destination[0] = '\0';

		if (!source || source[0] == '\0')
			return false;

		length = strlen(source);
		if (length >= capacity)
			return false;

		memcpy(destination, source, length + 1);
		return true;
	}
}

namespace WeaponFramework
{
	WeaponDefinition::WeaponDefinition()
		: m_id(0), m_valid(false)
	{
		m_name[0] = '\0';
		m_entityClassname[0] = '\0';
	}

	WeaponDefinition::WeaponDefinition(int id, const char *name, const char *entityClassname)
		: m_id(id), m_valid(false)
	{
		const bool validName = CopyText(m_name, sizeof(m_name), name);
		const bool validClassname = CopyText(m_entityClassname, sizeof(m_entityClassname), entityClassname);
		m_valid = validName && validClassname;
	}

	int WeaponDefinition::GetId() const { return m_id; }
	const char *WeaponDefinition::GetName() const { return m_name; }
	const char *WeaponDefinition::GetEntityClassname() const { return m_entityClassname; }
	bool WeaponDefinition::IsValid() const { return m_valid; }
}

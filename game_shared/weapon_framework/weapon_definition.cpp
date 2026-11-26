/*
 * Shared weapon identity and metadata definition.
 */
#include "weapon_definition.h"
#include <stddef.h>
#include <string.h>

namespace
{
	bool CopyRequiredText(char *destination, size_t capacity, const char *source)
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

	bool CopyOptionalText(char *destination, size_t capacity, const char *source)
	{
		if (!destination || capacity == 0)
			return false;
		destination[0] = '\0';
		if (!source || source[0] == '\0')
			return true;
		return CopyRequiredText(destination, capacity, source);
	}
}

namespace WeaponFramework
{
	WeaponDefinition::WeaponDefinition()
		: m_id(0), m_slot(0), m_position(0), m_weight(0), m_flags(0),
		  m_clipSize(-1), m_defaultAmmo(0), m_primaryAmmoMax(-1),
		  m_primaryAmmoPickupAmount(0), m_secondaryAmmoMax(-1), m_valid(false)
	{
		m_name[0] = '\0';
		m_entityClassname[0] = '\0';
		m_primaryAmmoType[0] = '\0';
		m_secondaryAmmoType[0] = '\0';
		m_viewModel[0] = '\0';
		m_worldModel[0] = '\0';
		m_playerModel[0] = '\0';
	}

	WeaponDefinition::WeaponDefinition(int id, const char *name, const char *entityClassname)
		: WeaponDefinition()
	{
		m_id = id;
		m_valid = CopyRequiredText(m_name, sizeof(m_name), name) &&
			CopyRequiredText(m_entityClassname, sizeof(m_entityClassname), entityClassname);
	}

	WeaponDefinition::WeaponDefinition(const WeaponDefinitionData &data)
		: WeaponDefinition()
	{
		m_id = data.id;
		m_slot = data.slot;
		m_position = data.position;
		m_weight = data.weight;
		m_flags = data.flags;
		m_clipSize = data.clipSize;
		m_defaultAmmo = data.defaultAmmo;
		m_primaryAmmoMax = data.primaryAmmoMax;
		m_primaryAmmoPickupAmount = data.primaryAmmoPickupAmount;
		m_secondaryAmmoMax = data.secondaryAmmoMax;

		m_valid =
			CopyRequiredText(m_name, sizeof(m_name), data.name) &&
			CopyRequiredText(m_entityClassname, sizeof(m_entityClassname), data.entityClassname) &&
			CopyOptionalText(m_primaryAmmoType, sizeof(m_primaryAmmoType), data.primaryAmmoType) &&
			CopyOptionalText(m_secondaryAmmoType, sizeof(m_secondaryAmmoType), data.secondaryAmmoType) &&
			CopyOptionalText(m_viewModel, sizeof(m_viewModel), data.viewModel) &&
			CopyOptionalText(m_worldModel, sizeof(m_worldModel), data.worldModel) &&
			CopyOptionalText(m_playerModel, sizeof(m_playerModel), data.playerModel) &&
			m_slot >= 0 && m_slot < MAX_ITEM_TYPES &&
			m_position >= 0 && m_position <= 255 &&
			m_clipSize >= -1 &&
			m_defaultAmmo >= 0 && m_defaultAmmo <= 32767 &&
			((m_primaryAmmoType[0] == '\0' && m_primaryAmmoMax == -1 && m_primaryAmmoPickupAmount == 0) ||
			 (m_primaryAmmoType[0] != '\0' && m_primaryAmmoMax >= 0 &&
			  m_primaryAmmoPickupAmount >= 0 && m_primaryAmmoPickupAmount <= 32767)) &&
			((m_secondaryAmmoType[0] == '\0' && m_secondaryAmmoMax == -1) ||
			 (m_secondaryAmmoType[0] != '\0' && m_secondaryAmmoMax >= 0));
	}

	int WeaponDefinition::GetId() const { return m_id; }
	const char *WeaponDefinition::GetName() const { return m_name; }
	const char *WeaponDefinition::GetEntityClassname() const { return m_entityClassname; }
	bool WeaponDefinition::IsValid() const { return m_valid; }
	int WeaponDefinition::GetSlot() const { return m_slot; }
	int WeaponDefinition::GetPosition() const { return m_position; }
	int WeaponDefinition::GetWeight() const { return m_weight; }
	int WeaponDefinition::GetFlags() const { return m_flags; }
	int WeaponDefinition::GetClipSize() const { return m_clipSize; }
	int WeaponDefinition::GetDefaultAmmo() const { return m_defaultAmmo; }
	const char *WeaponDefinition::GetPrimaryAmmoType() const { return m_primaryAmmoType[0] ? m_primaryAmmoType : NULL; }
	int WeaponDefinition::GetPrimaryAmmoMax() const { return m_primaryAmmoMax; }
	int WeaponDefinition::GetPrimaryAmmoPickupAmount() const { return m_primaryAmmoPickupAmount; }
	const char *WeaponDefinition::GetSecondaryAmmoType() const { return m_secondaryAmmoType[0] ? m_secondaryAmmoType : NULL; }
	int WeaponDefinition::GetSecondaryAmmoMax() const { return m_secondaryAmmoMax; }
	const char *WeaponDefinition::GetViewModel() const { return m_viewModel[0] ? m_viewModel : NULL; }
	const char *WeaponDefinition::GetWorldModel() const { return m_worldModel[0] ? m_worldModel : NULL; }
	const char *WeaponDefinition::GetPlayerModel() const { return m_playerModel[0] ? m_playerModel : NULL; }
}

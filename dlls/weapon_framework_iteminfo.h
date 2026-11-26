/*
 * Shared accessors for JSON-backed stock weapon metadata.
 *
 * This header is included by both the server weapon classes and the client
 * prediction weapon classes. Native attack/reload implementations remain
 * in their existing C++ classes.
 */
#pragma once
#ifndef WEAPON_FRAMEWORK_ITEMINFO_H
#define WEAPON_FRAMEWORK_ITEMINFO_H

#include "weapons.h"
#include "weapon_registry.h"

inline const WeaponFramework::WeaponDefinition *
WeaponFramework_FindLegacyDefinition(int weaponId)
{
	return WeaponFramework::GetRegistry().FindById(weaponId);
}

inline bool WeaponFramework_ApplyItemInfo(ItemInfo *item, int weaponId)
{
	const WeaponFramework::WeaponDefinition *definition =
		WeaponFramework_FindLegacyDefinition(weaponId);

	if (!item || !definition)
		return false;

	// Keep pszName supplied by the native entity. Some stock weapons have
	// legacy aliases (for example weapon_glock / weapon_9mmhandgun).
	item->pszAmmo1 = definition->GetPrimaryAmmoType();
	item->iMaxAmmo1 = definition->GetPrimaryAmmoMax();
	item->pszAmmo2 = definition->GetSecondaryAmmoType();
	item->iMaxAmmo2 = definition->GetSecondaryAmmoMax();
	item->iMaxClip = definition->GetClipSize();
	item->iSlot = definition->GetSlot();
	item->iPosition = definition->GetPosition();
	item->iFlags = definition->GetFlags();
	item->iId = definition->GetId();
	item->iWeight = definition->GetWeight();
	return true;
}

inline int WeaponFramework_GetDefaultAmmo(int weaponId, int fallback)
{
	const WeaponFramework::WeaponDefinition *definition =
		WeaponFramework_FindLegacyDefinition(weaponId);
	return definition ? definition->GetDefaultAmmo() : fallback;
}

inline const char *WeaponFramework_GetPrimaryAmmoType(int weaponId, const char *fallback)
{
	const WeaponFramework::WeaponDefinition *definition =
		WeaponFramework_FindLegacyDefinition(weaponId);
	const char *ammoType = definition ? definition->GetPrimaryAmmoType() : NULL;
	return ammoType ? ammoType : fallback;
}

inline int WeaponFramework_GetPrimaryAmmoMax(int weaponId, int fallback)
{
	const WeaponFramework::WeaponDefinition *definition =
		WeaponFramework_FindLegacyDefinition(weaponId);
	return definition && definition->GetPrimaryAmmoType() ?
		definition->GetPrimaryAmmoMax() : fallback;
}

inline int WeaponFramework_GetPrimaryAmmoPickupAmount(int weaponId, int fallback)
{
	const WeaponFramework::WeaponDefinition *definition =
		WeaponFramework_FindLegacyDefinition(weaponId);
	return definition && definition->GetPrimaryAmmoType() ?
		definition->GetPrimaryAmmoPickupAmount() : fallback;
}

inline const char *WeaponFramework_GetViewModel(int weaponId, const char *fallback)
{
	const WeaponFramework::WeaponDefinition *definition =
		WeaponFramework_FindLegacyDefinition(weaponId);
	const char *model = definition ? definition->GetViewModel() : NULL;
	return model ? model : fallback;
}

inline const char *WeaponFramework_GetWorldModel(int weaponId, const char *fallback)
{
	const WeaponFramework::WeaponDefinition *definition =
		WeaponFramework_FindLegacyDefinition(weaponId);
	const char *model = definition ? definition->GetWorldModel() : NULL;
	return model ? model : fallback;
}

inline const char *WeaponFramework_GetPlayerModel(int weaponId, const char *fallback)
{
	const WeaponFramework::WeaponDefinition *definition =
		WeaponFramework_FindLegacyDefinition(weaponId);
	const char *model = definition ? definition->GetPlayerModel() : NULL;
	return model ? model : fallback;
}

#endif // WEAPON_FRAMEWORK_ITEMINFO_H

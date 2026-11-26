/*
 * Small, allocation-free JSON reader for weapon definitions.
 * Supports the strict version-1 definition format without exceptions or RTTI.
 */
#pragma once
#ifndef WEAPON_FRAMEWORK_JSON_H
#define WEAPON_FRAMEWORK_JSON_H

#include <stddef.h>
#include "weapon_definition.h"

namespace WeaponFramework
{
	bool ParseWeaponDefinitionJson(const char *json, size_t length, WeaponDefinition &outDefinition);
	bool RegisterWeaponDefinitionJson(const char *json, size_t length);
}

#endif // WEAPON_FRAMEWORK_JSON_H

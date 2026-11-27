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

	// Callbacks keep the shared loader independent of the client/server engine API.
	typedef unsigned char *(*WeaponFileLoadCallback)(const char *path, int *length);
	typedef void (*WeaponFileFreeCallback)(unsigned char *fileData);

	// Loads the root package catalogue, package manifests, then weapon JSON files.
	// All packages and weapons are sorted by stable name before registration.
	bool LoadWeaponPackageCatalog(const char *catalogPath,
		WeaponFileLoadCallback loadFile, WeaponFileFreeCallback freeFile);
}

#endif // WEAPON_FRAMEWORK_JSON_H

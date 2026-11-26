/*
 * Small, allocation-free JSON reader for weapon definitions.
 */
#include "weapon_json.h"
#include "weapon_registry.h"
#include "weapons.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace
{
	enum JsonType
	{
		JSON_OBJECT,
		JSON_ARRAY,
		JSON_STRING,
		JSON_NUMBER,
		JSON_TRUE,
		JSON_FALSE,
		JSON_NULL
	};

	enum
	{
		MAX_JSON_TOKENS = 256,
		MAX_JSON_DEPTH = 16,
		MAX_JSON_BYTES = 16384
	};

	struct JsonToken
	{
		JsonType type;
		size_t start;
		size_t end;
		int next;
		int children;
	};

	struct JsonParser
	{
		const char *data;
		size_t length;
		size_t position;
		JsonToken tokens[MAX_JSON_TOKENS];
		int tokenCount;
		bool failed;
	};

	bool IsWhitespace(char c)
	{
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	}

	void SkipWhitespace(JsonParser &parser)
	{
		while (parser.position < parser.length &&
			IsWhitespace(parser.data[parser.position]))
			++parser.position;
	}

	bool IsHexDigit(char c)
	{
		return (c >= '0' && c <= '9') ||
			(c >= 'a' && c <= 'f') ||
			(c >= 'A' && c <= 'F');
	}

	unsigned int HexValue(char c)
	{
		if (c >= '0' && c <= '9')
			return (unsigned int)(c - '0');
		if (c >= 'a' && c <= 'f')
			return (unsigned int)(c - 'a' + 10);
		return (unsigned int)(c - 'A' + 10);
	}

	bool ReadHex4(const char *data, size_t length, size_t &position, unsigned int &value)
	{
		int i;

		if (position + 4 > length)
			return false;

		value = 0;
		for (i = 0; i < 4; ++i)
		{
			if (!IsHexDigit(data[position]))
				return false;
			value = (value << 4) | HexValue(data[position]);
			++position;
		}
		return true;
	}

	int AddToken(JsonParser &parser, JsonType type, size_t start)
	{
		int index;

		if (parser.tokenCount >= MAX_JSON_TOKENS)
		{
			parser.failed = true;
			return -1;
		}

		index = parser.tokenCount++;
		parser.tokens[index].type = type;
		parser.tokens[index].start = start;
		parser.tokens[index].end = start;
		parser.tokens[index].next = parser.tokenCount;
		parser.tokens[index].children = 0;
		return index;
	}

	bool ParseStringToken(JsonParser &parser)
	{
		size_t start;
		int token;

		if (parser.position >= parser.length || parser.data[parser.position] != '"')
			return false;

		++parser.position;
		start = parser.position;

		while (parser.position < parser.length)
		{
			const unsigned char c = (unsigned char)parser.data[parser.position];

			if (c == '"')
			{
				token = AddToken(parser, JSON_STRING, start);
				if (token < 0)
					return false;
				parser.tokens[token].end = parser.position;
				++parser.position;
				parser.tokens[token].next = parser.tokenCount;
				return true;
			}

			if (c < 0x20)
				return false;

			if (c == '\\')
			{
				char escaped;
				++parser.position;
				if (parser.position >= parser.length)
					return false;
				escaped = parser.data[parser.position];

				if (escaped == '"' || escaped == '\\' || escaped == '/' ||
					escaped == 'b' || escaped == 'f' || escaped == 'n' ||
					escaped == 'r' || escaped == 't')
				{
					++parser.position;
					continue;
				}

				if (escaped == 'u')
				{
					unsigned int ignored;
					++parser.position;
					if (!ReadHex4(parser.data, parser.length, parser.position, ignored))
						return false;
					continue;
				}

				return false;
			}

			++parser.position;
		}

		return false;
	}

	bool ParseNumberToken(JsonParser &parser)
	{
		const size_t start = parser.position;
		int token;

		if (parser.data[parser.position] == '-')
			++parser.position;

		if (parser.position >= parser.length)
			return false;

		if (parser.data[parser.position] == '0')
		{
			++parser.position;
		}
		else
		{
			if (parser.data[parser.position] < '1' || parser.data[parser.position] > '9')
				return false;
			while (parser.position < parser.length &&
				parser.data[parser.position] >= '0' &&
				parser.data[parser.position] <= '9')
				++parser.position;
		}

		if (parser.position < parser.length && parser.data[parser.position] == '.')
		{
			++parser.position;
			if (parser.position >= parser.length ||
				parser.data[parser.position] < '0' || parser.data[parser.position] > '9')
				return false;
			while (parser.position < parser.length &&
				parser.data[parser.position] >= '0' &&
				parser.data[parser.position] <= '9')
				++parser.position;
		}

		if (parser.position < parser.length &&
			(parser.data[parser.position] == 'e' || parser.data[parser.position] == 'E'))
		{
			++parser.position;
			if (parser.position < parser.length &&
				(parser.data[parser.position] == '+' || parser.data[parser.position] == '-'))
				++parser.position;
			if (parser.position >= parser.length ||
				parser.data[parser.position] < '0' || parser.data[parser.position] > '9')
				return false;
			while (parser.position < parser.length &&
				parser.data[parser.position] >= '0' &&
				parser.data[parser.position] <= '9')
				++parser.position;
		}

		token = AddToken(parser, JSON_NUMBER, start);
		if (token < 0)
			return false;
		parser.tokens[token].end = parser.position;
		parser.tokens[token].next = parser.tokenCount;
		return true;
	}

	bool ParseValue(JsonParser &parser, int depth);

	bool ParseArray(JsonParser &parser, int depth)
	{
		int token;

		if (depth > MAX_JSON_DEPTH)
			return false;

		token = AddToken(parser, JSON_ARRAY, parser.position);
		if (token < 0)
			return false;

		++parser.position;
		SkipWhitespace(parser);

		if (parser.position < parser.length && parser.data[parser.position] == ']')
		{
			++parser.position;
			parser.tokens[token].end = parser.position;
			parser.tokens[token].next = parser.tokenCount;
			return true;
		}

		for (;;)
		{
			if (!ParseValue(parser, depth + 1))
				return false;
			++parser.tokens[token].children;
			SkipWhitespace(parser);

			if (parser.position >= parser.length)
				return false;

			if (parser.data[parser.position] == ']')
			{
				++parser.position;
				parser.tokens[token].end = parser.position;
				parser.tokens[token].next = parser.tokenCount;
				return true;
			}

			if (parser.data[parser.position] != ',')
				return false;

			++parser.position;
			SkipWhitespace(parser);
		}
	}

	bool ParseObject(JsonParser &parser, int depth)
	{
		int token;

		if (depth > MAX_JSON_DEPTH)
			return false;

		token = AddToken(parser, JSON_OBJECT, parser.position);
		if (token < 0)
			return false;

		++parser.position;
		SkipWhitespace(parser);

		if (parser.position < parser.length && parser.data[parser.position] == '}')
		{
			++parser.position;
			parser.tokens[token].end = parser.position;
			parser.tokens[token].next = parser.tokenCount;
			return true;
		}

		for (;;)
		{
			if (!ParseStringToken(parser))
				return false;

			SkipWhitespace(parser);
			if (parser.position >= parser.length || parser.data[parser.position] != ':')
				return false;

			++parser.position;
			SkipWhitespace(parser);

			if (!ParseValue(parser, depth + 1))
				return false;
			++parser.tokens[token].children;
			SkipWhitespace(parser);

			if (parser.position >= parser.length)
				return false;

			if (parser.data[parser.position] == '}')
			{
				++parser.position;
				parser.tokens[token].end = parser.position;
				parser.tokens[token].next = parser.tokenCount;
				return true;
			}

			if (parser.data[parser.position] != ',')
				return false;

			++parser.position;
			SkipWhitespace(parser);
		}
	}

	bool ParseValue(JsonParser &parser, int depth)
	{
		if (depth > MAX_JSON_DEPTH || parser.position >= parser.length)
			return false;

		switch (parser.data[parser.position])
		{
		case '{':
			return ParseObject(parser, depth);
		case '[':
			return ParseArray(parser, depth);
		case '"':
			return ParseStringToken(parser);
		case 't':
			if (parser.position + 4 <= parser.length &&
				memcmp(parser.data + parser.position, "true", 4) == 0)
			{
				const int token = AddToken(parser, JSON_TRUE, parser.position);
				if (token < 0)
					return false;
				parser.position += 4;
				parser.tokens[token].end = parser.position;
				parser.tokens[token].next = parser.tokenCount;
				return true;
			}
			return false;
		case 'f':
			if (parser.position + 5 <= parser.length &&
				memcmp(parser.data + parser.position, "false", 5) == 0)
			{
				const int token = AddToken(parser, JSON_FALSE, parser.position);
				if (token < 0)
					return false;
				parser.position += 5;
				parser.tokens[token].end = parser.position;
				parser.tokens[token].next = parser.tokenCount;
				return true;
			}
			return false;
		case 'n':
			if (parser.position + 4 <= parser.length &&
				memcmp(parser.data + parser.position, "null", 4) == 0)
			{
				const int token = AddToken(parser, JSON_NULL, parser.position);
				if (token < 0)
					return false;
				parser.position += 4;
				parser.tokens[token].end = parser.position;
				parser.tokens[token].next = parser.tokenCount;
				return true;
			}
			return false;
		default:
			if (parser.data[parser.position] == '-' ||
				(parser.data[parser.position] >= '0' && parser.data[parser.position] <= '9'))
				return ParseNumberToken(parser);
			return false;
		}
	}

	bool ParseDocument(JsonParser &parser, const char *json, size_t length)
	{
		if (!json || length == 0 || length > MAX_JSON_BYTES)
			return false;

		parser.data = json;
		parser.length = length;
		parser.position = 0;
		parser.tokenCount = 0;
		parser.failed = false;

		SkipWhitespace(parser);
		if (!ParseValue(parser, 0))
			return false;
		SkipWhitespace(parser);

		return parser.position == parser.length && parser.tokenCount > 0 &&
			parser.tokens[0].type == JSON_OBJECT && !parser.failed;
	}

	bool AppendUtf8(char *output, size_t capacity, size_t &written, unsigned int codepoint)
	{
		unsigned char bytes[4];
		size_t count;
		size_t i;

		if (codepoint < 0x20)
			return false;
		if (codepoint <= 0x7f)
		{
			bytes[0] = (unsigned char)codepoint;
			count = 1;
		}
		else if (codepoint <= 0x7ff)
		{
			bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
			bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
			count = 2;
		}
		else if (codepoint <= 0xffff)
		{
			bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
			bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
			bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
			count = 3;
		}
		else if (codepoint <= 0x10ffff)
		{
			bytes[0] = (unsigned char)(0xf0 | (codepoint >> 18));
			bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
			bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
			bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
			count = 4;
		}
		else
		{
			return false;
		}

		if (written + count >= capacity)
			return false;

		for (i = 0; i < count; ++i)
			output[written++] = (char)bytes[i];
		return true;
	}

	bool DecodeString(const JsonParser &parser, int tokenIndex, char *output, size_t capacity)
	{
		size_t read;
		size_t written = 0;
		const JsonToken &token = parser.tokens[tokenIndex];

		if (token.type != JSON_STRING || !output || capacity == 0)
			return false;

		for (read = token.start; read < token.end; ++read)
		{
			unsigned int codepoint;
			char c = parser.data[read];

			if (c != '\\')
			{
				if (written + 1 >= capacity)
					return false;
				output[written++] = c;
				continue;
			}

			++read;
			if (read >= token.end)
				return false;

			switch (parser.data[read])
			{
			case '"': codepoint = '"'; break;
			case '\\': codepoint = '\\'; break;
			case '/': codepoint = '/'; break;
			case 'b': codepoint = '\b'; break;
			case 'f': codepoint = '\f'; break;
			case 'n': codepoint = '\n'; break;
			case 'r': codepoint = '\r'; break;
			case 't': codepoint = '\t'; break;
			case 'u':
			{
				size_t unicodePosition = read + 1;
				if (!ReadHex4(parser.data, token.end, unicodePosition, codepoint))
					return false;

				if (codepoint >= 0xd800 && codepoint <= 0xdbff)
				{
					unsigned int low;
					if (unicodePosition + 6 > token.end ||
						parser.data[unicodePosition] != '\\' ||
						parser.data[unicodePosition + 1] != 'u')
						return false;
					unicodePosition += 2;
					if (!ReadHex4(parser.data, token.end, unicodePosition, low) ||
						low < 0xdc00 || low > 0xdfff)
						return false;
					codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
				}
				else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
				{
					return false;
				}

				if (!AppendUtf8(output, capacity, written, codepoint))
					return false;
				read = unicodePosition - 1;
				continue;
			}
			default:
				return false;
			}

			if (!AppendUtf8(output, capacity, written, codepoint))
				return false;
		}

		output[written] = '\0';
		return true;
	}

	bool StringTokenEquals(const JsonParser &parser, int tokenIndex, const char *value)
	{
		char decoded[128];
		return DecodeString(parser, tokenIndex, decoded, sizeof(decoded)) &&
			strcmp(decoded, value) == 0;
	}

	bool ValidateKeys(const JsonParser &parser, int objectIndex,
		const char *const *allowedKeys, int allowedCount)
	{
		bool seen[12] = { false };
		int i;
		int member;

		if (objectIndex < 0 || parser.tokens[objectIndex].type != JSON_OBJECT ||
			allowedCount > 12)
			return false;

		i = objectIndex + 1;
		for (member = 0; member < parser.tokens[objectIndex].children; ++member)
		{
			int keyIndex = i;
			int valueIndex = keyIndex + 1;
			int key;
			int match = -1;

			if (valueIndex >= parser.tokenCount ||
				parser.tokens[keyIndex].type != JSON_STRING)
				return false;

			for (key = 0; key < allowedCount; ++key)
			{
				if (StringTokenEquals(parser, keyIndex, allowedKeys[key]))
				{
					match = key;
					break;
				}
			}

			if (match < 0 || seen[match])
				return false;

			seen[match] = true;
			i = parser.tokens[valueIndex].next;
		}

		return true;
	}

	int FindMember(const JsonParser &parser, int objectIndex, const char *key)
	{
		int i;
		int member;

		if (objectIndex < 0 || parser.tokens[objectIndex].type != JSON_OBJECT)
			return -1;

		i = objectIndex + 1;
		for (member = 0; member < parser.tokens[objectIndex].children; ++member)
		{
			const int keyIndex = i;
			const int valueIndex = keyIndex + 1;
			if (StringTokenEquals(parser, keyIndex, key))
				return valueIndex;
			i = parser.tokens[valueIndex].next;
		}
		return -1;
	}

	bool ReadInteger(const JsonParser &parser, int tokenIndex, int minimum, int maximum, int &value)
	{
		char number[32];
		size_t length;
		char *end = NULL;
		long parsed;

		if (tokenIndex < 0 || parser.tokens[tokenIndex].type != JSON_NUMBER)
			return false;

		length = parser.tokens[tokenIndex].end - parser.tokens[tokenIndex].start;
		if (length == 0 || length >= sizeof(number))
			return false;

		memcpy(number, parser.data + parser.tokens[tokenIndex].start, length);
		number[length] = '\0';

		if (strchr(number, '.') || strchr(number, 'e') || strchr(number, 'E'))
			return false;

		parsed = strtol(number, &end, 10);
		if (!end || *end != '\0' || parsed < minimum || parsed > maximum)
			return false;

		value = (int)parsed;
		return true;
	}

	bool ReadString(const JsonParser &parser, int tokenIndex, char *output, size_t capacity)
	{
		return tokenIndex >= 0 && DecodeString(parser, tokenIndex, output, capacity);
	}

	bool GetRequiredMember(const JsonParser &parser, int objectIndex, const char *key, int &valueIndex)
	{
		valueIndex = FindMember(parser, objectIndex, key);
		return valueIndex >= 0;
	}

	bool IsValidStableName(const char *name)
	{
		const char *colon;
		const char *p;

		if (!name || strlen(name) < 3 || strlen(name) > 63)
			return false;

		colon = strchr(name, ':');
		if (!colon || colon == name || colon[1] == '\0' || strchr(colon + 1, ':'))
			return false;

		for (p = name; *p; ++p)
		{
			const char c = *p;
			const bool valid =
				(c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') ||
				c == '_' || c == '.' || c == '-' ||
				(c == ':' && p == colon) ||
				(p > colon && c == '/');
			if (!valid)
				return false;
		}
		return true;
	}

	bool IsValidEntityClassname(const char *name)
	{
		const char *p;
		if (!name || strncmp(name, "weapon_", 7) != 0 ||
			strlen(name) < 8 || strlen(name) > 63)
			return false;

		for (p = name + 7; *p; ++p)
		{
			const char c = *p;
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '_'))
				return false;
		}
		return true;
	}

	bool IsValidAmmoType(const char *type)
	{
		const char *p;
		if (!type || type[0] == '\0' || strlen(type) > 31)
			return false;
		for (p = type; *p; ++p)
		{
			const char c = *p;
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '_'))
				return false;
		}
		return true;
	}

	bool IsValidModelPath(const char *path)
	{
		const size_t length = path ? strlen(path) : 0;
		const char *p;
		if (length < 11 || length >= ModelPathCapacity ||
			strncmp(path, "models/", 7) != 0 ||
			strcmp(path + length - 4, ".mdl") != 0 ||
			strstr(path, "..") || strchr(path, '\\'))
			return false;
		for (p = path + 7; *p; ++p)
			if ((unsigned char)*p < 0x20)
				return false;
		return true;
	}

	bool ParseAmmo(const JsonParser &parser, int ammoToken,
		char *ammoType, size_t ammoTypeCapacity, int &maxCarry)
	{
		static const char *const keys[] = { "type", "max_carry" };
		int typeToken;
		int maxToken;

		ammoType[0] = '\0';
		maxCarry = -1;

		if (ammoToken < 0)
			return false;

		if (parser.tokens[ammoToken].type == JSON_NULL)
			return true;

		if (!ValidateKeys(parser, ammoToken, keys, 2) ||
			parser.tokens[ammoToken].children != 2)
			return false;

		if (!GetRequiredMember(parser, ammoToken, "type", typeToken) ||
			!ReadString(parser, typeToken, ammoType, ammoTypeCapacity) ||
			!IsValidAmmoType(ammoType)
			return false;

		if (!GetRequiredMember(parser, ammoToken, "max_carry", maxToken) ||
			!ReadInteger(parser, maxToken, 0, 32767, maxCarry))
			return false;

		return true;
	}

	bool ParseFlags(const JsonParser &parser, int flagsToken, int &flags)
	{
		int i;
		int item;
		flags = 0;

		if (flagsToken < 0 || parser.tokens[flagsToken].type != JSON_ARRAY)
			return false;

		item = flagsToken + 1;
		for (i = 0; i < parser.tokens[flagsToken].children; ++i)
		{
			char flag[64];
			int bit = 0;

			if (!ReadString(parser, item, flag, sizeof(flag)))
				return false;

			if (strcmp(flag, "select_on_empty") == 0)
				bit = ITEM_FLAG_SELECTONEMPTY;
			else if (strcmp(flag, "no_auto_reload") == 0)
				bit = ITEM_FLAG_NOAUTORELOAD;
			else if (strcmp(flag, "no_auto_switch_empty") == 0)
				bit = ITEM_FLAG_NOAUTOSWITCHEMPTY;
			else if (strcmp(flag, "limit_in_world") == 0)
				bit = ITEM_FLAG_LIMITINWORLD;
			else if (strcmp(flag, "exhaustible") == 0)
				bit = ITEM_FLAG_EXHAUSTIBLE;
			else if (strcmp(flag, "no_auto_switch_to") == 0)
				bit = ITEM_FLAG_NOAUTOSWITCHTO;
			else
				return false;

			if (flags & bit)
				return false;
			flags |= bit;
			item = parser.tokens[item].next;
		}

		return true;
	}

	bool ParseWeapon(const JsonParser &parser, WeaponDefinition &outDefinition)
	{
		static const char *const rootKeys[] = {
			"format_version", "identity", "inventory", "ammo", "models"
		};
		static const char *const identityKeys[] = { "name", "entity_classname", "legacy_id" };
		static const char *const inventoryKeys[] = { "slot", "position", "weight", "flags" };
		static const char *const ammoKeys[] = { "clip_size", "default_ammo", "primary", "secondary" };
		static const char *const modelKeys[] = { "view", "world", "player" };
		WeaponDefinitionData data = {};
		char stableName[WeaponNameCapacity];
		char entityClassname[EntityClassnameCapacity];
		char primaryAmmo[AmmoNameCapacity];
		char secondaryAmmo[AmmoNameCapacity];
		char viewModel[ModelPathCapacity];
		char worldModel[ModelPathCapacity];
		char playerModel[ModelPathCapacity];
		int root = 0;
		int identity;
		int inventory;
		int ammo;
		int models;
		int token;
		int legacyId = 0;
		int formatVersion = 0;

		if (!ValidateKeys(parser, root, rootKeys, 5) ||
			parser.tokens[root].children != 5)
			return false;

		if (!GetRequiredMember(parser, root, "format_version", token) ||
			!ReadInteger(parser, token, 1, 1, formatVersion) || formatVersion != 1)
			return false;

		if (!GetRequiredMember(parser, root, "identity", identity) ||
			!ValidateKeys(parser, identity, identityKeys, 3) ||
			parser.tokens[identity].children < 2 ||
			parser.tokens[identity].children > 3)
			return false;

		if (!GetRequiredMember(parser, identity, "name", token) ||
			!ReadString(parser, token, stableName, sizeof(stableName)) ||
			!IsValidStableName(stableName))
			return false;

		if (!GetRequiredMember(parser, identity, "entity_classname", token) ||
			!ReadString(parser, token, entityClassname, sizeof(entityClassname)) ||
			!IsValidEntityClassname(entityClassname))
			return false;

		token = FindMember(parser, identity, "legacy_id");
		if (token >= 0 && !ReadInteger(parser, token, FirstLegacyWeaponId, LastLegacyWeaponId, legacyId))
			return false;

		if (!GetRequiredMember(parser, root, "inventory", inventory) ||
			!ValidateKeys(parser, inventory, inventoryKeys, 4) ||
			parser.tokens[inventory].children != 4)
			return false;

		if (!GetRequiredMember(parser, inventory, "slot", token) ||
			!ReadInteger(parser, token, 0, MAX_ITEM_TYPES - 1, data.slot))
			return false;
		if (!GetRequiredMember(parser, inventory, "position", token) ||
			!ReadInteger(parser, token, 0, 255, data.position))
			return false;
		if (!GetRequiredMember(parser, inventory, "weight", token) ||
			!ReadInteger(parser, token, -32768, 32767, data.weight))
			return false;
		if (!GetRequiredMember(parser, inventory, "flags", token) ||
			!ParseFlags(parser, token, data.flags))
			return false;

		if (!GetRequiredMember(parser, root, "ammo", ammo) ||
			!ValidateKeys(parser, ammo, ammoKeys, 4) ||
			parser.tokens[ammo].children != 4)
			return false;

		if (!GetRequiredMember(parser, ammo, "clip_size", token) ||
			!ReadInteger(parser, token, -1, 32767, data.clipSize))
			return false;
		if (!GetRequiredMember(parser, ammo, "default_ammo", token) ||
			!ReadInteger(parser, token, 0, 32767, data.defaultAmmo))
			return false;
		if (!GetRequiredMember(parser, ammo, "primary", token) ||
			!ParseAmmo(parser, token, primaryAmmo, sizeof(primaryAmmo), data.primaryAmmoMax))
			return false;
		if (!GetRequiredMember(parser, ammo, "secondary", token) ||
			!ParseAmmo(parser, token, secondaryAmmo, sizeof(secondaryAmmo), data.secondaryAmmoMax))
			return false;

		if (!GetRequiredMember(parser, root, "models", models) ||
			!ValidateKeys(parser, models, modelKeys, 3) ||
			parser.tokens[models].children != 3)
			return false;

		if (!GetRequiredMember(parser, models, "view", token) ||
			!ReadString(parser, token, viewModel, sizeof(viewModel)) ||
			!IsValidModelPath(viewModel))
			return false;
		if (!GetRequiredMember(parser, models, "world", token) ||
			!ReadString(parser, token, worldModel, sizeof(worldModel)) ||
			!IsValidModelPath(worldModel))
			return false;

		if (!GetRequiredMember(parser, models, "player", token))
			return false;
		if (parser.tokens[token].type == JSON_NULL)
		{
			playerModel[0] = '\0';
		}
		else if (!ReadString(parser, token, playerModel, sizeof(playerModel)) ||
			!IsValidModelPath(playerModel))
		{
			return false;
		}

		data.id = legacyId;
		data.name = stableName;
		data.entityClassname = entityClassname;
		data.primaryAmmoType = primaryAmmo[0] ? primaryAmmo : NULL;
		data.secondaryAmmoType = secondaryAmmo[0] ? secondaryAmmo : NULL;
		data.viewModel = viewModel;
		data.worldModel = worldModel;
		data.playerModel = playerModel[0] ? playerModel : NULL;

		outDefinition = WeaponDefinition(data);
		return outDefinition.IsValid();
	}
}

namespace WeaponFramework
{
	bool ParseWeaponDefinitionJson(const char *json, size_t length, WeaponDefinition &outDefinition)
	{
		JsonParser parser;
		memset(&parser, 0, sizeof(parser));

		if (!ParseDocument(parser, json, length))
			return false;

		return ParseWeapon(parser, outDefinition);
	}

	bool RegisterWeaponDefinitionJson(const char *json, size_t length)
	{
		WeaponDefinition definition;

		if (!ParseWeaponDefinitionJson(json, length, definition))
			return false;

		if (definition.GetId() >= FirstLegacyWeaponId &&
			definition.GetId() <= LastLegacyWeaponId)
			return GetRegistry().RegisterLegacy(definition);

		if (definition.GetId() == 0)
			return GetRegistry().RegisterAddon(definition) != 0;

		return false;
	}
}

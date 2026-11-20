// GoldSrc adaptation of Valve's Source C_GameInstructor and CBaseLesson flow.
// Source-only IGameEvent/KeyValues/VGUI systems are mapped to GoldSrc usermessages,
// a small KeyValues-compatible reader, entity indices and the existing HUD locator card.

#include "hud.h"
#include "cl_util.h"
#include "const.h"
#include "com_model.h"
#include "cdll_int.h"
#include "game_instructor.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <algorithm>

#ifndef FCVAR_ARCHIVE
#define FCVAR_ARCHIVE 1
#endif

#define GAMEINSTRUCTOR_SCRIPT_FILE "scripts/instructor_lessons.txt"
#define GAMEINSTRUCTOR_MOD_SCRIPT_FILE "scripts/mod_lessons.txt"
#define GAMEINSTRUCTOR_MAX_DEPTH 32
#define LESSON_MIN_TIME_ON_SCREEN_TO_MARK_DISPLAYED 1.5f
#define LESSON_MIN_TIME_BEFORE_LOCK_ALLOWED 0.1f

namespace
{
	bool StringEqualNoCase(const char *a, const char *b)
	{
		if (!a || !b)
			return a == b;
		while (*a && *b)
		{
			if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
				return false;
			++a;
			++b;
		}
		return *a == *b;
	}

	std::string NormalizeVariableName(const std::string &name)
	{
		std::string normalized(name);
		for (size_t i = 0; i < normalized.size(); ++i)
			normalized[i] = (char)tolower((unsigned char)normalized[i]);
		return normalized;
	}

	bool InstructorVerboseEnabled()
	{
		return CVAR_GET_FLOAT("gameinstructor_verbose") != 0.0f;
	}

	struct KVNode
	{
		std::string name;
		std::string value;
		std::vector<KVNode> children;
	};

	class KVReader
	{
	public:
		explicit KVReader(const char *text) : m_pText(text ? text : ""), m_iDepth(0) {}

		bool Parse(KVNode &root)
		{
			std::string rootName;
			if (!ReadToken(rootName) || rootName.empty())
				return false;
			std::string brace;
			if (!ReadToken(brace) || brace != "{")
				return false;
			root.name = rootName;
			return ParseChildren(root);
		}

	private:
		void SkipSpace()
		{
			for (;;)
			{
				while (*m_pText && (unsigned char)*m_pText <= ' ')
					++m_pText;

				if (m_pText[0] == '/' && m_pText[1] == '/')
				{
					m_pText += 2;
					while (*m_pText && *m_pText != '\n')
						++m_pText;
					continue;
				}

				if (m_pText[0] == '/' && m_pText[1] == '*')
				{
					m_pText += 2;
					while (*m_pText && !(m_pText[0] == '*' && m_pText[1] == '/'))
						++m_pText;
					if (*m_pText)
						m_pText += 2;
					continue;
				}

				break;
			}
		}

		bool ReadToken(std::string &token)
		{
			SkipSpace();
			token.clear();
			if (!*m_pText)
				return false;

			if (*m_pText == '{' || *m_pText == '}')
			{
				token.push_back(*m_pText++);
				return true;
			}

			if (*m_pText == '"')
			{
				++m_pText;
				while (*m_pText && *m_pText != '"')
				{
					if (*m_pText == '\\' && m_pText[1])
					{
						++m_pText;
						switch (*m_pText)
						{
						case 'n': token.push_back('\n'); break;
						case 't': token.push_back('\t'); break;
						default: token.push_back(*m_pText); break;
						}
						++m_pText;
					}
					else
					{
						token.push_back(*m_pText++);
					}
				}
				if (*m_pText == '"')
					++m_pText;
				return true;
			}

			while (*m_pText && (unsigned char)*m_pText > ' ' && *m_pText != '{' && *m_pText != '}')
				token.push_back(*m_pText++);
			return !token.empty();
		}

		bool ParseChildren(KVNode &parent)
		{
			if (++m_iDepth > GAMEINSTRUCTOR_MAX_DEPTH)
				return false;

			for (;;)
			{
				std::string key;
				if (!ReadToken(key))
				{
					--m_iDepth;
					return false;
				}
				if (key == "}")
				{
					--m_iDepth;
					return true;
				}

				std::string value;
				if (!ReadToken(value))
				{
					--m_iDepth;
					return false;
				}

				KVNode child;
				child.name = key;
				if (value == "{")
				{
					if (!ParseChildren(child))
					{
						--m_iDepth;
						return false;
					}
				}
				else if (value == "}")
				{
					--m_iDepth;
					return false;
				}
				else
				{
					child.value = value;
				}
				parent.children.push_back(child);
			}
		}

		const char *m_pText;
		int m_iDepth;
	};

	const KVNode *FindChild(const KVNode &node, const char *name)
	{
		for (size_t i = 0; i < node.children.size(); ++i)
		{
			if (StringEqualNoCase(node.children[i].name.c_str(), name))
				return &node.children[i];
		}
		return NULL;
	}

	std::string ChildString(const KVNode &node, const char *name, const char *fallback = "")
	{
		const KVNode *child = FindChild(node, name);
		return child ? child->value : fallback;
	}

	int ChildInt(const KVNode &node, const char *name, int fallback = 0)
	{
		const KVNode *child = FindChild(node, name);
		return child ? (int)strtol(child->value.c_str(), NULL, 0) : fallback;
	}

	float ChildFloat(const KVNode &node, const char *name, float fallback = 0.0f)
	{
		const KVNode *child = FindChild(node, name);
		return child ? (float)atof(child->value.c_str()) : fallback;
	}

	bool ChildBool(const KVNode &node, const char *name, bool fallback = false)
	{
		const KVNode *child = FindChild(node, name);
		if (!child)
			return fallback;
		if (StringEqualNoCase(child->value.c_str(), "true") || StringEqualNoCase(child->value.c_str(), "yes"))
			return true;
		if (StringEqualNoCase(child->value.c_str(), "false") || StringEqualNoCase(child->value.c_str(), "no"))
			return false;
		return atoi(child->value.c_str()) != 0;
	}

	bool LoadKVFile(const char *fileName, KVNode &root)
	{
		if (!gEngfuncs.COM_LoadFile || !gEngfuncs.COM_FreeFile)
			return false;

		int length = 0;
		byte *buffer = gEngfuncs.COM_LoadFile(fileName, 5, &length);
		if (!buffer || length <= 0)
		{
			if (buffer)
				gEngfuncs.COM_FreeFile(buffer);
			if (gEngfuncs.Con_Printf)
				gEngfuncs.Con_Printf("[GameInstructor] Could not load script '%s' (length %d).\n", fileName, length);
			return false;
		}

		std::string text((const char *)buffer, (size_t)length);
		gEngfuncs.COM_FreeFile(buffer);
		KVReader reader(text.c_str());
		const bool parsed = reader.Parse(root);
		if (gEngfuncs.Con_Printf)
			gEngfuncs.Con_Printf("[GameInstructor] Script '%s': %s, %u top-level entries.\n",
				fileName, parsed ? "parsed" : "parse FAILED",
				(unsigned int)root.children.size());
		return parsed;
	}

	void ParseLessonEvents(const KVNode &lesson, const char *sectionName,
		std::vector<InstructorLessonEvent> &events)
	{
		const KVNode *section = FindChild(lesson, sectionName);
		if (!section)
			return;

		for (size_t i = 0; i < section->children.size(); ++i)
		{
			const KVNode &eventNode = section->children[i];
			InstructorLessonEvent event;
			event.name = eventNode.name;

			for (size_t j = 0; j < eventNode.children.size(); ++j)
			{
				InstructorLessonElement element;
				element.expression = eventNode.children[j].name;
				element.parameter = eventNode.children[j].value;
				event.elements.push_back(element);
			}
			events.push_back(event);
		}
	}

	void ParseLessonDefinition(CScriptedIconLesson *lesson, const KVNode &node)
	{
		lesson->m_InstanceType = (LessonInstanceType)ChildInt(node, "instance_type", LESSON_INSTANCE_MULTIPLE);
		if (lesson->m_InstanceType < LESSON_INSTANCE_MULTIPLE || lesson->m_InstanceType >= LESSON_INSTANCE_TYPE_TOTAL)
			lesson->m_InstanceType = LESSON_INSTANCE_MULTIPLE;
		lesson->m_ReplaceKey = ChildString(node, "replace_key");
		lesson->m_iFixedInstancesMax = ChildInt(node, "fixed_instances_max", 1);
		if (lesson->m_iFixedInstancesMax < 1)
			lesson->m_iFixedInstancesMax = 1;
		lesson->m_bReplaceOnlyWhenStopped = ChildBool(node, "replace_only_when_stopped");
		lesson->m_iPriority = ChildInt(node, "priority", 100);
		lesson->m_iDisplayLimit = ChildInt(node, "display_limit");
		lesson->m_iSuccessLimit = ChildInt(node, "success_limit");
		lesson->m_flLockDuration = ChildFloat(node, "lock_duration");
		lesson->m_flTimeout = ChildFloat(node, "timeout");
		lesson->m_flUpdateInterval = ChildFloat(node, "update_interval", 0.5f);
		if (lesson->m_flUpdateInterval < 0.05f)
			lesson->m_flUpdateInterval = 0.05f;
		lesson->m_bCanOpenWhenDead = ChildBool(node, "can_open_when_dead");
		lesson->m_bBumpWithTimeoutWhenLearned = ChildBool(node, "bump_with_timeout_when_learned");
		lesson->m_bCanTimeoutWhileInactive = ChildBool(node, "can_timeout_while_inactive");
		lesson->m_Group = ChildString(node, "group");

		lesson->m_Caption = ChildString(node, "caption");
		lesson->m_CaptionParam = ChildString(node, "caption_param");
		lesson->m_Binding = ChildString(node, "binding");
		lesson->m_GamepadBinding = ChildString(node, "gamepad_binding");
		lesson->m_OnscreenIcon = ChildString(node, "onscreen_icon");
		lesson->m_OffscreenIcon = ChildString(node, "offscreen_icon");
		lesson->m_CaptionColor = ChildString(node, "caption_color", "255,255,255");
		lesson->m_VguiTargetName = ChildString(node, "vgui_target_name");
		lesson->m_VguiTargetLookup = ChildString(node, "vgui_target_lookup");
		lesson->m_iVguiTargetEdge = ChildInt(node, "vgui_target_edge");
		lesson->m_iFlags = ChildInt(node, "flags");
		lesson->m_flRange = ChildFloat(node, "range");
		lesson->m_flIconOffset = ChildFloat(node, "icon_offset");
		lesson->m_flRelativeIconOffset = ChildFloat(node, "icon_relative_offset");
		lesson->m_bNoIconTarget = ChildBool(node, "no_icon_target");
		lesson->m_bAllowNodrawTarget = ChildBool(node, "allow_nodraw_target");
		lesson->m_bVisible = ChildBool(node, "visible", true);
		lesson->m_bShowWhenOccluded = ChildBool(node, "show_when_occluded", true);
		lesson->m_bNoOffscreen = ChildBool(node, "no_offscreen");
		lesson->m_bForceCaption = ChildBool(node, "force_caption");
		lesson->m_bFixedPosition = ChildBool(node, "fixed_position");
		lesson->m_flFixedX = ChildFloat(node, "fixed_position_x");
		lesson->m_flFixedY = ChildFloat(node, "fixed_position_y");
		lesson->m_StartSound = ChildString(node, "start_sound");

		lesson->m_Prerequisites.clear();
		for (size_t i = 0; i < node.children.size(); ++i)
		{
			if (StringEqualNoCase(node.children[i].name.c_str(), "prereq"))
				lesson->m_Prerequisites.push_back(node.children[i].value);
		}

		lesson->m_OpenEvents.clear();
		lesson->m_CloseEvents.clear();
		lesson->m_SuccessEvents.clear();
		lesson->m_OnOpenEvents.clear();
		lesson->m_UpdateEvents.clear();
		ParseLessonEvents(node, "open", lesson->m_OpenEvents);
		ParseLessonEvents(node, "close", lesson->m_CloseEvents);
		ParseLessonEvents(node, "success", lesson->m_SuccessEvents);
		ParseLessonEvents(node, "onopen", lesson->m_OnOpenEvents);
		ParseLessonEvents(node, "update", lesson->m_UpdateEvents);
	}

	struct ParamValue
	{
		enum Type { VALUE_NONE, VALUE_INTEGER, VALUE_FLOAT, VALUE_STRING, VALUE_BOOLEAN, VALUE_ENTITY } type;
		int integer;
		float number;
		std::string string;
		ParamValue() : type(VALUE_NONE), integer(0), number(0.0f) {}
	};

	bool ParseExpression(const std::string &expression, std::string &variable,
		std::string &action, bool &optional, bool &negate)
	{
		size_t split = expression.find(' ');
		if (split == std::string::npos)
			return false;
		variable = expression.substr(0, split);
		action = expression.substr(split + 1);
		while (!action.empty() && action[0] == ' ')
			action.erase(0, 1);
		optional = false;
		negate = false;
		if (!action.empty() && action[0] == '?')
		{
			optional = true;
			action.erase(0, 1);
		}
		if (!action.empty() && action[0] == '!')
		{
			negate = true;
			action.erase(0, 1);
		}
		return !variable.empty() && !action.empty();
	}

	bool IsNumericString(const char *text)
	{
		if (!text || !*text)
			return false;
		if (*text == '-' || *text == '+' || *text == '.')
			return true;
		return isdigit((unsigned char)*text) != 0;
	}

	ParamValue GetVariable(const CScriptedIconLesson &lesson, const std::string &name)
	{
		const std::string variableName = NormalizeVariableName(name);
		ParamValue value;
		if (variableName == "local_player" || variableName == "void") { value.type = ParamValue::VALUE_ENTITY; value.integer = lesson.m_iLocalPlayer; }
		else if (variableName == "entity1") { value.type = ParamValue::VALUE_ENTITY; value.integer = lesson.m_iEntity1; }
		else if (variableName == "entity2") { value.type = ParamValue::VALUE_ENTITY; value.integer = lesson.m_iEntity2; }
		else if (variableName == "icon_target") { value.type = ParamValue::VALUE_ENTITY; value.integer = lesson.m_iIconTarget; }
		else if (variableName == "integer1") { value.type = ParamValue::VALUE_INTEGER; value.integer = lesson.m_iInteger1; }
		else if (variableName == "integer2") { value.type = ParamValue::VALUE_INTEGER; value.integer = lesson.m_iInteger2; }
		else if (variableName == "output") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flOutput; }
		else if (variableName == "float1") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flFloat1; }
		else if (variableName == "float2") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flFloat2; }
		else if (variableName == "string1") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_String1; }
		else if (variableName == "string2") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_String2; }
		else if (variableName == "caption") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_Caption; }
		else if (variableName == "caption_param") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_CaptionParam; }
		else if (variableName == "binding") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_Binding; }
		else if (variableName == "gamepad_binding") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_GamepadBinding; }
		else if (variableName == "onscreen_icon") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_OnscreenIcon; }
		else if (variableName == "offscreen_icon") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_OffscreenIcon; }
		else if (variableName == "replace_key") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_ReplaceKey; }
		else if (variableName == "group") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_Group; }
		else if (variableName == "range") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flRange; }
		else if (variableName == "timeout") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flTimeout; }
		else if (variableName == "priority") { value.type = ParamValue::VALUE_INTEGER; value.integer = lesson.m_iPriority; }
		else if (variableName == "flags") { value.type = ParamValue::VALUE_INTEGER; value.integer = lesson.m_iFlags; }
		else if (variableName == "visible") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bVisible ? 1 : 0; }
		else if (variableName == "no_offscreen") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bNoOffscreen ? 1 : 0; }
		else if (variableName == "force_caption") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bForceCaption ? 1 : 0; }
		else if (variableName == "no_icon_target") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bNoIconTarget ? 1 : 0; }
		else if (variableName == "allow_nodraw_target") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bAllowNodrawTarget ? 1 : 0; }
		else if (variableName == "show_when_occluded") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bShowWhenOccluded ? 1 : 0; }
		else if (variableName == "fixed_position") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bFixedPosition ? 1 : 0; }
		else if (variableName == "fixed_position_x") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flFixedX; }
		else if (variableName == "fixed_position_y") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flFixedY; }
		else if (variableName == "icon_offset") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flIconOffset; }
		else if (variableName == "icon_relative_offset") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flRelativeIconOffset; }
		else if (variableName == "caption_color") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_CaptionColor; }
		else if (variableName == "start_sound") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_StartSound; }
		else if (variableName == "vgui_target_name") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_VguiTargetName; }
		else if (variableName == "vgui_target_lookup") { value.type = ParamValue::VALUE_STRING; value.string = lesson.m_VguiTargetLookup; }
		else if (variableName == "vgui_target_edge") { value.type = ParamValue::VALUE_INTEGER; value.integer = lesson.m_iVguiTargetEdge; }
		else if (variableName == "lock_duration") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flLockDuration; }
		else if (variableName == "update_interval") { value.type = ParamValue::VALUE_FLOAT; value.number = lesson.m_flUpdateInterval; }
		else if (variableName == "can_open_when_dead") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bCanOpenWhenDead ? 1 : 0; }
		else if (variableName == "bump_with_timeout_when_learned") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bBumpWithTimeoutWhenLearned ? 1 : 0; }
		else if (variableName == "can_timeout_while_inactive") { value.type = ParamValue::VALUE_BOOLEAN; value.integer = lesson.m_bCanTimeoutWhileInactive ? 1 : 0; }
		return value;
	}

	bool SetVariable(CScriptedIconLesson &lesson, const std::string &name, const ParamValue &value)
	{
		const std::string variableName = NormalizeVariableName(name);
		const int i = value.type == ParamValue::VALUE_ENTITY ? value.integer :
			value.type == ParamValue::VALUE_BOOLEAN ? value.integer :
			value.type == ParamValue::VALUE_INTEGER ? value.integer :
			(int)value.number;
		const float f = value.type == ParamValue::VALUE_FLOAT ? value.number : (float)i;
		const std::string s = value.type == ParamValue::VALUE_STRING ? value.string :
			(value.type == ParamValue::VALUE_BOOLEAN ? (i ? "1" : "0") :
			(value.type == ParamValue::VALUE_ENTITY || value.type == ParamValue::VALUE_INTEGER ? std::to_string(i) : std::to_string(f)));

		if (variableName == "local_player" || variableName == "void") lesson.m_iLocalPlayer = i;
		else if (variableName == "entity1") lesson.m_iEntity1 = i;
		else if (variableName == "entity2") lesson.m_iEntity2 = i;
		else if (variableName == "icon_target") lesson.m_iIconTarget = i;
		else if (variableName == "integer1") lesson.m_iInteger1 = i;
		else if (variableName == "integer2") lesson.m_iInteger2 = i;
		else if (variableName == "output") lesson.m_flOutput = f;
		else if (variableName == "float1") lesson.m_flFloat1 = f;
		else if (variableName == "float2") lesson.m_flFloat2 = f;
		else if (variableName == "string1") lesson.m_String1 = s;
		else if (variableName == "string2") lesson.m_String2 = s;
		else if (variableName == "caption") lesson.m_Caption = s;
		else if (variableName == "caption_param") lesson.m_CaptionParam = s;
		else if (variableName == "binding") lesson.m_Binding = s;
		else if (variableName == "gamepad_binding") lesson.m_GamepadBinding = s;
		else if (variableName == "onscreen_icon") lesson.m_OnscreenIcon = s;
		else if (variableName == "offscreen_icon") lesson.m_OffscreenIcon = s;
		else if (variableName == "replace_key") lesson.m_ReplaceKey = s;
		else if (variableName == "group") lesson.m_Group = s;
		else if (variableName == "range") lesson.m_flRange = f;
		else if (variableName == "timeout") lesson.m_flTimeout = f;
		else if (variableName == "priority") lesson.m_iPriority = i;
		else if (variableName == "flags") lesson.m_iFlags = i;
		else if (variableName == "visible") lesson.m_bVisible = (i != 0);
		else if (variableName == "no_offscreen") lesson.m_bNoOffscreen = (i != 0);
		else if (variableName == "force_caption") lesson.m_bForceCaption = (i != 0);
		else if (variableName == "no_icon_target") lesson.m_bNoIconTarget = (i != 0);
		else if (variableName == "allow_nodraw_target") lesson.m_bAllowNodrawTarget = (i != 0);
		else if (variableName == "show_when_occluded") lesson.m_bShowWhenOccluded = (i != 0);
		else if (variableName == "fixed_position") lesson.m_bFixedPosition = (i != 0);
		else if (variableName == "fixed_position_x") lesson.m_flFixedX = f;
		else if (variableName == "fixed_position_y") lesson.m_flFixedY = f;
		else if (variableName == "icon_offset") lesson.m_flIconOffset = f;
		else if (variableName == "icon_relative_offset") lesson.m_flRelativeIconOffset = f;
		else if (variableName == "caption_color") lesson.m_CaptionColor = s;
		else if (variableName == "start_sound") lesson.m_StartSound = s;
		else if (variableName == "vgui_target_name") lesson.m_VguiTargetName = s;
		else if (variableName == "vgui_target_lookup") lesson.m_VguiTargetLookup = s;
		else if (variableName == "vgui_target_edge") lesson.m_iVguiTargetEdge = i;
		else if (variableName == "lock_duration") lesson.m_flLockDuration = f;
		else if (variableName == "update_interval") lesson.m_flUpdateInterval = f;
		else if (variableName == "can_open_when_dead") lesson.m_bCanOpenWhenDead = (i != 0);
		else if (variableName == "bump_with_timeout_when_learned") lesson.m_bBumpWithTimeoutWhenLearned = (i != 0);
		else if (variableName == "can_timeout_while_inactive") lesson.m_bCanTimeoutWhileInactive = (i != 0);
		else return false;
		return true;
	}

	bool ResolveParameter(CScriptedIconLesson &lesson, const InstructorEventData *event,
		const std::string &typeName, const std::string &paramName, ParamValue &value)
	{
		value = ParamValue();

		ParamValue scripted = GetVariable(lesson, paramName);
		if (scripted.type != ParamValue::VALUE_NONE)
		{
			value = scripted;
			return true;
		}

		if (event)
		{
			if (paramName == "userid" || paramName == "hint_activator_userid")
			{
				if (typeName == "player" || typeName == "entity")
				{
					cl_entity_t *localPlayer = gEngfuncs.GetLocalPlayer ?
						gEngfuncs.GetLocalPlayer() : NULL;
					value.type = ParamValue::VALUE_ENTITY;
					value.integer = localPlayer ? localPlayer->index : event->userId;
					return value.integer > 0;
				}
				value.type = ParamValue::VALUE_INTEGER;
				value.integer = event->userId;
				return true;
			}
			if (paramName == "hint_target" || paramName == "entity")
			{
				value.type = ParamValue::VALUE_ENTITY;
				value.integer = event->entityIndex;
				return event->entityIndex > 0;
			}
			if (paramName == "classname")
			{
				value.type = ParamValue::VALUE_STRING;
				value.string = event->className;
				return event->className[0] != '\0';
			}
			if (paramName == "targetname" || paramName == "hint_name")
			{
				value.type = ParamValue::VALUE_STRING;
				value.string = event->targetName;
				return event->targetName[0] != '\0';
			}
			if (paramName == "value")
			{
				if (typeName == "float")
				{
					value.type = ParamValue::VALUE_FLOAT;
					value.number = event->floatValue;
					return true;
				}
				if (typeName == "bool" || typeName == "boolean" ||
					typeName == "int" || typeName == "integer" ||
					typeName == "short" || typeName == "long")
				{
					value.type = (typeName == "bool" || typeName == "boolean") ?
						ParamValue::VALUE_BOOLEAN : ParamValue::VALUE_INTEGER;
					value.integer = event->integerValue;
					return true;
				}
				value.type = ParamValue::VALUE_STRING;
				value.string = event->stringValue;
				return event->stringValue[0] != '\0';
			}
			if (paramName == "int_value" || paramName == "hint_timeout")
			{
				value.type = ParamValue::VALUE_INTEGER;
				value.integer = event->integerValue;
				return true;
			}
			if (paramName == "hint_icon_offset" || paramName == "hint_range" ||
				paramName == "float_value")
			{
				value.type = ParamValue::VALUE_FLOAT;
				value.number = event->floatValue;
				return true;
			}
			if (paramName == "hint_flags" || paramName == "hint_local_player_only" ||
				paramName == "hint_allow_nodraw_target" || paramName == "hint_nooffscreen" ||
				paramName == "hint_forcecaption" || paramName == "int_value" ||
				paramName == "hint_timeout")
			{
				value.type = ParamValue::VALUE_INTEGER;
				value.integer = event->integerValue;
				return true;
			}
			if (paramName == event->fieldName && event->fieldName[0])
			{
				if (typeName == "float")
				{
					value.type = ParamValue::VALUE_FLOAT;
					value.number = event->floatValue;
					return true;
				}
				if (typeName == "bool" || typeName == "boolean" ||
					typeName == "int" || typeName == "integer" ||
					typeName == "short" || typeName == "long")
				{
					value.type = (typeName == "bool" || typeName == "boolean") ?
						ParamValue::VALUE_BOOLEAN : ParamValue::VALUE_INTEGER;
					value.integer = event->integerValue;
					return true;
				}
				value.type = ParamValue::VALUE_STRING;
				value.string = event->stringValue;
				return event->stringValue[0] != '\0';
			}
		}

		if (typeName == "void")
		{
			value.type = ParamValue::VALUE_NONE;
			return true;
		}

		if (typeName == "convar")
		{
			value.type = ParamValue::VALUE_FLOAT;
			value.number = CVAR_GET_FLOAT(paramName.c_str());
			return true;
		}

		if (typeName == "string")
		{
			if (paramName.compare(0, 7, "convar ") == 0)
			{
				value.type = ParamValue::VALUE_STRING;
				value.string = CVAR_GET_STRING(paramName.c_str() + 7);
				return true;
			}
			value.type = ParamValue::VALUE_STRING;
			value.string = paramName;
			return true;
		}

		if (typeName == "bool" || typeName == "boolean")
		{
			value.type = ParamValue::VALUE_BOOLEAN;
			value.integer = (StringEqualNoCase(paramName.c_str(), "true") || StringEqualNoCase(paramName.c_str(), "yes") || atoi(paramName.c_str()) != 0) ? 1 : 0;
			return true;
		}

		if (typeName == "float")
		{
			if (!IsNumericString(paramName.c_str()))
				return false;
			value.type = ParamValue::VALUE_FLOAT;
			value.number = (float)atof(paramName.c_str());
			return true;
		}

		if (typeName == "int" || typeName == "integer" || typeName == "short" || typeName == "long")
		{
			if (!IsNumericString(paramName.c_str()))
				return false;
			value.type = ParamValue::VALUE_INTEGER;
			value.integer = atoi(paramName.c_str());
			return true;
		}

		if (typeName == "player" || typeName == "entity")
			return false;

		return false;
	}

	int EntityDistance(int entityA, int entityB)
	{
		if (entityA <= 0 || entityB <= 0 || !gEngfuncs.GetEntityByIndex)
			return 0;
		cl_entity_t *a = gEngfuncs.GetEntityByIndex(entityA);
		cl_entity_t *b = gEngfuncs.GetEntityByIndex(entityB);
		if (!a || !b)
			return 0;
		const float x = a->origin[0] - b->origin[0];
		const float y = a->origin[1] - b->origin[1];
		const float z = a->origin[2] - b->origin[2];
		return (int)sqrtf(x * x + y * y + z * z);
	}

	void ParseColor(const std::string &color, int &r, int &g, int &b)
	{
		r = g = b = 255;
		if (color.empty())
			return;
		int values[3] = {255, 255, 255};
		if (sscanf(color.c_str(), "%d,%d,%d", &values[0], &values[1], &values[2]) == 3 ||
			sscanf(color.c_str(), "%d %d %d", &values[0], &values[1], &values[2]) == 3)
		{
			r = std::max(0, std::min(255, values[0]));
			g = std::max(0, std::min(255, values[1]));
			b = std::max(0, std::min(255, values[2]));
		}
	}

	void MakeCountCvarName(const char *lessonName, const char *suffix, char *out, size_t outSize)
	{
		char safeName[64];
		size_t n = 0;
		for (const char *p = lessonName; *p && n < sizeof(safeName) - 1; ++p)
		{
			char c = (char)tolower((unsigned char)*p);
			safeName[n++] = (isalnum((unsigned char)c) || c == '_') ? c : '_';
		}
		safeName[n] = '\0';
		snprintf(out, outSize, "gameinstructor_%s_%s", safeName, suffix);
	}
}

CBaseLesson::CBaseLesson(const char *name, bool definition)
	: m_Name(name ? name : ""),
	  m_pRoot(NULL),
	  m_InstanceType(LESSON_INSTANCE_MULTIPLE),
	  m_iPriority(100),
	  m_iFixedInstancesMax(1),
	  m_bReplaceOnlyWhenStopped(false),
	  m_iDisplayLimit(0),
	  m_iSuccessLimit(0),
	  m_iDisplayCount(0),
	  m_iSuccessCount(0),
	  m_flTimeout(0.0f),
	  m_flLockDuration(0.0f),
	  m_flUpdateInterval(0.5f),
	  m_flInitTime(0.0f),
	  m_flStartTime(0.0f),
	  m_flLockTime(0.0f),
	  m_bCanOpenWhenDead(false),
	  m_bBumpWithTimeoutWhenLearned(false),
	  m_bCanTimeoutWhileInactive(false),
	  m_bDisabled(false),
	  m_bWasDisplayed(false),
	  m_bSuccessCounted(false),
	  m_bCloseRequested(false),
	  m_bStarted(false),
	  m_bDefinition(definition)
{
	m_CloseReason = "None given.";
}

CBaseLesson::~CBaseLesson()
{
}

bool CBaseLesson::IsLearned() const
{
	const CBaseLesson *root = m_pRoot ? m_pRoot : this;
	if (root->m_iDisplayLimit > 0 && root->m_iDisplayCount >= root->m_iDisplayLimit)
		return true;
	if (root->m_iSuccessLimit > 0 && root->m_iSuccessCount >= root->m_iSuccessLimit)
		return true;
	return false;
}

bool CBaseLesson::PrerequisitesHaveBeenMet() const
{
	const CBaseLesson *root = m_pRoot ? m_pRoot : this;
	for (size_t i = 0; i < root->m_Prerequisites.size(); ++i)
	{
		const CScriptedIconLesson *prereq = GetGameInstructor().GetLesson(root->m_Prerequisites[i].c_str());
		if (!prereq || !prereq->IsLearned())
			return false;
	}
	return true;
}

bool CBaseLesson::IsTimedOut(float now)
{
	const CBaseLesson *root = m_pRoot ? m_pRoot : this;
	if (m_flTimeout <= 0.0f)
		return false;

	float startTime = m_bStarted ? m_flStartTime : m_flInitTime;
	if (root->IsLearned())
	{
		if (!m_bBumpWithTimeoutWhenLearned)
		{
			SetCloseReason("Learned.");
			return true;
		}
		startTime = m_flInitTime;
	}
	else if (!m_bStarted && !m_bCanTimeoutWhileInactive)
	{
		return false;
	}

	if (startTime + m_flTimeout < now)
	{
		SetCloseReason("Timed out.");
		return true;
	}
	return false;
}

bool CBaseLesson::IsLocked(float now) const
{
	if (m_flLockDuration <= 0.0f || !m_bStarted)
		return false;
	float lockTime = m_flLockTime > 0.0f ? m_flLockTime : m_flStartTime;
	return now > m_flStartTime + LESSON_MIN_TIME_BEFORE_LOCK_ALLOWED &&
		now < lockTime + m_flLockDuration;
}

void CBaseLesson::MarkSucceeded()
{
	if (m_bSuccessCounted)
		return;

	m_bSuccessCounted = true;
	CBaseLesson *root = m_pRoot ? m_pRoot : this;
	++root->m_iSuccessCount;
}

void CBaseLesson::MarkDisplayed()
{
	if (m_bWasDisplayed)
		return;

	m_bWasDisplayed = true;
	CBaseLesson *root = m_pRoot ? m_pRoot : this;
	++root->m_iDisplayCount;
}

void CBaseLesson::SetCloseReason(const char *reason)
{
	m_CloseReason = reason ? reason : "";
}

CTextLesson::CTextLesson(const char *name, bool definition)
	: CBaseLesson(name, definition)
{
}

CIconLesson::CIconLesson(const char *name, bool definition)
	: CTextLesson(name, definition),
	  m_iIconTarget(0),
	  m_iFlags(0),
	  m_flRange(0.0f),
	  m_flIconOffset(0.0f),
	  m_bNoIconTarget(false),
	  m_bAllowNodrawTarget(false),
	  m_bVisible(true),
	  m_bShowWhenOccluded(true),
	  m_bNoOffscreen(false),
	  m_bForceCaption(false),
	  m_bFixedPosition(false),
	  m_flFixedX(0.0f),
	  m_flFixedY(0.0f)
{
	m_CaptionColor = "255,255,255";
	m_iVguiTargetEdge = 0;
	m_flRelativeIconOffset = 0.0f;
}

CScriptedIconLesson::CScriptedIconLesson(const char *name, bool definition)
	: CIconLesson(name, definition),
	  m_iInstanceSerial(0),
	  m_iLocalPlayer(gEngfuncs.GetLocalPlayer && gEngfuncs.GetLocalPlayer() ? gEngfuncs.GetLocalPlayer()->index : 1),
	  m_iEntity1(0),
	  m_iEntity2(0),
	  m_iInteger1(0),
	  m_iInteger2(0),
	  m_iIconTargetVariable(0),
	  m_flOutput(0.0f),
	  m_flFloat1(0.0f),
	  m_flFloat2(0.0f),
	  m_pDefinition(NULL),
	  m_flNextUpdateEvent(0.0f),
	  m_flNextTargetValidation(0.0f)
{
}

CScriptedIconLesson::CScriptedIconLesson(const CScriptedIconLesson &definition)
	: CIconLesson(definition.m_Name.c_str(), false),
	  m_iInstanceSerial(0),
	  m_iLocalPlayer(1),
	  m_iEntity1(0),
	  m_iEntity2(0),
	  m_iInteger1(0),
	  m_iInteger2(0),
	  m_iIconTargetVariable(0),
	  m_flOutput(0.0f),
	  m_flFloat1(0.0f),
	  m_flFloat2(0.0f),
	  m_pDefinition(&definition),
	  m_flNextUpdateEvent(0.0f),
	  m_flNextTargetValidation(0.0f)
{
	m_pRoot = const_cast<CScriptedIconLesson *>(&definition);
	m_InstanceType = definition.m_InstanceType;
	m_ReplaceKey = definition.m_ReplaceKey;
	m_Group = definition.m_Group;
	m_iPriority = definition.m_iPriority;
	m_iFixedInstancesMax = definition.m_iFixedInstancesMax;
	m_bReplaceOnlyWhenStopped = definition.m_bReplaceOnlyWhenStopped;
	m_iDisplayLimit = definition.m_iDisplayLimit;
	m_iSuccessLimit = definition.m_iSuccessLimit;
	m_iDisplayCount = definition.m_iDisplayCount;
	m_iSuccessCount = definition.m_iSuccessCount;
	m_flTimeout = definition.m_flTimeout;
	m_flLockDuration = definition.m_flLockDuration;
	m_flUpdateInterval = definition.m_flUpdateInterval;
	m_bCanOpenWhenDead = definition.m_bCanOpenWhenDead;
	m_bBumpWithTimeoutWhenLearned = definition.m_bBumpWithTimeoutWhenLearned;
	m_bCanTimeoutWhileInactive = definition.m_bCanTimeoutWhileInactive;
	m_Caption = definition.m_Caption;
	m_CaptionParam = definition.m_CaptionParam;
	m_Binding = definition.m_Binding;
	m_GamepadBinding = definition.m_GamepadBinding;
	m_OnscreenIcon = definition.m_OnscreenIcon;
	m_OffscreenIcon = definition.m_OffscreenIcon;
	m_CaptionColor = definition.m_CaptionColor;
	m_VguiTargetName = definition.m_VguiTargetName;
	m_VguiTargetLookup = definition.m_VguiTargetLookup;
	m_iVguiTargetEdge = definition.m_iVguiTargetEdge;
	m_iFlags = definition.m_iFlags;
	m_flRange = definition.m_flRange;
	m_flIconOffset = definition.m_flIconOffset;
	m_flRelativeIconOffset = definition.m_flRelativeIconOffset;
	m_bNoIconTarget = definition.m_bNoIconTarget;
	m_bAllowNodrawTarget = definition.m_bAllowNodrawTarget;
	m_bVisible = definition.m_bVisible;
	m_bShowWhenOccluded = definition.m_bShowWhenOccluded;
	m_bNoOffscreen = definition.m_bNoOffscreen;
	m_bForceCaption = definition.m_bForceCaption;
	m_bFixedPosition = definition.m_bFixedPosition;
	m_flFixedX = definition.m_flFixedX;
	m_flFixedY = definition.m_flFixedY;
	m_StartSound = definition.m_StartSound;
	m_Prerequisites = definition.m_Prerequisites;
	m_flInitTime = definition.m_flInitTime;
	m_OpenEvents = definition.m_OpenEvents;
	m_CloseEvents = definition.m_CloseEvents;
	m_SuccessEvents = definition.m_SuccessEvents;
	m_OnOpenEvents = definition.m_OnOpenEvents;
	m_UpdateEvents = definition.m_UpdateEvents;
}

CScriptedIconLesson::~CScriptedIconLesson()
{
}

void CScriptedIconLesson::InitFromKeys(const std::string &text)
{
	KVNode root;
	KVReader reader(text.c_str());
	if (!reader.Parse(root))
		return;

	for (size_t i = 0; i < root.children.size(); ++i)
	{
		if (StringEqualNoCase(root.children[i].name.c_str(), m_Name.c_str()))
		{
			ParseLessonDefinition(this, root.children[i]);
			return;
		}
	}
}

void CScriptedIconLesson::InitPrerequisites()
{
	// Prerequisites are name references and are evaluated dynamically so that
	// script load order does not matter.
}

CScriptedIconLesson *CScriptedIconLesson::CreateOpportunity(float now) const
{
	CScriptedIconLesson *instance = new CScriptedIconLesson(*this);
	static int nextInstanceSerial = 1;
	if( nextInstanceSerial <= 0 )
		nextInstanceSerial = 1;
	instance->m_iInstanceSerial = nextInstanceSerial++;
	instance->m_flInitTime = now;
	instance->m_flStartTime = 0.0f;
	instance->m_flLockTime = 0.0f;
	instance->m_bWasDisplayed = false;
	instance->m_bCloseRequested = false;
	instance->m_bStarted = false;
	instance->m_CloseReason = "None given.";
	instance->m_iLocalPlayer = gEngfuncs.GetLocalPlayer && gEngfuncs.GetLocalPlayer() ? gEngfuncs.GetLocalPlayer()->index : 1;
	instance->m_iIconTarget = 0;
	instance->m_iIconTargetVariable = 0;
	instance->m_flNextUpdateEvent = now;
	return instance;
}

static bool ParseTypedParameter(CScriptedIconLesson &lesson, const InstructorEventData *event,
	const std::string &text, ParamValue &value, bool optional)
{
	size_t split = text.find(' ');
	std::string type = split == std::string::npos ? text : text.substr(0, split);
	std::string param = split == std::string::npos ? "" : text.substr(split + 1);
	while (!param.empty() && param[0] == ' ')
		param.erase(0, 1);

	if (type == "void")
	{
		value = ParamValue();
		return true;
	}
	if (ResolveParameter(lesson, event, type, param, value))
		return true;
	return optional;
}

static bool CompareValues(const ParamValue &left, const ParamValue &right, const std::string &action, bool negate)
{
	bool result = false;
	if (action == "is")
	{
		if (left.type == ParamValue::VALUE_STRING || right.type == ParamValue::VALUE_STRING)
			result = left.string == right.string;
		else
		{
			const float a = left.type == ParamValue::VALUE_FLOAT ? left.number : (float)left.integer;
			const float b = right.type == ParamValue::VALUE_FLOAT ? right.number : (float)right.integer;
			result = fabsf(a - b) < 0.0001f;
		}
	}
	else if (action == "less than")
	{
		if (left.type == ParamValue::VALUE_STRING || right.type == ParamValue::VALUE_STRING)
			result = left.string < right.string;
		else
		{
			const float a = left.type == ParamValue::VALUE_FLOAT ? left.number : (float)left.integer;
			const float b = right.type == ParamValue::VALUE_FLOAT ? right.number : (float)right.integer;
			result = a < b;
		}
	}
	else if (action == "has prefix")
	{
		result = left.string.compare(0, right.string.size(), right.string) == 0;
	}
	else if (action == "has bit")
	{
		result = (static_cast<unsigned int>(left.integer) & (1U << (right.integer & 31))) != 0;
	}
	else if (action == "bit count is" || action == "bit count less than")
	{
		unsigned int bits = (unsigned int)left.integer;
		int count = 0;
		while (bits)
		{
			count += (bits & 1U);
			bits >>= 1;
		}
		result = action == "bit count is" ? count == right.integer : count < right.integer;
	}
	return negate ? !result : result;
}

bool CScriptedIconLesson::ProcessElements(const InstructorEventData *event,
	const std::vector<InstructorLessonElement> &elements)
{
	int scopeDepth = 0;
	int continueScope = -1;

	for (size_t i = 0; i < elements.size(); ++i)
	{
		std::string variable, action;
		bool optional = false, negate = false;
		if (!ParseExpression(elements[i].expression, variable, action, optional, negate))
		{
			if (scopeDepth > 0)
			{
				continueScope = scopeDepth - 1;
				continue;
			}
			return false;
		}

		if (action == "scope in")
		{
			++scopeDepth;
			continue;
		}
		if (action == "scope out")
		{
			if (scopeDepth > 0)
				--scopeDepth;
			if (continueScope == scopeDepth)
				continueScope = -1;
			continue;
		}
		if (continueScope >= 0)
			continue;

		if (action == "close")
		{
			m_bCloseRequested = true;
			SetCloseReason("Close action.");
			continue;
		}
		if (action == "success")
		{
			GetGameInstructor().MarkSucceeded(this);
			continue;
		}
		if (action == "lock")
		{
			m_flLockTime = gHUD.m_flTime;
			continue;
		}
		if (action == "present start")
		{
			HUD_ApplyManagedInstructorPresentation(m_Name.c_str(), m_iInstanceSerial, true);
			continue;
		}
		if (action == "present end")
		{
			HUD_ApplyManagedInstructorPresentation(m_Name.c_str(), m_iInstanceSerial, false);
			continue;
		}
		if (action == "present complete")
		{
			const bool complete = HUD_IsManagedInstructorPresentationComplete(
				m_Name.c_str(), m_iInstanceSerial);
			if (negate ? complete : !complete)
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}
			continue;
		}
		if (action == "reference open")
		{
			std::string referenceName = elements[i].parameter;
			const size_t split = referenceName.find(' ');
			if (split != std::string::npos)
				referenceName = referenceName.substr(split + 1);
			const CScriptedIconLesson *reference = GetGameInstructor().GetLesson(referenceName.c_str());
			bool isOpen = reference && GetGameInstructor().IsLessonOfSameTypeOpen(reference);
			if (negate)
				isOpen = !isOpen;
			if (!isOpen)
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}
			continue;
		}

		ParamValue parameter;
		if (!ParseTypedParameter(*this, event, elements[i].parameter, parameter, optional))
		{
			if (optional)
				continue;
			if (scopeDepth > 0)
			{
				continueScope = scopeDepth - 1;
				continue;
			}
			return false;
		}
		if (parameter.type == ParamValue::VALUE_NONE && action != "set")
		{
			if (optional)
				continue;
			if (scopeDepth > 0)
			{
				continueScope = scopeDepth - 1;
				continue;
			}
			return false;
		}

		if (action == "set")
		{
			if (!SetVariable(*this, variable, parameter))
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}
			continue;
		}

		ParamValue current = GetVariable(*this, variable);
		if (current.type == ParamValue::VALUE_NONE)
		{
			if (scopeDepth > 0)
			{
				continueScope = scopeDepth - 1;
				continue;
			}
			return false;
		}

		if (action == "add" || action == "subtract" || action == "multiply")
		{
			if (current.type == ParamValue::VALUE_STRING)
			{
				if (action == "add" && parameter.type == ParamValue::VALUE_STRING)
				{
					current.string += parameter.string;
					if (SetVariable(*this, variable, current))
						continue;
				}
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}

			if (current.type == ParamValue::VALUE_FLOAT)
			{
				const float a = current.number;
				const float b = parameter.type == ParamValue::VALUE_FLOAT ?
					parameter.number : (float)parameter.integer;
				current.number = action == "add" ? a + b : action == "subtract" ? a - b : a * b;
			}
			else
			{
				const int a = current.integer;
				const int b = parameter.type == ParamValue::VALUE_FLOAT ?
					(int)parameter.number : parameter.integer;
				current.integer = action == "add" ? a + b : action == "subtract" ? a - b : a * b;
			}
			if (!SetVariable(*this, variable, current))
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}
			continue;
		}

		if (action == "classname is" || action == "modelname is")
		{
			if (current.type != ParamValue::VALUE_ENTITY || parameter.type != ParamValue::VALUE_STRING)
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}

			const char *actualName = "";
			if (action == "classname is" && event && current.integer == event->entityIndex)
				actualName = event->className;
			else if (action == "modelname is" && gEngfuncs.GetEntityByIndex)
			{
				cl_entity_t *entity = gEngfuncs.GetEntityByIndex(current.integer);
				if (entity && entity->model)
					actualName = entity->model->name;
			}

			bool matches = StringEqualNoCase(actualName, parameter.string.c_str());
			if (negate)
				matches = !matches;
			if (!matches)
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}
			continue;
		}

		if (action == "get use target" || action == "use target is")
		{
			const bool isLocalPlayer = current.type == ParamValue::VALUE_ENTITY &&
				current.integer == m_iLocalPlayer;
			const bool hasUseEvent = event && StringEqualNoCase(event->name, "player_use") &&
				event->entityIndex > 0;
			if (!isLocalPlayer || !hasUseEvent)
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}

			if (action == "get use target")
			{
				const char *destination = parameter.integer == 2 ? "entity2" : "entity1";
				ParamValue target;
				target.type = ParamValue::VALUE_ENTITY;
				target.integer = event->entityIndex;
				if (!SetVariable(*this, destination, target))
				{
					if (scopeDepth > 0)
					{
						continueScope = scopeDepth - 1;
						continue;
					}
					return false;
				}
			}
			else
			{
				bool matches = event->entityIndex == parameter.integer;
				if (negate)
					matches = !matches;
				if (!matches)
				{
					if (scopeDepth > 0)
					{
						continueScope = scopeDepth - 1;
						continue;
					}
					return false;
				}
			}
			continue;
		}

		if (action == "get distance" || action == "get angular distance")
		{
			if (current.type != ParamValue::VALUE_ENTITY || parameter.type != ParamValue::VALUE_ENTITY)
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}
			if (!gEngfuncs.GetEntityByIndex ||
				!gEngfuncs.GetEntityByIndex(current.integer) ||
				!gEngfuncs.GetEntityByIndex(parameter.integer))
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}

			ParamValue distance;
			distance.type = ParamValue::VALUE_FLOAT;
			if (action == "get distance")
			{
				distance.number = (float)EntityDistance(current.integer, parameter.integer);
			}
			else
			{
				cl_entity_t *from = gEngfuncs.GetEntityByIndex(current.integer);
				cl_entity_t *to = gEngfuncs.GetEntityByIndex(parameter.integer);
				float dx = to->origin[0] - from->origin[0];
				float dy = to->origin[1] - from->origin[1];
				float dz = to->origin[2] - from->origin[2];
				const float length = sqrtf(dx * dx + dy * dy + dz * dz);
				if (length <= 0.0001f)
					distance.number = 0.0f;
				else
				{
					dx /= length; dy /= length; dz /= length;
					const float pitch = from->angles[0] * 0.017453292519943295f;
					const float yaw = from->angles[1] * 0.017453292519943295f;
					const float fx = cosf(pitch) * cosf(yaw);
					const float fy = cosf(pitch) * sinf(yaw);
					const float fz = -sinf(pitch);
					float dot = fx * dx + fy * dy + fz * dz;
					dot = dot < -1.0f ? -1.0f : dot > 1.0f ? 1.0f : dot;
					distance.number = acosf(dot) * 57.29577951308232f;
				}
			}
			if (!SetVariable(*this, "output", distance))
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}
			continue;
		}

		if (action == "is" || action == "less than" || action == "has prefix" ||
			action == "has bit" || action == "bit count is" || action == "bit count less than")
		{
			if (!CompareValues(current, parameter, action, negate))
			{
				if (scopeDepth > 0)
				{
					continueScope = scopeDepth - 1;
					continue;
				}
				return false;
			}
			continue;
		}

		if (scopeDepth > 0)
		{
			continueScope = scopeDepth - 1;
			continue;
		}
		return false;
	}

	return scopeDepth == 0;
}

void CScriptedIconLesson::OnOpen()
{
	if (!m_pDefinition)
		return;
	for (size_t i = 0; i < m_OnOpenEvents.size(); ++i)
		ProcessElements(NULL, m_OnOpenEvents[i].elements);
}

void CScriptedIconLesson::Update(float now)
{
	// Source runs scripted update events at their configured interval, but the
	// icon lesson's target validation runs every frame independently.
	if (now >= m_flNextUpdateEvent)
	{
		m_flNextUpdateEvent = now + (m_flUpdateInterval > 0.0f ? m_flUpdateInterval : 0.5f);
		for (size_t i = 0; i < m_UpdateEvents.size(); ++i)
			ProcessElements(NULL, m_UpdateEvents[i].elements);
	}

	ValidateIconTarget(now);
}

void CScriptedIconLesson::UpdateInactive(float now)
{
	// Source's CIconLesson::UpdateInactive keeps target handles valid even when
	// another lesson owns the HUD. GoldSrc has no EHANDLE/dormancy API, so use
	// the client entity index and the NODRAW bit exposed by cl_entity_t.
	ValidateIconTarget(now);
}

bool CScriptedIconLesson::ValidateIconTarget(float now)
{
	if (m_bCloseRequested || now < m_flNextTargetValidation)
		return !m_bCloseRequested;

	m_flNextTargetValidation = now + 0.1f;

	if (m_iIconTarget <= 0)
	{
		if (!m_bNoIconTarget)
		{
			m_bCloseRequested = true;
			SetCloseReason("Lost our icon target handle returned NULL.");
			return false;
		}
		return true;
	}

	if (!gEngfuncs.GetEntityByIndex)
		return true;

	cl_entity_t *target = gEngfuncs.GetEntityByIndex(m_iIconTarget);
	if (!target)
	{
		if (!m_bNoIconTarget)
		{
			m_bCloseRequested = true;
			SetCloseReason("Lost our icon target handle returned NULL.");
			return false;
		}
		return true;
	}

	if ((target->curstate.effects & EF_NODRAW) && !m_bAllowNodrawTarget)
	{
		m_bCloseRequested = true;
		SetCloseReason("Icon Target is using effect NODRAW and allow_nodraw_target is false!");
		return false;
	}

	return true;
}

void CScriptedIconLesson::ProcessEvent(const InstructorEventData &event, int eventType)
{
	std::vector<InstructorLessonEvent> *events = NULL;
	if (eventType == 0) events = &m_OpenEvents;
	else if (eventType == 1) events = &m_CloseEvents;
	else if (eventType == 2) events = &m_SuccessEvents;
	if (!events)
		return;

	for (size_t i = 0; i < events->size(); ++i)
	{
		if ((*events)[i].name != event.name)
			continue;

		if (ProcessElements(&event, (*events)[i].elements))
		{
			// Source treats a successful success-event rule as the completion
			// signal for this lesson; it is not necessary to add "void success"
			// manually to every script.
			if (eventType == 2)
				GetGameInstructor().MarkSucceeded(this);
			else if (eventType == 1)
			{
				m_bCloseRequested = true;
				SetCloseReason("Close event.");
			}
		}
	}
}

C_GameInstructor::C_GameInstructor()
	: m_bInitialized(false),
	  m_bNoDraw(false),
	  m_bHiddenDueToOtherElements(false),
	  m_bManagedLessonVisible(false),
	  m_ManagedLessonName(),
	  m_iCurrentPriority(0),
	  m_flLastUpdate(0.0f),
	  m_flNextStartSoundTime(0.0f)
{
}

C_GameInstructor::~C_GameInstructor()
{
	Shutdown();
}

C_GameInstructor &GetGameInstructor()
{
	static C_GameInstructor instructor;
	return instructor;
}

bool C_GameInstructor::Init()
{
	if (m_bInitialized)
		return true;

	CVAR_CREATE("gameinstructor_enable", "1", FCVAR_ARCHIVE);
	CVAR_CREATE("gameinstructor_verbose", "0", 0);
	CVAR_CREATE("gameinstructor_find_errors", "0", 0);
	// Source locator presentation stays centered for this many seconds after
	// StartPresent; the name/default match locator_lerp_rest.
	CVAR_CREATE("locator_lerp_rest", "2.25", 0);

	m_bNoDraw = false;
	m_bHiddenDueToOtherElements = false;
	m_iCurrentPriority = 0;
	m_flNextStartSoundTime = 0.0f;
	m_PreviousStartSound.clear();

	ReadLessonsFromFile(GAMEINSTRUCTOR_MOD_SCRIPT_FILE);
	ReadLessonsFromFile(GAMEINSTRUCTOR_SCRIPT_FILE);
	InitLessonPrerequisites();
	EvaluateLessonsForGameRules();

	m_bInitialized = true;
	return true;
}

void C_GameInstructor::Shutdown()
{
	CloseAllOpenOpportunities();
	HideManagedLesson();

	for (size_t i = 0; i < m_Lessons.size(); ++i)
		delete m_Lessons[i];
	m_Lessons.clear();
	m_GroupConVarToggles.clear();
	m_bInitialized = false;
}

void C_GameInstructor::ReadLessonsFromFile(const char *fileName)
{
	KVNode root;
	if (!LoadKVFile(fileName, root))
		return;

	for (size_t i = 0; i < root.children.size(); ++i)
	{
		const KVNode &node = root.children[i];
		if (StringEqualNoCase(node.name.c_str(), "GroupConVarToggle"))
		{
			std::pair<std::string, std::string> toggle;
			toggle.first = ChildString(node, "convar");
			toggle.second = ChildString(node, "group");
			if (!toggle.first.empty() && !toggle.second.empty())
				m_GroupConVarToggles.push_back(toggle);
			continue;
		}

		if (StringEqualNoCase(node.name.c_str(), "version number") || node.name.empty())
			continue;
		if (GetLesson(node.name.c_str()))
			continue;

		CScriptedIconLesson *lesson = new CScriptedIconLesson(node.name.c_str(), true);
		ParseLessonDefinition(lesson, node);
		m_Lessons.push_back(lesson);
		LoadCounts(lesson);
		if (InstructorVerboseEnabled() && gEngfuncs.Con_Printf)
			gEngfuncs.Con_Printf("[GameInstructor] Registered lesson '%s' from '%s'.\n",
				lesson->m_Name.c_str(), fileName);
	}
}

void C_GameInstructor::InitLessonPrerequisites()
{
	// All names resolve only after both script files have been loaded.
}

const CScriptedIconLesson *C_GameInstructor::GetLesson(const char *name) const
{
	if (!name)
		return NULL;
	for (size_t i = 0; i < m_Lessons.size(); ++i)
	{
		if (StringEqualNoCase(m_Lessons[i]->m_Name.c_str(), name))
			return m_Lessons[i];
	}
	return NULL;
}

bool C_GameInstructor::IsLessonOfSameTypeOpen(const CScriptedIconLesson *lesson) const
{
	if (!lesson)
		return false;
	for (size_t i = 0; i < m_OpenOpportunities.size(); ++i)
	{
		if (StringEqualNoCase(m_OpenOpportunities[i]->m_Name.c_str(), lesson->m_Name.c_str()))
			return true;
	}
	return false;
}

bool C_GameInstructor::OpenOpportunity(CScriptedIconLesson *lesson)
{
	if (!lesson || !lesson->m_pRoot)
	{
		delete lesson;
		return false;
	}

	CScriptedIconLesson *root = static_cast<CScriptedIconLesson *>(lesson->m_pRoot);
	if (lesson->PrerequisitesHaveBeenMet() == false)
	{
		delete lesson;
		return false;
	}

	int sameTypeCount = 0;
	CScriptedIconLesson *replace = NULL;
	for (size_t i = 0; i < m_OpenOpportunities.size(); ++i)
	{
		CScriptedIconLesson *open = m_OpenOpportunities[i];
		if (open->m_pRoot != root)
			continue;
		if (root->m_InstanceType == LESSON_INSTANCE_SINGLE_OPEN)
		{
			delete lesson;
			return false;
		}
		if (root->m_InstanceType == LESSON_INSTANCE_FIXED_REPLACE &&
			open->m_ReplaceKey == lesson->m_ReplaceKey)
		{
			++sameTypeCount;
			if (!root->m_bReplaceOnlyWhenStopped || !open->IsInstructing())
				replace = open;
		}
	}

	if (root->m_InstanceType == LESSON_INSTANCE_FIXED_REPLACE && sameTypeCount >= root->m_iFixedInstancesMax)
	{
		if (replace)
			CloseOpportunity(replace, "Replaced by newer instance.");
		else
		{
			delete lesson;
			return false;
		}
	}

	m_OpenOpportunities.push_back(lesson);
	return true;
}

void C_GameInstructor::CloseOpportunity(CScriptedIconLesson *lesson, const char *reason)
{
	if (!lesson)
		return;
	if (reason && reason[0])
		lesson->SetCloseReason(reason);
	UpdateInactiveLesson(lesson, gHUD.m_flTime);
	if (lesson->WasDisplayed() && lesson->m_pRoot)
		SaveCounts(static_cast<CScriptedIconLesson *>(lesson->m_pRoot));

	std::vector<CScriptedIconLesson*>::iterator it =
		std::find(m_OpenOpportunities.begin(), m_OpenOpportunities.end(), lesson);
	if (it != m_OpenOpportunities.end())
		m_OpenOpportunities.erase(it);
	delete lesson;
}

void C_GameInstructor::CloseAllOpenOpportunities()
{
	while (!m_OpenOpportunities.empty())
		CloseOpportunity(m_OpenOpportunities.back(), "All opportunities closed.");
}

void C_GameInstructor::StopAllLessons()
{
	for (size_t i = 0; i < m_OpenOpportunities.size(); ++i)
		UpdateInactiveLesson(m_OpenOpportunities[i], gHUD.m_flTime);
	HideManagedLesson();
}

void C_GameInstructor::MarkDisplayed(const char *name)
{
	if (!name)
		return;

	CScriptedIconLesson *displayed = NULL;
	for (size_t i = 0; i < m_OpenOpportunities.size(); ++i)
	{
		CScriptedIconLesson *lesson = m_OpenOpportunities[i];
		if (StringEqualNoCase(lesson->m_Name.c_str(), name) &&
			lesson->IsInstructing() && !lesson->WasDisplayed() &&
			(!displayed || lesson->m_iPriority > displayed->m_iPriority))
		{
			displayed = lesson;
		}
	}

	if (!displayed)
		return;

	displayed->MarkDisplayed();
	if (displayed->m_pRoot)
		SaveCounts(static_cast<CScriptedIconLesson *>(displayed->m_pRoot));
}

void C_GameInstructor::MarkSucceeded(const char *name)
{
	if (!name)
		return;

	for (size_t i = 0; i < m_OpenOpportunities.size(); ++i)
	{
		CScriptedIconLesson *lesson = m_OpenOpportunities[i];
		if (StringEqualNoCase(lesson->m_Name.c_str(), name))
			MarkSucceeded(lesson);
	}
}

void C_GameInstructor::MarkSucceeded(CScriptedIconLesson *lesson)
{
	if (!lesson)
		return;

	lesson->MarkSucceeded();
	if (lesson->m_pRoot)
		SaveCounts(static_cast<CScriptedIconLesson *>(lesson->m_pRoot));
}

void C_GameInstructor::LoadCounts(CScriptedIconLesson *lesson)
{
	if (!lesson || !gEngfuncs.pfnGetCvarFloat)
		return;
	char displayName[128], successName[128];
	MakeCountCvarName(lesson->m_Name.c_str(), "display", displayName, sizeof(displayName));
	MakeCountCvarName(lesson->m_Name.c_str(), "success", successName, sizeof(successName));
	CVAR_CREATE(displayName, "0", FCVAR_ARCHIVE);
	CVAR_CREATE(successName, "0", FCVAR_ARCHIVE);
	lesson->m_iDisplayCount = (int)CVAR_GET_FLOAT(displayName);
	lesson->m_iSuccessCount = (int)CVAR_GET_FLOAT(successName);
}

void C_GameInstructor::SaveCounts(const CScriptedIconLesson *lesson)
{
	if (!lesson || !gEngfuncs.Cvar_Set)
		return;
	char displayName[128], successName[128], displayValue[32], successValue[32];
	MakeCountCvarName(lesson->m_Name.c_str(), "display", displayName, sizeof(displayName));
	MakeCountCvarName(lesson->m_Name.c_str(), "success", successName, sizeof(successName));
	snprintf(displayValue, sizeof(displayValue), "%d", lesson->m_iDisplayCount);
	snprintf(successValue, sizeof(successValue), "%d", lesson->m_iSuccessCount);
	gEngfuncs.Cvar_Set(displayName, displayValue);
	gEngfuncs.Cvar_Set(successName, successValue);
}

void C_GameInstructor::EvaluateLessonsForGameRules()
{
	for (size_t i = 0; i < m_Lessons.size(); ++i)
		m_Lessons[i]->m_bDisabled = false;

	for (size_t i = 0; i < m_GroupConVarToggles.size(); ++i)
	{
		if (CVAR_GET_FLOAT(m_GroupConVarToggles[i].first.c_str()) != 0.0f)
			SetLessonGroupEnabled(m_GroupConVarToggles[i].second.c_str(), false);
	}
}

void C_GameInstructor::SetLessonGroupEnabled(const char *group, bool enabled)
{
	if (!group)
		return;
	for (size_t i = 0; i < m_Lessons.size(); ++i)
	{
		if (StringEqualNoCase(m_Lessons[i]->m_Group.c_str(), group))
			m_Lessons[i]->m_bDisabled = !enabled;
	}
}

void C_GameInstructor::DumpOpenOpportunities() const
{
	for (size_t i = 0; i < m_OpenOpportunities.size(); ++i)
		gEngfuncs.Con_Printf("Instructor: open lesson '%s' priority %d target %d\n",
			m_OpenOpportunities[i]->m_Name.c_str(), m_OpenOpportunities[i]->m_iPriority,
			m_OpenOpportunities[i]->m_iIconTarget);
}

void C_GameInstructor::ProcessDefinitionEvent(CScriptedIconLesson *definition,
	const InstructorEventData &event, int eventType)
{
	if (!definition || definition->m_bDisabled || eventType != 0)
		return;
	if (m_bNoDraw && !definition->m_bCanOpenWhenDead)
		return;

	for (size_t i = 0; i < definition->m_OpenEvents.size(); ++i)
	{
		if (definition->m_OpenEvents[i].name != event.name)
			continue;
		if (InstructorVerboseEnabled() && gEngfuncs.Con_Printf)
			gEngfuncs.Con_Printf("[GameInstructor] Lesson '%s' matched open event '%s'.\n",
				definition->m_Name.c_str(), event.name);
		CScriptedIconLesson *instance = definition->CreateOpportunity(gHUD.m_flTime);
		if (instance->ProcessElements(&event, definition->m_OpenEvents[i].elements))
		{
			if (!instance->m_bCloseRequested)
			{
				if (InstructorVerboseEnabled() && gEngfuncs.Con_Printf)
					gEngfuncs.Con_Printf("[GameInstructor] Opening opportunity '%s'.\n",
						definition->m_Name.c_str());
				OpenOpportunity(instance);
			}
			else
				delete instance;
		}
		else
			delete instance;
	}
}

void C_GameInstructor::ProcessOpportunityEvent(CScriptedIconLesson *opportunity,
	const InstructorEventData &event, int eventType)
{
	if (!opportunity)
		return;
	opportunity->ProcessEvent(event, eventType);
}

void C_GameInstructor::FireGameEvent(const InstructorEventData &event)
{
	if (!m_bInitialized)
		Init();

	if (InstructorVerboseEnabled() && gEngfuncs.Con_Printf)
		gEngfuncs.Con_Printf("[GameInstructor] Event '%s' entity %d class '%s'.\n",
			event.name, event.entityIndex, event.className);

	if (!StringEqualNoCase(event.name, "player_death") &&
		!StringEqualNoCase(event.name, "player_spawn") && m_bNoDraw)
		return;

	if (StringEqualNoCase(event.name, "player_death"))
	{
		m_bNoDraw = true;
		for (int i = (int)m_OpenOpportunities.size() - 1; i >= 0; --i)
		{
			CScriptedIconLesson *lesson = m_OpenOpportunities[(size_t)i];
			if (!lesson->m_bCanOpenWhenDead)
				CloseOpportunity(lesson, "Player died.");
		}
		HideManagedLesson();
	}
	else if (StringEqualNoCase(event.name, "player_spawn"))
	{
		m_bNoDraw = false;
	}

	for (size_t i = 0; i < m_Lessons.size(); ++i)
		ProcessDefinitionEvent(m_Lessons[i], event, 0);

	for (int i = (int)m_OpenOpportunities.size() - 1; i >= 0; --i)
	{
		CScriptedIconLesson *lesson = m_OpenOpportunities[(size_t)i];
		ProcessOpportunityEvent(lesson, event, 2);
		ProcessOpportunityEvent(lesson, event, 1);
	}
}

void C_GameInstructor::UpdateActiveLesson(CScriptedIconLesson *lesson)
{
	if (!lesson)
		return;
	if (!lesson->IsInstructing() && !lesson->IsLearned())
	{
		lesson->m_flStartTime = gHUD.m_flTime;
		lesson->m_bStarted = true;

		// Source creates the icon/locator target in CIconLesson::Start before
		// scripted OnOpen events run. Create the GoldSrc managed target here so
		// present_start/end actions in OnOpen can address it immediately.
		DisplayLesson(lesson, gHUD.m_flTime);
		lesson->OnOpen();
	}
	if (lesson->IsInstructing())
		lesson->Update(gHUD.m_flTime);
}

void C_GameInstructor::UpdateInactiveLesson(CScriptedIconLesson *lesson, float now)
{
	if (!lesson)
		return;

	// Source stops a lesson when it loses the active presentation slot. Its
	// start-time clock is reset, so timeout only advances while active unless
	// can_timeout_while_inactive is set. On the next activation OnOpen runs again.
	if (lesson->IsInstructing())
	{
		lesson->m_bStarted = false;
		lesson->m_flStartTime = 0.0f;
	}

	lesson->UpdateInactive(now);
}

void C_GameInstructor::DisplayLesson(const CScriptedIconLesson *lesson, float now)
{
	if (!lesson)
		return;
	if (!lesson->m_bVisible || (!lesson->m_bNoIconTarget && !lesson->m_bFixedPosition && lesson->m_iIconTarget <= 0))
		return;

	int r, g, b;
	ParseColor(lesson->m_CaptionColor, r, g, b);

	const char *caption = lesson->m_Caption.c_str();
	if (!lesson->m_CaptionParam.empty())
	{
		static char combined[256];
		snprintf(combined, sizeof(combined), "%s %s", caption, lesson->m_CaptionParam.c_str());
		caption = combined;
	}

	if (!lesson->m_StartSound.empty() && lesson->m_StartSound != m_PreviousStartSound &&
		now >= m_flNextStartSoundTime)
	{
		HUD_PlayInstructorSound(lesson->m_StartSound.c_str());
		m_PreviousStartSound = lesson->m_StartSound;
		m_flNextStartSoundTime = now + 4.0f;
	}


	HUD_SetManagedInstructorHint(lesson->m_Name.c_str(), lesson->m_iInstanceSerial, caption,
		lesson->m_Binding.c_str(), lesson->m_OnscreenIcon.c_str(),
		lesson->m_OffscreenIcon.c_str(), lesson->m_iIconTarget, lesson->m_iPriority, 0.0f,
		lesson->m_iFlags | (lesson->m_bNoOffscreen ? 0x00000200 : 0) |
			(lesson->m_bAllowNodrawTarget ? 0x00010000 : 0), r, g, b, lesson->m_flRange,
			lesson->m_flIconOffset, lesson->m_bForceCaption,
		lesson->m_bFixedPosition, lesson->m_flFixedX, lesson->m_flFixedY);
	m_bManagedLessonVisible = true;
}

void C_GameInstructor::HideManagedLesson()
{
	HUD_ClearManagedInstructorHint();
	m_bManagedLessonVisible = false;
	m_ManagedLessonName.clear();
}

void C_GameInstructor::Update(float now)
{
	if (!m_bInitialized)
		Init();
	// CHud::Redraw applies the global hide-HUD gate to this card as well.
	m_bHiddenDueToOtherElements = false;
	if (CVAR_GET_FLOAT("gameinstructor_enable") == 0.0f || m_bNoDraw || m_bHiddenDueToOtherElements)
	{
		StopAllLessons();
		return;
	}

	if (m_flLastUpdate > now)
		m_flLastUpdate = now;
	m_flLastUpdate = now;

	for (int i = (int)m_OpenOpportunities.size() - 1; i >= 0; --i)
	{
		CScriptedIconLesson *lesson = m_OpenOpportunities[(size_t)i];
		if (lesson->m_bCloseRequested || lesson->IsTimedOut(now))
		{
			CloseOpportunity(lesson, lesson->m_CloseReason.c_str());
			continue;
		}
	}

	// Clear per-frame single-active reservation just like Source's Update().
	for (size_t i = 0; i < m_Lessons.size(); ++i)
		m_Lessons[i]->m_bStarted = false;

	int currentPriority = 0;
	int displayPriority = -2147483647;
	CScriptedIconLesson *displayLessons[MAX_INSTRUCTOR_LOCATOR_TARGETS];
	int displayCount = 0;

	// Process higher-priority opportunities first. Preserve the manager's
	// newest-first order for equal priorities, matching the previous reverse
	// traversal while making arbitration independent of event/open order.
	std::vector<CScriptedIconLesson *> priorityOrder;
	priorityOrder.reserve( m_OpenOpportunities.size() );
	for( int i = (int)m_OpenOpportunities.size() - 1; i >= 0; --i )
		priorityOrder.push_back( m_OpenOpportunities[(size_t)i] );

	for( size_t i = 1; i < priorityOrder.size(); ++i )
	{
		CScriptedIconLesson *key = priorityOrder[i];
		size_t j = i;
		while( j > 0 && priorityOrder[j - 1]->m_iPriority < key->m_iPriority )
		{
			priorityOrder[j] = priorityOrder[j - 1];
			--j;
		}
		priorityOrder[j] = key;
	}

	// Retain every eligible lesson at the winning presentation priority.
	for( size_t i = 0; i < priorityOrder.size(); ++i )
	{
		CScriptedIconLesson *lesson = priorityOrder[i];
		CScriptedIconLesson *root = lesson->m_pRoot ?
			static_cast<CScriptedIconLesson *>(lesson->m_pRoot) : lesson;
		const bool learned = lesson->IsLearned();
		const bool shouldDisplay = lesson->m_bVisible &&
			(lesson->m_bNoIconTarget || lesson->m_bFixedPosition || lesson->m_iIconTarget > 0);
		const bool locked = lesson->IsLocked(now);
		const bool singleActiveBlocked = root->m_InstanceType == LESSON_INSTANCE_SINGLE_ACTIVE &&
			root->m_bStarted;

		if ((shouldDisplay || locked) &&
			(lesson->m_iPriority >= currentPriority || lesson->m_iPriority == 0 || locked) &&
			!singleActiveBlocked)
		{
			UpdateActiveLesson(lesson);
			if (lesson->IsInstructing() || learned)
			{
				root->m_bStarted = true;
				if (!learned && shouldDisplay)
				{
					if (lesson->m_iPriority > displayPriority)
					{
						displayPriority = lesson->m_iPriority;
						displayCount = 0;
					}
					if (lesson->m_iPriority == displayPriority &&
						displayCount < MAX_INSTRUCTOR_LOCATOR_TARGETS)
						displayLessons[displayCount++] = lesson;
				}
				if (lesson->m_iPriority > currentPriority && lesson->m_iPriority != 0)
					currentPriority = lesson->m_iPriority;
			}
		}
		else
		{
			UpdateInactiveLesson(lesson, now);
		}
	}

	m_iCurrentPriority = currentPriority;
	if (displayCount > 0)
	{
		for (int i = 0; i < displayCount; ++i)
			DisplayLesson(displayLessons[i], now);
		m_bManagedLessonVisible = true;
		HUD_PruneManagedInstructorHints();
	}
	else
	{
		HideManagedLesson();
	}

}

void GameInstructor_Init()
{
	GetGameInstructor().Init();
}

void GameInstructor_Shutdown()
{
	GetGameInstructor().Shutdown();
}

void GameInstructor_Update(float now)
{
	GetGameInstructor().Update(now);
}

void GameInstructor_FireEvent(const InstructorEventData &event)
{
	GetGameInstructor().FireGameEvent(event);
}

void GameInstructor_FireEvent(const char *name, int entityIndex, const char *className,
	const char *targetName, const char *stringValue, int integerValue, float floatValue)
{
	InstructorEventData event;
	memset(&event, 0, sizeof(event));
	if (name)
		strncpy(event.name, name, sizeof(event.name) - 1);
	event.userId = 1;
	event.entityIndex = entityIndex;
	if (className)
		strncpy(event.className, className, sizeof(event.className) - 1);
	if (targetName)
		strncpy(event.targetName, targetName, sizeof(event.targetName) - 1);
	if (stringValue)
		strncpy(event.stringValue, stringValue, sizeof(event.stringValue) - 1);
	event.integerValue = integerValue;
	event.floatValue = floatValue;
	GameInstructor_FireEvent(event);
}

void GameInstructor_MarkDisplayed(const char *name)
{
	GetGameInstructor().MarkDisplayed(name);
}

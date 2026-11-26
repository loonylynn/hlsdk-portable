/*
 * Native weapon behaviour defaults and stable-key registry.
 */
#include "weapon_behavior.h"

#include <string.h>

namespace
{
	bool IsValidBehaviorId(const char *id)
	{
		const char *p;
		size_t length;

		if (!id)
			return false;

		length = strlen(id);
		if (length == 0 || length >= WeaponFramework::WeaponBehaviorNameCapacity)
			return false;

		for (p = id; *p; ++p)
		{
			const char c = *p;
			if (!((c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') || c == '_' || c == '-' ||
				c == '.' || c == '/'))
				return false;
		}

		return true;
	}

	void SetError(char *error, size_t capacity, const char *message)
	{
		size_t length;
		if (!error || capacity == 0)
			return;

		length = strlen(message);
		if (length >= capacity)
			length = capacity - 1;
		memcpy(error, message, length);
		error[length] = '\0';
	}
}

namespace WeaponFramework
{
	bool WeaponBehavior::ValidateDefinition(const WeaponDefinition &definition,
		char *error, size_t errorCapacity) const
	{
		const char *definitionBehavior = definition.GetBehaviorName();
		const char *behaviorId = GetBehaviorId();

		if (!definition.IsValid() || !definitionBehavior || !behaviorId ||
			strcmp(definitionBehavior, behaviorId) != 0)
		{
			SetError(error, errorCapacity, "weapon definition does not match native behavior");
			return false;
		}

		if (error && errorCapacity > 0)
			error[0] = '\0';
		return true;
	}

	bool WeaponBehavior::CanDeploy(const WeaponBehaviorContext &context) const
	{
		return context.IsValid();
	}

	void WeaponBehavior::OnDeploy(WeaponBehaviorContext &) const {}
	void WeaponBehavior::OnHolster(WeaponBehaviorContext &) const {}
	void WeaponBehavior::ItemPreFrame(WeaponBehaviorContext &) const {}
	void WeaponBehavior::ItemPostFrame(WeaponBehaviorContext &) const {}
	void WeaponBehavior::ItemBusyFrame(WeaponBehaviorContext &) const {}
	bool WeaponBehavior::PrimaryAttack(WeaponBehaviorContext &) const { return false; }
	bool WeaponBehavior::SecondaryAttack(WeaponBehaviorContext &) const { return false; }
	bool WeaponBehavior::BeginReload(WeaponBehaviorContext &) const { return false; }
	void WeaponBehavior::UpdateReload(WeaponBehaviorContext &) const {}
	void WeaponBehavior::FinishReload(WeaponBehaviorContext &) const {}
	void WeaponBehavior::AbortReload(WeaponBehaviorContext &) const {}
	void WeaponBehavior::WeaponIdle(WeaponBehaviorContext &) const {}

	bool WeaponBehavior::SaveBehaviorState(const WeaponBehaviorContext &,
		IWeaponStateWriter &) const
	{
		return true;
	}

	bool WeaponBehavior::RestoreBehaviorState(WeaponBehaviorContext &,
		const IWeaponStateReader &) const
	{
		return true;
	}

	WeaponBehaviorRegistry::WeaponBehaviorRegistry()
		: m_count(0)
	{
		Clear();
	}

	bool WeaponBehaviorRegistry::Register(const WeaponBehavior *behavior)
	{
		const char *id;
		int i;

		if (!behavior)
			return false;

		id = behavior->GetBehaviorId();
		if (!IsValidBehaviorId(id) || m_count >= MaxRegisteredWeaponBehaviors)
			return false;

		for (i = 0; i < m_count; ++i)
		{
			if (strcmp(m_behaviors[i]->GetBehaviorId(), id) == 0)
				return false;
		}

		m_behaviors[m_count++] = behavior;
		return true;
	}

	const WeaponBehavior *WeaponBehaviorRegistry::Find(const char *behaviorId) const
	{
		int i;
		if (!behaviorId || behaviorId[0] == '\0')
			return NULL;

		for (i = 0; i < m_count; ++i)
			if (strcmp(m_behaviors[i]->GetBehaviorId(), behaviorId) == 0)
				return m_behaviors[i];

		return NULL;
	}

	const WeaponBehavior *WeaponBehaviorRegistry::FindForDefinition(
		const WeaponDefinition &definition) const
	{
		return definition.IsValid() ?
			Find(definition.GetBehaviorName()) : NULL;
	}

	bool WeaponBehaviorRegistry::ValidateDefinition(const WeaponDefinition &definition,
		char *error, size_t errorCapacity) const
	{
		const WeaponBehavior *behavior = FindForDefinition(definition);

		if (!behavior)
		{
			SetError(error, errorCapacity, "weapon definition references an unregistered native behavior");
			return false;
		}

		return behavior->ValidateDefinition(definition, error, errorCapacity);
	}

	void WeaponBehaviorRegistry::Clear()
	{
		int i;
		for (i = 0; i < MaxRegisteredWeaponBehaviors; ++i)
			m_behaviors[i] = NULL;
		m_count = 0;
	}

	WeaponBehaviorRegistry &GetBehaviorRegistry()
	{
		static WeaponBehaviorRegistry registry;
		return registry;
	}
}

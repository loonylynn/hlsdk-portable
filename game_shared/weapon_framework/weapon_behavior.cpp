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

	class HitscanFirearmBehavior : public WeaponFramework::WeaponBehavior
	{
	public:
		const char *GetBehaviorId() const
		{
			return "firearm.hitscan";
		}

		bool ValidateDefinition(const WeaponFramework::WeaponDefinition &definition,
			char *error, size_t errorCapacity) const
		{
			if (!WeaponBehavior::ValidateDefinition(definition, error, errorCapacity))
				return false;

			if (definition.GetActionType() != WeaponFramework::WeaponActionHitscan)
			{
				SetError(error, errorCapacity, "firearm.hitscan requires a hitscan weapon definition");
				return false;
			}

			return true;
		}

		bool PrimaryAttack(WeaponFramework::WeaponBehaviorContext &context) const
		{
			using namespace WeaponFramework;
			WeaponHitscanRequest request;

			// Validate the adapter/context before reading instance or definition
			// pointers. Behaviours may be called by tests or future adapters that
			// have not constructed a complete context yet.
			if (!context.IsValid())
				return false;

			WeaponInstanceState &instance = *context.instance;
			const WeaponDefinition &definition = *context.definition;
			const bool wantsFire = definition.IsAutomatic() ?
				context.input.primaryDown : context.input.primaryPressed;

			if (instance.lifecycle != WeaponLifecycleReady || !wantsFire ||
				context.currentTime < instance.nextPrimaryAttackTime ||
				instance.loadedAmmo <= 0)
				return false;

			request.bulletType = definition.GetBulletType();
			request.damage = (float)definition.GetDamage();
			request.pellets = definition.GetPellets();
			request.range = (float)definition.GetRange();
			request.spread = definition.GetSpread();

			// Do not consume ammunition or advance timers when the engine adapter
			// rejects the shot. Prediction and authoritative simulation use the
			// same result contract.
			if (!context.host->FireHitscan(request))
				return false;

			--instance.loadedAmmo;
			instance.nextPrimaryAttackTime = context.currentTime + definition.GetFireRate();
			instance.nextIdleTime = context.currentTime + 1.0f;
			instance.actionStartTime = context.currentTime;
			instance.actionEndTime = instance.nextPrimaryAttackTime;
			instance.lifecycle = WeaponLifecyclePrimaryAction;
			++instance.actionSequence;

			context.host->SetOwnerAttackAnimation();
			context.host->PlayAnimation("fire");
			if (definition.GetFireSound())
				context.host->PlaySound(definition.GetFireSound());
			context.host->EmitEffect("muzzle_flash");
			context.host->NotifyAmmoChanged();
			return true;
		}
	};
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
		static HitscanFirearmBehavior hitscanBehavior;

		// Register built-ins lazily so the client and server each initialise
		// their own identical native behaviour registry on first use.
		if (!registry.Find(hitscanBehavior.GetBehaviorId()))
			registry.Register(&hitscanBehavior);

		return registry;
	}
}

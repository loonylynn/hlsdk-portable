/*
 * Native weapon behaviour contract.
 *
 * Behaviours are long-lived, stateless implementations registered by a
 * stable string key. Per-owned-weapon mutable data belongs in
 * WeaponInstanceState or in the behaviour's save archive, never in the
 * behaviour object itself.
 */
#pragma once
#ifndef WEAPON_FRAMEWORK_BEHAVIOR_H
#define WEAPON_FRAMEWORK_BEHAVIOR_H

#include <stddef.h>
#include "weapon_definition.h"
#include "weapon_runtime.h"

namespace WeaponFramework
{
	enum { MaxRegisteredWeaponBehaviors = 32 };

	// The behaviour asks the engine adapter to perform effects through this
	// narrow interface. Implementations may be backed by GoldSrc today and a
	// different engine-facing layer later.
	struct WeaponHitscanRequest
	{
		const char *damageProfile;
		float damage;
		int pellets;
		float range;
		float spread;

		WeaponHitscanRequest()
			: damageProfile(NULL), damage(0.0f), pellets(1), range(0.0f), spread(0.0f)
		{
		}
	};

	class IWeaponActionHost
	{
	public:
		virtual ~IWeaponActionHost() {}

		virtual int GetReserveAmmo(const char *ammoName) const = 0;
		virtual bool ConsumeReserveAmmo(const char *ammoName, int amount) = 0;
		virtual bool FireHitscan(const WeaponHitscanRequest &request) = 0;
		virtual void PlayAnimation(const char *activityName) = 0;
		virtual void PlaySound(const char *soundEvent) = 0;
		virtual void EmitEffect(const char *effectName) = 0;
		virtual void SetOwnerAttackAnimation() = 0;
		virtual bool IsOwnerUnderwater() const = 0;
		virtual void NotifyAmmoChanged() = 0;
	};

	struct WeaponBehaviorContext
	{
		const WeaponDefinition *definition;
		WeaponInstanceState *instance;
		IWeaponActionHost *host;
		WeaponInputState input;
		float currentTime;
		float frameTime;
		bool authoritative;
		bool predicting;

		WeaponBehaviorContext()
			: definition(NULL), instance(NULL), host(NULL), currentTime(0.0f),
			  frameTime(0.0f), authoritative(false), predicting(false)
		{
		}

		bool IsValid() const
		{
			return definition != NULL && definition->IsValid() &&
				instance != NULL && host != NULL;
		}
	};

	// Keyed archive interfaces let a behaviour persist custom state without
	// writing raw structs, pointers, or compiler-specific binary layouts.
	class IWeaponStateWriter
	{
	public:
		virtual ~IWeaponStateWriter() {}
		virtual bool WriteInt(const char *key, int value) = 0;
		virtual bool WriteFloat(const char *key, float value) = 0;
		virtual bool WriteBool(const char *key, bool value) = 0;
		virtual bool WriteString(const char *key, const char *value) = 0;
	};

	class IWeaponStateReader
	{
	public:
		virtual ~IWeaponStateReader() {}
		virtual bool ReadInt(const char *key, int &value) const = 0;
		virtual bool ReadFloat(const char *key, float &value) const = 0;
		virtual bool ReadBool(const char *key, bool &value) const = 0;
		virtual bool ReadString(const char *key, char *value, size_t capacity) const = 0;
	};

	class WeaponBehavior
	{
	public:
		virtual ~WeaponBehavior() {}

		// Stable identifier referenced by WeaponDefinition. This is not a
		// GoldSrc classname and does not identify one specific weapon.
		virtual const char *GetBehaviorId() const = 0;

		virtual bool ValidateDefinition(const WeaponDefinition &definition,
			char *error, size_t errorCapacity) const;

		virtual bool CanDeploy(const WeaponBehaviorContext &context) const;
		virtual void OnDeploy(WeaponBehaviorContext &context) const;
		virtual void OnHolster(WeaponBehaviorContext &context) const;
		virtual void ItemPreFrame(WeaponBehaviorContext &context) const;
		virtual void ItemPostFrame(WeaponBehaviorContext &context) const;
		virtual void ItemBusyFrame(WeaponBehaviorContext &context) const;
		virtual bool PrimaryAttack(WeaponBehaviorContext &context) const;
		virtual bool SecondaryAttack(WeaponBehaviorContext &context) const;
		virtual bool BeginReload(WeaponBehaviorContext &context) const;
		virtual void UpdateReload(WeaponBehaviorContext &context) const;
		virtual void FinishReload(WeaponBehaviorContext &context) const;
		virtual void AbortReload(WeaponBehaviorContext &context) const;
		virtual void WeaponIdle(WeaponBehaviorContext &context) const;

		virtual bool SaveBehaviorState(const WeaponBehaviorContext &context,
			IWeaponStateWriter &writer) const;
		virtual bool RestoreBehaviorState(WeaponBehaviorContext &context,
			const IWeaponStateReader &reader) const;
	};

	// Stores non-owning pointers to static/native behaviour objects. Register
	// them during DLL initialization, before definitions are validated.
	class WeaponBehaviorRegistry
	{
	public:
		WeaponBehaviorRegistry();

		bool Register(const WeaponBehavior *behavior);
		const WeaponBehavior *Find(const char *behaviorId) const;
		const WeaponBehavior *FindForDefinition(const WeaponDefinition &definition) const;
		bool ValidateDefinition(const WeaponDefinition &definition,
			char *error, size_t errorCapacity) const;
		void Clear();

	private:
		const WeaponBehavior *m_behaviors[MaxRegisteredWeaponBehaviors];
		int m_count;
	};

	WeaponBehaviorRegistry &GetBehaviorRegistry();
}

#endif // WEAPON_FRAMEWORK_BEHAVIOR_H

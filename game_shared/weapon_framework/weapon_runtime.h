/*
 * Engine-independent per-instance weapon runtime state and input snapshot.
 *
 * This is a shared contract. Engine entity pointers, GoldSrc numeric IDs and
 * save-game handles must stay in the compatibility adapter.
 */
#pragma once
#ifndef WEAPON_FRAMEWORK_RUNTIME_H
#define WEAPON_FRAMEWORK_RUNTIME_H

namespace WeaponFramework
{
	enum WeaponLifecycleState
	{
		WeaponLifecycleUnowned = 0,
		WeaponLifecycleHolstered,
		WeaponLifecycleDeploying,
		WeaponLifecycleReady,
		WeaponLifecyclePrimaryAction,
		WeaponLifecycleSecondaryAction,
		WeaponLifecycleReloading,
		WeaponLifecycleBusy
	};

	enum WeaponReloadState
	{
		WeaponReloadNone = 0,
		WeaponReloadStarting,
		WeaponReloadInProgress,
		WeaponReloadFinishing,
		WeaponReloadInterrupted
	};

	// A snapshot of input for one simulation frame. Pressed and down are
	// separate so semi-automatic and automatic behaviours can share an API.
	struct WeaponInputState
	{
		bool primaryDown;
		bool primaryPressed;
		bool primaryReleased;
		bool secondaryDown;
		bool secondaryPressed;
		bool secondaryReleased;
		bool reloadPressed;
		bool holsterPressed;

		WeaponInputState()
			: primaryDown(false), primaryPressed(false), primaryReleased(false),
			  secondaryDown(false), secondaryPressed(false), secondaryReleased(false),
			  reloadPressed(false), holsterPressed(false)
		{
		}
	};

	// Mutable state for one owned weapon. It contains no engine pointers and
	// never aliases the immutable WeaponDefinition.
	struct WeaponInstanceState
	{
		enum { CurrentVersion = 1 };

		int version;
		WeaponLifecycleState lifecycle;
		WeaponReloadState reload;
		// -1 means this weapon has no conventional loaded clip.
		int loadedAmmo;
		float nextPrimaryAttackTime;
		float nextSecondaryAttackTime;
		float nextIdleTime;
		float actionStartTime;
		float actionEndTime;
		unsigned int actionSequence;

		WeaponInstanceState()
			: version(CurrentVersion),
			  lifecycle(WeaponLifecycleUnowned),
			  reload(WeaponReloadNone),
			  loadedAmmo(0),
			  nextPrimaryAttackTime(0.0f),
			  nextSecondaryAttackTime(0.0f),
			  nextIdleTime(0.0f),
			  actionStartTime(0.0f),
			  actionEndTime(0.0f),
			  actionSequence(0)
		{
		}

		void Reset(int initialLoadedAmmo)
		{
			version = CurrentVersion;
			lifecycle = WeaponLifecycleHolstered;
			reload = WeaponReloadNone;
			loadedAmmo = initialLoadedAmmo;
			nextPrimaryAttackTime = 0.0f;
			nextSecondaryAttackTime = 0.0f;
			nextIdleTime = 0.0f;
			actionStartTime = 0.0f;
			actionEndTime = 0.0f;
			actionSequence = 0;
		}

		// Save data is versioned and validated before an adapter trusts it.
		// Bounded comparisons also reject NaN and infinite time values.
		bool IsValid() const
		{
			const float timeLimit = 100000000.0f;
			return version == CurrentVersion &&
				lifecycle >= WeaponLifecycleUnowned &&
				lifecycle <= WeaponLifecycleBusy &&
				reload >= WeaponReloadNone &&
				reload <= WeaponReloadInterrupted &&
				loadedAmmo >= -1 &&
				nextPrimaryAttackTime >= -timeLimit &&
				nextPrimaryAttackTime <= timeLimit &&
				nextSecondaryAttackTime >= -timeLimit &&
				nextSecondaryAttackTime <= timeLimit &&
				nextIdleTime >= -timeLimit &&
				nextIdleTime <= timeLimit &&
				actionStartTime >= -timeLimit &&
				actionStartTime <= timeLimit &&
				actionEndTime >= -timeLimit &&
				actionEndTime <= timeLimit;
		}
	};
}

#endif // WEAPON_FRAMEWORK_RUNTIME_H

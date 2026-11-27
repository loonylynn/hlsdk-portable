/*
 * Focused tests for the shared weapon JSON and package registry.
 *
 * These tests exercise metadata loading without starting the GoldSrc engine.
 */
#include "weapon_json.h"
#include "weapon_behavior.h"
#include "weapon_registry.h"

#include <new>
#include <stdio.h>
#include <string.h>

using namespace WeaponFramework;

namespace
{
	class TestActionHost : public IWeaponActionHost
	{
	public:
		TestActionHost()
			: fireSucceeds(true), fireCalls(0), animationCalls(0), soundCalls(0),
			  effectCalls(0), attackAnimationCalls(0), ammoChangedCalls(0)
		{
		}

		int GetReserveAmmo(const char *) const { return 0; }
		bool ConsumeReserveAmmo(const char *, int) { return false; }
		bool FireHitscan(const WeaponHitscanRequest &request)
		{
			lastRequest = request;
			++fireCalls;
			return fireSucceeds;
		}
		void PlayAnimation(const char *) { ++animationCalls; }
		void PlaySound(const char *) { ++soundCalls; }
		void EmitEffect(const char *) { ++effectCalls; }
		void SetOwnerAttackAnimation() { ++attackAnimationCalls; }
		bool IsOwnerUnderwater() const { return false; }
		void NotifyAmmoChanged() { ++ammoChangedCalls; }

		bool fireSucceeds;
		int fireCalls;
		int animationCalls;
		int soundCalls;
		int effectCalls;
		int attackAnimationCalls;
		int ammoChangedCalls;
		WeaponHitscanRequest lastRequest;
	};

	class TestHitscanBehavior : public WeaponBehavior
	{
	public:
		const char *GetBehaviorId() const { return "firearm.hitscan"; }
	};

	int g_failures = 0;

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			++g_failures; \
		} \
	} while (0)

	const char kAlphaDefinition[] =
		"{"
		"\"format_version\":1,"
		"\"identity\":{\"name\":\"sample:weapon/alpha\",\"entity_classname\":\"weapon_test_alpha\"},"
		"\"inventory\":{\"slot\":1,\"position\":0,\"weight\":10,\"flags\":[\"select_on_empty\"]},"
		"\"ammo\":{\"clip_size\":12,\"default_ammo\":12,"
			"\"primary\":{\"type\":\"alpha_rounds\",\"max_carry\":120,\"pickup_amount\":12},"
			"\"secondary\":null},"
		"\"models\":{\"view\":\"models/v_alpha.mdl\",\"world\":\"models/w_alpha.mdl\",\"player\":null}"
		"}";



	const char kTripMineLegacyDefinition[] =
		"{"
		"\"format_version\":1,"
		"\"identity\":{\"name\":\"goldsrc:weapon/tripmine\",\"entity_classname\":\"weapon_tripmine\",\"legacy_id\":13},"
		"\"inventory\":{\"slot\":4,\"position\":2,\"weight\":-10,\"flags\":[\"limit_in_world\",\"exhaustible\"]},"
		"\"ammo\":{\"clip_size\":-1,\"default_ammo\":1,"
			"\"primary\":{\"type\":\"Trip Mine\",\"max_carry\":5,\"pickup_amount\":1},"
			"\"secondary\":null},"
		"\"models\":{\"view\":\"models/v_tripmine.mdl\",\"world\":\"models/v_tripmine.mdl\",\"player\":\"models/p_tripmine.mdl\"}"
		"}";

	const char kHitscanDefinition[] =
		"{"
		"\"format_version\":1,"
		"\"identity\":{\"name\":\"sample:weapon/hitscan\",\"entity_classname\":\"weapon_test_hitscan\"},"
		"\"inventory\":{\"slot\":1,\"position\":2,\"weight\":9,\"flags\":[]},"
		"\"ammo\":{\"clip_size\":12,\"default_ammo\":36,"
			"\"primary\":{\"type\":\"9mm\",\"max_carry\":250,\"pickup_amount\":12},"
			"\"secondary\":null},"
		"\"models\":{\"view\":\"models/v_test.mdl\",\"world\":\"models/w_test.mdl\",\"player\":null},"
		"\"behavior\":{\"type\":\"hitscan\",\"bullet_type\":\"9mm\",\"damage\":20,"
			"\"pellets\":1,\"range\":4096,\"spread\":0.02,\"fire_rate\":0.25,"
			"\"reload_time\":1.2,\"automatic\":false,\"fire_sound\":\"weapons/test_fire.wav\","
			"\"animations\":{\"draw\":0,\"fire\":1,\"reload\":2,\"idle\":0}}"
		"}";

	const char kUnknownFieldDefinition[] =
		"{"
		"\"format_version\":1,"
		"\"identity\":{\"name\":\"sample:weapon/alpha\",\"entity_classname\":\"weapon_test_alpha\"},"
		"\"inventory\":{\"slot\":1,\"position\":0,\"weight\":10,\"flags\":[]},"
		"\"ammo\":{\"clip_size\":12,\"default_ammo\":12,\"primary\":null,\"secondary\":null},"
		"\"models\":{\"view\":\"models/v_alpha.mdl\",\"world\":\"models/w_alpha.mdl\",\"player\":null},"
		"\"debug\":true"
		"}";

	const char kWrongTypeDefinition[] =
		"{"
		"\"format_version\":1,"
		"\"identity\":{\"name\":\"sample:weapon/alpha\",\"entity_classname\":\"weapon_test_alpha\"},"
		"\"inventory\":{\"slot\":\"1\",\"position\":0,\"weight\":10,\"flags\":[]},"
		"\"ammo\":{\"clip_size\":12,\"default_ammo\":12,\"primary\":null,\"secondary\":null},"
		"\"models\":{\"view\":\"models/v_alpha.mdl\",\"world\":\"models/w_alpha.mdl\",\"player\":null}"
		"}";

	const char kTraversalModelDefinition[] =
		"{"
		"\"format_version\":1,"
		"\"identity\":{\"name\":\"sample:weapon/alpha\",\"entity_classname\":\"weapon_test_alpha\"},"
		"\"inventory\":{\"slot\":1,\"position\":0,\"weight\":10,\"flags\":[]},"
		"\"ammo\":{\"clip_size\":12,\"default_ammo\":12,\"primary\":null,\"secondary\":null},"
		"\"models\":{\"view\":\"models/../v_alpha.mdl\",\"world\":\"models/w_alpha.mdl\",\"player\":null}"
		"}";

	const char kEchoDefinition[] =
		"{"
		"\"format_version\":1,"
		"\"identity\":{\"name\":\"sample:weapon/echo\",\"entity_classname\":\"weapon_test_echo\"},"
		"\"inventory\":{\"slot\":1,\"position\":1,\"weight\":8,\"flags\":[]},"
		"\"ammo\":{\"clip_size\":8,\"default_ammo\":8,"
			"\"primary\":{\"type\":\"echo_rounds\",\"max_carry\":80,\"pickup_amount\":8},"
			"\"secondary\":null},"
		"\"models\":{\"view\":\"models/v_echo.mdl\",\"world\":\"models/w_echo.mdl\",\"player\":null}"
		"}";

	const char kZuluDefinition[] =
		"{"
		"\"format_version\":1,"
		"\"identity\":{\"name\":\"sample:weapon/zulu\",\"entity_classname\":\"weapon_test_zulu\"},"
		"\"inventory\":{\"slot\":1,\"position\":2,\"weight\":6,\"flags\":[]},"
		"\"ammo\":{\"clip_size\":6,\"default_ammo\":6,"
			"\"primary\":{\"type\":\"zulu_rounds\",\"max_carry\":60,\"pickup_amount\":6},"
			"\"secondary\":null},"
		"\"models\":{\"view\":\"models/v_zulu.mdl\",\"world\":\"models/w_zulu.mdl\",\"player\":null}"
		"}";

	const char kAlphaManifest[] =
		"{"
		"\"format_version\":1,"
		"\"name\":\"sample:pack/alpha\","
		"\"weapons\":["
			"{\"name\":\"sample:weapon/alpha\",\"file\":\"addons/alpha/weapons/alpha.json\"}"
		"]"
		"}";

	const char kZetaManifestForward[] =
		"{"
		"\"format_version\":1,"
		"\"name\":\"sample:pack/zeta\","
		"\"weapons\":["
			"{\"name\":\"sample:weapon/zulu\",\"file\":\"addons/zeta/weapons/zulu.json\"},"
			"{\"name\":\"sample:weapon/echo\",\"file\":\"addons/zeta/weapons/echo.json\"}"
		"]"
		"}";

	const char kZetaManifestReverse[] =
		"{"
		"\"format_version\":1,"
		"\"name\":\"sample:pack/zeta\","
		"\"weapons\":["
			"{\"name\":\"sample:weapon/echo\",\"file\":\"addons/zeta/weapons/echo.json\"},"
			"{\"name\":\"sample:weapon/zulu\",\"file\":\"addons/zeta/weapons/zulu.json\"}"
		"]"
		"}";

	const char kCatalogZetaFirst[] =
		"{"
		"\"format_version\":1,"
		"\"packages\":["
			"{\"name\":\"sample:pack/zeta\",\"manifest\":\"addons/zeta/package.json\"},"
			"{\"name\":\"sample:pack/alpha\",\"manifest\":\"addons/alpha/package.json\"}"
		"]"
		"}";

	const char kCatalogAlphaFirst[] =
		"{"
		"\"format_version\":1,"
		"\"packages\":["
			"{\"name\":\"sample:pack/alpha\",\"manifest\":\"addons/alpha/package.json\"},"
			"{\"name\":\"sample:pack/zeta\",\"manifest\":\"addons/zeta/package.json\"}"
		"]"
		"}";

	const char kCatalogMissingWeapon[] =
		"{"
		"\"format_version\":1,"
		"\"packages\":["
			"{\"name\":\"sample:pack/alpha\",\"manifest\":\"addons/alpha/missing_test.json\"}"
		"]"
		"}";

	const char kManifestWithMissingWeapon[] =
		"{"
		"\"format_version\":1,"
		"\"name\":\"sample:pack/alpha\","
		"\"weapons\":["
			"{\"name\":\"sample:weapon/alpha\",\"file\":\"addons/alpha/weapons/alpha.json\"},"
			"{\"name\":\"sample:weapon/missing\",\"file\":\"addons/alpha/weapons/missing.json\"}"
		"]"
		"}";

	struct MemoryFile
	{
		const char *path;
		const char *contents;
	};

	const MemoryFile kFilesZetaFirst[] =
	{
		{ "scripts/weapons/packages.json", kCatalogZetaFirst },
		{ "addons/zeta/package.json", kZetaManifestForward },
		{ "addons/alpha/package.json", kAlphaManifest },
		{ "addons/zeta/weapons/zulu.json", kZuluDefinition },
		{ "addons/zeta/weapons/echo.json", kEchoDefinition },
		{ "addons/alpha/weapons/alpha.json", kAlphaDefinition }
	};

	const MemoryFile kFilesAlphaFirst[] =
	{
		{ "scripts/weapons/packages.json", kCatalogAlphaFirst },
		{ "addons/alpha/package.json", kAlphaManifest },
		{ "addons/zeta/package.json", kZetaManifestReverse },
		{ "addons/alpha/weapons/alpha.json", kAlphaDefinition },
		{ "addons/zeta/weapons/echo.json", kEchoDefinition },
		{ "addons/zeta/weapons/zulu.json", kZuluDefinition }
	};

	const MemoryFile kFilesMissingWeapon[] =
	{
		{ "scripts/weapons/packages.json", kCatalogMissingWeapon },
		{ "addons/alpha/missing_test.json", kManifestWithMissingWeapon },
		{ "addons/alpha/weapons/alpha.json", kAlphaDefinition }
	};

	const MemoryFile *g_files = NULL;
	int g_fileCount = 0;
	int g_loadCount = 0;
	int g_freeCount = 0;

	void SetMemoryFiles(const MemoryFile *files, int fileCount)
	{
		g_files = files;
		g_fileCount = fileCount;
		g_loadCount = 0;
		g_freeCount = 0;
	}

	unsigned char *LoadMemoryFile(const char *path, int *length)
	{
		int i;
		if (!path || !length)
			return NULL;

		for (i = 0; i < g_fileCount; ++i)
		{
			if (strcmp(path, g_files[i].path) == 0)
			{
				const size_t size = strlen(g_files[i].contents);
				unsigned char *copy = new (std::nothrow) unsigned char[size];
				if (!copy)
					return NULL;
				memcpy(copy, g_files[i].contents, size);
				*length = (int)size;
				++g_loadCount;
				return copy;
			}
		}

		*length = 0;
		return NULL;
	}

	void FreeMemoryFile(unsigned char *fileData)
	{
		if (fileData)
		{
			++g_freeCount;
			delete[] fileData;
		}
	}

	void CheckExpectedAddonIds()
	{
		const WeaponDefinition *alpha = GetRegistry().FindByName("sample:weapon/alpha");
		const WeaponDefinition *echo = GetRegistry().FindByName("sample:weapon/echo");
		const WeaponDefinition *zulu = GetRegistry().FindByName("sample:weapon/zulu");

		CHECK(alpha != NULL);
		CHECK(echo != NULL);
		CHECK(zulu != NULL);
		if (alpha && echo && zulu)
		{
			CHECK(alpha->GetId() == FirstAddonWeaponId);
			CHECK(echo->GetId() == FirstAddonWeaponId + 1);
			CHECK(zulu->GetId() == FirstAddonWeaponId + 2);
		}
	}

	void TestDefinitionParsing()
	{
		WeaponDefinition definition;

		CHECK(ParseWeaponDefinitionJson(kAlphaDefinition, strlen(kAlphaDefinition), definition));
		CHECK(definition.IsValid());
		CHECK(definition.GetId() == 0);
		CHECK(strcmp(definition.GetName(), "sample:weapon/alpha") == 0);
		CHECK(strcmp(definition.GetEntityClassname(), "weapon_test_alpha") == 0);
		CHECK(definition.GetSlot() == 1);
		CHECK(definition.GetPosition() == 0);
		CHECK(definition.GetClipSize() == 12);
		CHECK(definition.GetDefaultAmmo() == 12);
		CHECK(definition.GetPrimaryAmmoType() != NULL);
		if (definition.GetPrimaryAmmoType())
			CHECK(strcmp(definition.GetPrimaryAmmoType(), "alpha_rounds") == 0);
		CHECK(definition.GetPrimaryAmmoMax() == 120);
		CHECK(definition.GetPrimaryAmmoPickupAmount() == 12);
		CHECK(definition.GetViewModel() != NULL);
		CHECK(definition.GetWorldModel() != NULL);
		CHECK(definition.GetPlayerModel() == NULL);

		CHECK(!ParseWeaponDefinitionJson(kUnknownFieldDefinition,
			strlen(kUnknownFieldDefinition), definition));
		CHECK(!ParseWeaponDefinitionJson(kWrongTypeDefinition,
			strlen(kWrongTypeDefinition), definition));
		CHECK(!ParseWeaponDefinitionJson(kTraversalModelDefinition,
			strlen(kTraversalModelDefinition), definition));
		CHECK(!ParseWeaponDefinitionJson("{", 1, definition));
	}



	void TestWeaponInstanceStateValidation()
	{
		WeaponInstanceState state;
		CHECK(state.IsValid());

		state.Reset(17);
		CHECK(state.IsValid());
		CHECK(state.version == WeaponInstanceState::CurrentVersion);
		CHECK(state.lifecycle == WeaponLifecycleHolstered);
		CHECK(state.reload == WeaponReloadNone);
		CHECK(state.loadedAmmo == 17);

		state.lifecycle = WeaponLifecycleReady;
		state.reload = WeaponReloadInProgress;
		state.loadedAmmo = 9;
		state.nextPrimaryAttackTime = 12.5f;
		state.actionStartTime = 12.0f;
		state.actionEndTime = 12.75f;
		state.actionSequence = 42;

		// The same state copy used by an entity save/restore adapter must
		// preserve the values and remain valid.
		WeaponInstanceState restored = state;
		CHECK(restored.IsValid());
		CHECK(restored.lifecycle == WeaponLifecycleReady);
		CHECK(restored.reload == WeaponReloadInProgress);
		CHECK(restored.loadedAmmo == 9);
		CHECK(restored.nextPrimaryAttackTime == 12.5f);
		CHECK(restored.actionSequence == 42);

		restored.version = 0;
		CHECK(!restored.IsValid());
		restored.Reset(5);
		restored.lifecycle = (WeaponLifecycleState)99;
		CHECK(!restored.IsValid());
		restored.Reset(5);
		restored.reload = (WeaponReloadState)99;
		CHECK(!restored.IsValid());
		restored.Reset(5);
		restored.loadedAmmo = -2;
		CHECK(!restored.IsValid());
		restored.Reset(5);
		restored.nextPrimaryAttackTime = 100000001.0f;
		CHECK(!restored.IsValid());
	}

	void TestLegacyMetadataWithSpacedAmmoNames()
	{
		WeaponDefinition definition;
		CHECK(ParseWeaponDefinitionJson(kTripMineLegacyDefinition,
			strlen(kTripMineLegacyDefinition), definition));
		CHECK(definition.IsValid());
		CHECK(definition.GetId() == 13);
		CHECK(definition.GetPrimaryAmmoType() != NULL);
		if (definition.GetPrimaryAmmoType())
			CHECK(strcmp(definition.GetPrimaryAmmoType(), "Trip Mine") == 0);
		CHECK(definition.GetFlags() == (8 | 16));
		CHECK(definition.GetActionType() == WeaponActionNone);
	}

	void TestHitscanBehaviorParsing()
	{
		WeaponDefinition definition;
		CHECK(ParseWeaponDefinitionJson(kHitscanDefinition, strlen(kHitscanDefinition), definition));
		CHECK(definition.IsValid());
		CHECK(definition.GetId() == 0);
		CHECK(definition.GetActionType() == WeaponActionHitscan);
		CHECK(definition.GetBehaviorName() != NULL);
		if (definition.GetBehaviorName())
			CHECK(strcmp(definition.GetBehaviorName(), "firearm.hitscan") == 0);
		CHECK(definition.GetBulletType() == 1);
		CHECK(definition.GetDamage() == 20);
		CHECK(definition.GetPellets() == 1);
		CHECK(definition.GetRange() == 4096);
		CHECK(definition.GetSpread() > 0.019f && definition.GetSpread() < 0.021f);
		CHECK(definition.GetFireRate() > 0.24f && definition.GetFireRate() < 0.26f);
		CHECK(definition.GetReloadTime() > 1.19f && definition.GetReloadTime() < 1.21f);
		CHECK(!definition.IsAutomatic());
		CHECK(definition.GetFireSound() != NULL);
		CHECK(definition.GetDrawAnimation() == 0);
		CHECK(definition.GetFireAnimation() == 1);
		CHECK(definition.GetReloadAnimation() == 2);
		CHECK(definition.GetIdleAnimation() == 0);
	}

	void TestNativeBehaviorContracts()
	{
		WeaponDefinition definition;
		WeaponDefinition metadataOnly;
		WeaponBehaviorRegistry registry;
		const WeaponBehavior *hitscan = GetBehaviorRegistry().Find("firearm.hitscan");
		TestHitscanBehavior duplicate;
		TestActionHost host;
		WeaponInstanceState instance;
		WeaponBehaviorContext context;
		WeaponBehaviorContext invalidContext;
		WeaponInputState input;
		char error[128] = { 0 };

		CHECK(ParseWeaponDefinitionJson(kHitscanDefinition,
			strlen(kHitscanDefinition), definition));
		CHECK(ParseWeaponDefinitionJson(kAlphaDefinition,
			strlen(kAlphaDefinition), metadataOnly));

		CHECK(hitscan != NULL);
		if (hitscan)
		{
			CHECK(!hitscan->PrimaryAttack(invalidContext));
			CHECK(registry.Register(hitscan));
			CHECK(registry.Find("firearm.hitscan") == hitscan);
			CHECK(registry.FindForDefinition(definition) == hitscan);
		}
		CHECK(registry.Find("missing.behavior") == NULL);
		CHECK(!registry.Register(&duplicate));
		CHECK(registry.ValidateDefinition(definition, error, sizeof(error)));
		CHECK(error[0] == '\0');
		CHECK(!registry.ValidateDefinition(metadataOnly, error, sizeof(error)));
		CHECK(error[0] != '\0');

		instance.Reset(17);
		CHECK(instance.version == WeaponInstanceState::CurrentVersion);
		CHECK(instance.lifecycle == WeaponLifecycleHolstered);
		CHECK(instance.reload == WeaponReloadNone);
		CHECK(instance.loadedAmmo == 17);
		CHECK(instance.nextPrimaryAttackTime == 0.0f);

		context.definition = &definition;
		context.instance = &instance;
		context.host = &host;
		context.input = input;
		context.currentTime = 2.0f;
		context.frameTime = 0.016f;
		context.authoritative = true;
		instance.lifecycle = WeaponLifecycleReady;
		context.input.primaryDown = true;
		context.input.primaryPressed = true;
		CHECK(context.IsValid());
		CHECK(hitscan != NULL && hitscan->CanDeploy(context));
		CHECK(hitscan != NULL && hitscan->PrimaryAttack(context));
		CHECK(instance.loadedAmmo == 16);
		CHECK(instance.nextPrimaryAttackTime > 2.24f && instance.nextPrimaryAttackTime < 2.26f);
		CHECK(instance.actionSequence == 1);
		CHECK(host.fireCalls == 1);
		CHECK(host.lastRequest.bulletType == definition.GetBulletType());
		CHECK(host.lastRequest.damage == (float)definition.GetDamage());
		CHECK(host.animationCalls == 1);
		CHECK(host.soundCalls == 1);
		CHECK(host.effectCalls == 1);
		CHECK(host.attackAnimationCalls == 1);
		CHECK(host.ammoChangedCalls == 1);

		// A held trigger must not repeat a semi-automatic shot without a
		// fresh pressed edge after the cooldown has elapsed.
		instance.lifecycle = WeaponLifecycleReady;
		context.currentTime = 2.5f;
		context.input.primaryPressed = false;
		CHECK(hitscan != NULL && !hitscan->PrimaryAttack(context));
		CHECK(instance.loadedAmmo == 16);
		CHECK(host.fireCalls == 1);

		// A rejected engine shot must not consume ammo or advance the timer.
		instance.lifecycle = WeaponLifecycleReady;
		context.input.primaryPressed = true;
		host.fireSucceeds = false;
		const float previousAttackTime = instance.nextPrimaryAttackTime;
		CHECK(hitscan != NULL && !hitscan->PrimaryAttack(context));
		CHECK(instance.loadedAmmo == 16);
		CHECK(instance.nextPrimaryAttackTime == previousAttackTime);
		CHECK(host.fireCalls == 2);

		registry.Clear();
		CHECK(registry.Find("firearm.hitscan") == NULL);
	}

	void TestRegistryRejectsDuplicates()
	{
		WeaponRegistry registry;
		WeaponDefinition alpha;
		WeaponDefinitionData duplicate = {};

		CHECK(ParseWeaponDefinitionJson(kAlphaDefinition, strlen(kAlphaDefinition), alpha));
		CHECK(registry.RegisterAddon(alpha) == FirstAddonWeaponId);

		duplicate.id = 0;
		duplicate.name = "sample:weapon/alpha";
		duplicate.entityClassname = "weapon_test_duplicate";
		duplicate.slot = 1;
		duplicate.position = 0;
		duplicate.weight = 1;
		duplicate.flags = 0;
		duplicate.clipSize = -1;
		duplicate.defaultAmmo = 0;
		duplicate.primaryAmmoType = NULL;
		duplicate.primaryAmmoMax = -1;
		duplicate.primaryAmmoPickupAmount = 0;
		duplicate.secondaryAmmoType = NULL;
		duplicate.secondaryAmmoMax = -1;
		duplicate.viewModel = "models/v_duplicate.mdl";
		duplicate.worldModel = "models/w_duplicate.mdl";
		duplicate.playerModel = NULL;

		CHECK(WeaponDefinition(duplicate).IsValid());
		CHECK(registry.RegisterAddon(WeaponDefinition(duplicate)) == 0);

		duplicate.name = "sample:weapon/other";
		duplicate.entityClassname = "weapon_test_alpha";
		CHECK(registry.RegisterAddon(WeaponDefinition(duplicate)) == 0);
	}

	void TestDeterministicPackageLoading()
	{
		SetMemoryFiles(kFilesZetaFirst, (int)(sizeof(kFilesZetaFirst) / sizeof(kFilesZetaFirst[0])));
		CHECK(LoadWeaponPackageCatalog("scripts/weapons/packages.json",
			LoadMemoryFile, FreeMemoryFile));
		CHECK(g_loadCount == g_freeCount);
		CheckExpectedAddonIds();

		// Reversing both package and weapon arrays must not change assigned IDs.
		SetMemoryFiles(kFilesAlphaFirst, (int)(sizeof(kFilesAlphaFirst) / sizeof(kFilesAlphaFirst[0])));
		CHECK(LoadWeaponPackageCatalog("scripts/weapons/packages.json",
			LoadMemoryFile, FreeMemoryFile));
		CHECK(g_loadCount == g_freeCount);
		CheckExpectedAddonIds();
	}

	void TestFailedLoadDoesNotPublishPartialRegistry()
	{
		SetMemoryFiles(kFilesMissingWeapon,
			(int)(sizeof(kFilesMissingWeapon) / sizeof(kFilesMissingWeapon[0])));
		CHECK(!LoadWeaponPackageCatalog("scripts/weapons/packages.json",
			LoadMemoryFile, FreeMemoryFile));
		CHECK(g_loadCount == g_freeCount);

		// The last valid catalogue remains active; the staged alpha entry is not
		// published when the following weapon file cannot be found.
		CheckExpectedAddonIds();
		CHECK(GetRegistry().FindByName("sample:weapon/missing") == NULL);

		CHECK(!LoadWeaponPackageCatalog("../scripts/weapons/packages.json",
			LoadMemoryFile, FreeMemoryFile));
		CheckExpectedAddonIds();
	}
}

int main()
{
	TestDefinitionParsing();
	TestWeaponInstanceStateValidation();
	TestLegacyMetadataWithSpacedAmmoNames();
	TestHitscanBehaviorParsing();
	TestNativeBehaviorContracts();
	TestRegistryRejectsDuplicates();
	TestDeterministicPackageLoading();
	TestFailedLoadDoesNotPublishPartialRegistry();

	if (g_failures != 0)
	{
		fprintf(stderr, "%d weapon framework test(s) failed.\n", g_failures);
		return 1;
	}

	printf("Weapon framework tests passed.\n");
	return 0;
}

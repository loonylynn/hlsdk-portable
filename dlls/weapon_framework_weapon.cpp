/*
 * One native engine classname adapts package definitions to GoldSrc's
 * existing player-weapon inventory and prediction contract.
 */
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "weapon_framework_weapon.h"
#include <string.h>

#ifndef CLIENT_DLL
LINK_ENTITY_TO_CLASS(weapon_framework, CFrameworkWeapon)
#endif

CFrameworkWeapon::CFrameworkWeapon()
{
	// CBasePlayerWeapon is a legacy POD-style entity. Initialize every field
	// the generic adapter reads instead of depending on engine allocator state.
	m_pPlayer = NULL;
	m_pNext = NULL;
	m_iId = 0;

	m_iPlayEmptySound = 0;
	m_fFireOnEmpty = FALSE;
	m_flPumpTime = 0.0f;
	m_fInSpecialReload = FALSE;
	m_flNextPrimaryAttack = 0.0f;
	m_flNextSecondaryAttack = 0.0f;
	m_flTimeWeaponIdle = 0.0f;
	m_iPrimaryAmmoType = 0;
	m_iSecondaryAmmoType = 0;
	m_iClip = 0;
	m_iClientClip = 0;
	m_iClientWeaponState = 0;
	m_fInReload = FALSE;
	m_iDefaultAmmo = 0;
	m_flPrevPrimaryAttack = 0.0f;
	m_flLastFireTime = 0.0f;

	m_definitionName[0] = '\0';
}
void CFrameworkWeapon::SetDefinitionId(int id)
{
	m_iId = id;
	m_definitionName[0] = '\0';
}
const WeaponFramework::WeaponDefinition *CFrameworkWeapon::GetDefinition() const
{
	const WeaponFramework::WeaponRegistry &registry = WeaponFramework::GetRegistry();
	const WeaponFramework::WeaponDefinition *definition = NULL;
	if (m_iId > 0) definition = registry.FindById(m_iId);
	if (!definition && m_definitionName[0]) definition = registry.FindByName(m_definitionName);
	return definition;
}
void CFrameworkWeapon::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "weapon_name"))
	{
		const size_t length = pkvd->szValue ? strlen(pkvd->szValue) : 0;
		if (length > 0 && length < sizeof(m_definitionName)) memcpy(m_definitionName, pkvd->szValue, length + 1);
		else m_definitionName[0] = '\0';
		pkvd->fHandled = TRUE;
		return;
	}
	CBaseEntity::KeyValue(pkvd);
}
void CFrameworkWeapon::Precache(void)
{
	const WeaponFramework::WeaponDefinition *definition = GetDefinition();
	if (!definition) return;
	if (definition->GetViewModel()) PRECACHE_MODEL(definition->GetViewModel());
	if (definition->GetWorldModel()) PRECACHE_MODEL(definition->GetWorldModel());
	if (definition->GetPlayerModel()) PRECACHE_MODEL(definition->GetPlayerModel());
	if (definition->GetFireSound()) PRECACHE_SOUND(definition->GetFireSound());
}
void CFrameworkWeapon::Spawn(void)
{
	// GiveNamedItem places an assigned ID in iuser4 before DispatchSpawn.
	if (pev->iuser4 >= WeaponFramework::FirstAddonWeaponId &&
		pev->iuser4 <= WeaponFramework::LastAddonWeaponId)
		m_iId = pev->iuser4;
	const WeaponFramework::WeaponDefinition *definition = GetDefinition();
	if (!definition || definition->GetId() < WeaponFramework::FirstAddonWeaponId ||
		definition->GetId() > WeaponFramework::LastAddonWeaponId ||
		definition->GetActionType() != WeaponFramework::WeaponActionHitscan)
	{
#ifndef CLIENT_DLL
		ALERT(at_console, "WeaponFramework: unsupported weapon_framework entity; check weapon_name\n");
		UTIL_Remove(this);
#endif
		return;
	}
	m_iId = definition->GetId();
	pev->iuser4 = definition->GetId();
	m_iDefaultAmmo = definition->GetDefaultAmmo();
	m_iClip = 0;
	m_fInReload = FALSE;
#ifdef CLIENT_DLL
	pev->classname = MAKE_STRING(definition->GetEntityClassname());
#else
	if (!CBasePlayerItem::ItemInfoArray[definition->GetId()].pszName)
	{
		ALERT(at_console, "WeaponFramework: %s is unavailable because its ammo types could not be registered\n",
			definition->GetEntityClassname());
		UTIL_Remove(this);
		return;
	}
	Precache();
	if (!definition->GetWorldModel()) { UTIL_Remove(this); return; }
	SET_MODEL(ENT(pev), definition->GetWorldModel());
	pev->fuser1 = UTIL_WeaponTimeBase();
	FallInit();
#endif
}
int CFrameworkWeapon::GetItemInfo(ItemInfo *p)
{
	const WeaponFramework::WeaponDefinition *definition = GetDefinition();
	if (!definition || !p) return 0;
	p->pszName = definition->GetEntityClassname();
	p->pszAmmo1 = definition->GetPrimaryAmmoType();
	p->iMaxAmmo1 = definition->GetPrimaryAmmoMax();
	p->pszAmmo2 = definition->GetSecondaryAmmoType();
	p->iMaxAmmo2 = definition->GetSecondaryAmmoMax();
	p->iMaxClip = definition->GetClipSize();
	p->iSlot = definition->GetSlot();
	p->iPosition = definition->GetPosition();
	p->iFlags = definition->GetFlags();
	p->iId = definition->GetId();
	p->iWeight = definition->GetWeight();
	m_iId = definition->GetId();
	return 1;
}
int CFrameworkWeapon::AddToPlayer(CBasePlayer *pPlayer)
{
	if (!CBasePlayerWeapon::AddToPlayer(pPlayer)) return FALSE;
	MESSAGE_BEGIN(MSG_ONE, gmsgWeapPickup, NULL, pPlayer->pev);
		WRITE_BYTE(m_iId);
	MESSAGE_END();
	return TRUE;
}
int CFrameworkWeapon::iItemSlot(void)
{
	const WeaponFramework::WeaponDefinition *definition = GetDefinition();
	if (!definition) return 0;
	return Q_min(definition->GetSlot() + 1, MAX_ITEM_TYPES - 1);
}
BOOL CFrameworkWeapon::Deploy(void)
{
	const WeaponFramework::WeaponDefinition *definition = GetDefinition();
	if (!definition || !definition->GetViewModel()) return FALSE;
	const char *playerModel = definition->GetPlayerModel() ? definition->GetPlayerModel() : "";
	return DefaultDeploy(definition->GetViewModel(), playerModel, definition->GetDrawAnimation(), "onehanded", UseDecrement() ? 1 : 0);
}
void CFrameworkWeapon::PrimaryAttack(void)
{
	const WeaponFramework::WeaponDefinition *definition = GetDefinition();
	if (!definition || !m_pPlayer) return;
	if (!definition->IsAutomatic() && !(m_pPlayer->m_afButtonPressed & IN_ATTACK)) return;
	if (m_iClip <= 0)
	{
		if (m_iPrimaryAmmoType >= 0 && m_iPrimaryAmmoType < MAX_AMMO_SLOTS && m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] > 0) Reload();
		else { PlayEmptySound(); m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.2f; }
		return;
	}
	--m_iClip;
#ifndef CLIENT_DLL
	m_pPlayer->TabulateAmmo();
#endif
	m_pPlayer->m_iWeaponVolume = LOUD_GUN_VOLUME;
	m_pPlayer->m_iWeaponFlash = NORMAL_GUN_FLASH;
	const Vector source = m_pPlayer->GetGunPosition();
	const Vector aiming = m_pPlayer->GetAutoaimVector(AUTOAIM_10DEGREES);
	const float spread = definition->GetSpread();
	m_pPlayer->FireBulletsPlayer((ULONG)definition->GetPellets(), source, aiming, Vector(spread, spread, spread),
		(float)definition->GetRange(), definition->GetBulletType(), 0, definition->GetDamage(), m_pPlayer->pev, m_pPlayer->random_seed);
	m_pPlayer->pev->effects |= EF_MUZZLEFLASH;
	m_pPlayer->SetAnimation(PLAYER_ATTACK1);
	SendWeaponAnim(definition->GetFireAnimation(), UseDecrement() ? 1 : 0);
	if (definition->GetFireSound()) EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_WEAPON, definition->GetFireSound(), 1.0, ATTN_NORM);
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + definition->GetFireRate();
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 1.0f;
	if (m_iClip == 0 && m_iPrimaryAmmoType >= 0 && m_iPrimaryAmmoType < MAX_AMMO_SLOTS && m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0)
		m_pPlayer->SetSuitUpdate("!HEV_AMO0", FALSE, 0);
}
void CFrameworkWeapon::Reload(void)
{
	const WeaponFramework::WeaponDefinition *definition = GetDefinition();
	if (!definition || !m_pPlayer || m_iClip >= iMaxClip()) return;
	DefaultReload(iMaxClip(), definition->GetReloadAnimation(), definition->GetReloadTime());
}
void CFrameworkWeapon::WeaponIdle(void)
{
	const WeaponFramework::WeaponDefinition *definition = GetDefinition();
	if (!definition || !m_pPlayer) return;
	ResetEmptySound();
	if (m_flTimeWeaponIdle > UTIL_WeaponTimeBase()) return;
	SendWeaponAnim(definition->GetIdleAnimation(), UseDecrement() ? 1 : 0);
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;
}
#ifndef CLIENT_DLL
extern bool AddAmmoNamesToAmmoRegistry(const char *szAmmoName1, const char *szAmmoName2);
void WeaponFramework_PrecacheAddonWeapons(void)
{
	for (int id = WeaponFramework::FirstAddonWeaponId; id <= WeaponFramework::LastAddonWeaponId; ++id)
	{
		const WeaponFramework::WeaponDefinition *definition = WeaponFramework::GetRegistry().FindById(id);
		if (!definition || definition->GetActionType() != WeaponFramework::WeaponActionHitscan) continue;
		CFrameworkWeapon preview;
		ItemInfo info = {};
		preview.SetDefinitionId(id);
		preview.Precache();
		if (!preview.GetItemInfo(&info)) continue;

		const bool ammoReady = AddAmmoNamesToAmmoRegistry(info.pszAmmo1, info.pszAmmo2);

		if (!ammoReady)
		{
			ALERT(at_console, "WeaponFramework: skipping %s because the GoldSrc ammo table is full\n",
				definition->GetName());
			continue;
		}

		CBasePlayerItem::ItemInfoArray[id] = info;
	}
}
#endif

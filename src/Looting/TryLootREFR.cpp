/*************************************************************************
SmartHarvest SE
Copyright (c) Steve Townsend 2020

>>> SOURCE LICENSE >>>
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation (www.fsf.org); either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

A copy of the GNU General Public License is available at
http://www.fsf.org/licensing/licenses
>>> END OF LICENSE >>>
*************************************************************************/
#include "PrecompiledHeaders.h"
#include "Looting/TryLootREFR.h"
#include "Looting/ContainerLister.h"
#include "Looting/LootableREFR.h"
#include "Looting/ManagedLists.h"
#include "Looting/objects.h"
#include "Looting/ScanGovernor.h"
#include "Looting/TheftCoordinator.h"
#include "Collections/CollectionManager.h"
#include "Data/dataCase.h"
#include "FormHelpers/FormHelper.h"
#include "VM/EventPublisher.h"
#include "Utilities/debugs.h"
#include "Utilities/utils.h"
#include "WorldState/PlayerState.h"

namespace shse
{

TryLootREFR::TryLootREFR(RE::TESObjectREFR* target, INIFile::SecondaryType targetType, const bool stolen)
	: m_candidate(target), m_targetType(targetType), m_glowReason(GlowReason::None), m_stolen(stolen)
{
}

Lootability TryLootREFR::Process(const bool dryRun)
{
	if (!m_candidate)
		return Lootability::NullReference;

	DataCase* data = DataCase::GetInstance();
	Lootability result(Lootability::Lootable);
	if (m_targetType == INIFile::SecondaryType::itemObjects)
	{
		LootableREFR refrEx(m_candidate, m_targetType);
		ObjectType objType = refrEx.GetObjectType();
		m_typeName = refrEx.GetTypeName();
		// Various form types contain an ingredient or FormList that is the final lootable item - resolve here
		if (!dryRun && objType == ObjectType::critter)
		{
			RE::TESForm* lootable(ProducerLootables::Instance().GetLootableForProducer(m_candidate->GetBaseObject()));
			if (lootable)
			{
				DBG_VMESSAGE("producer {}/0x{:08x} has lootable {}/0x{:08x}", m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->formID,
					lootable->GetName(), lootable->formID);
				refrEx.SetLootable(lootable);
			}
			else
			{
				// trigger critter -> ingredient resolution and skip until it's resolved - pending resolve recorded using nullptr,
				// only trigger if not already pending
				DBG_VMESSAGE("resolve critter {}/0x{:08x} to ingredient", m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->formID);
				if (ProducerLootables::Instance().SetLootableForProducer(m_candidate->GetBaseObject(), nullptr))
				{
					EventPublisher::Instance().TriggerGetProducerLootable(m_candidate);
				}
				return Lootability::PendingProducerIngredient;
			}
		}

		// initially no glow - use synthetic value with highest precedence
		m_glowReason = GlowReason::None;
		bool skipLooting(false);

		// TODO this may update state on a dry run but we should already have processed the item on >= 1 pass, so no harm?
		// Check Collections first in case there are Manual Loot items that do not have an objectType, esp. scripted ACTI
		auto collectible(refrEx.TreatAsCollectible());
		if (collectible.first)
		{
			CollectibleHandling collectibleAction(collectible.second);
			DBG_VMESSAGE("Collectible Item 0x{:08x}", m_candidate->GetBaseObject()->formID);
			if (!CanLootCollectible(collectibleAction))
			{
				// ignore collectibility from here on, since we've determined it is unlootable
				collectible.first = false;
				skipLooting = true;
				if (collectibleAction == CollectibleHandling::Print)
				{
					if (!dryRun)
					{
						ProcessManualLootItem(m_candidate);
					}
					//we do not want to blacklist the base object even if it's not a proper objectType
					return Lootability::ManualLootTarget;
				}
				else if (collectibleAction == CollectibleHandling::Glow)
				{
					DBG_VMESSAGE("glow collectible object {}/0x{:08x}", m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->formID);
					UpdateGlowReason(GlowReason::Collectible);
					result = Lootability::CollectibleItemSetToGlow;
				}
				else
				{
					if (!dryRun)
					{
						// this is a blacklist collection, blacklist the item forever
						DBG_VMESSAGE("block blacklist collection member 0x{:08x}", m_candidate->GetBaseObject()->formID);
						data->BlockFormPermanently(m_candidate->GetBaseObject(), Lootability::ObjectIsInBlacklistCollection);
					}
					return Lootability::ObjectIsInBlacklistCollection;
				}
			}
		}

		if (objType == ObjectType::unknown)
		{
			if (!dryRun)
			{
				DBG_VMESSAGE("blacklist objType == ObjectType::unknown for 0x{:08x}", m_candidate->GetFormID());
				data->BlacklistReference(m_candidate);
			}
			return Lootability::ObjectTypeUnknown;
		}

		if (ManagedList::BlackList().Contains(m_candidate->GetBaseObject()))
		{
			DBG_VMESSAGE("Skip BlackListed REFR base form 0x{:08x}", m_candidate->GetBaseObject()->GetFormID());
			return Lootability::BaseObjectOnBlacklist;
		}

		bool needsFullQuestFlags(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "questObjectScope") != 0);
		SpecialObjectHandling questObjectLoot =
			SpecialObjectHandlingFromIniSetting(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "questObjectLoot"));
		if (refrEx.IsQuestItem(needsFullQuestFlags))
		{
			DBG_VMESSAGE("Quest Item 0x{:08x}", m_candidate->GetBaseObject()->formID);
			if (questObjectLoot == SpecialObjectHandling::GlowTarget)
			{
				DBG_VMESSAGE("glow quest object {}/0x{:08x}", m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->formID);
				UpdateGlowReason(GlowReason::QuestObject);
			}

			if (!IsSpecialObjectLootable(questObjectLoot))
			{
				skipLooting = true;
				// ignore collectibility from here on, since we've determined it is unlootable as a Quest Target
				collectible.first = false;
				result = Lootability::CannotLootQuestTarget;
			}
		}
		// glow unread notes as they are often quest-related
		else if (questObjectLoot == SpecialObjectHandling::GlowTarget && objType == ObjectType::book && IsBookGlowable())
		{
			DBG_VMESSAGE("Glowable book 0x{:08x}", m_candidate->GetBaseObject()->formID);
			UpdateGlowReason(GlowReason::SimpleTarget);
		}

		SpecialObjectHandling valuableLoot =
			SpecialObjectHandlingFromIniSetting(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "ValuableItemLoot"));
		if (refrEx.IsValuable())
		{
			DBG_VMESSAGE("Valuable Item 0x{:08x}", m_candidate->GetBaseObject()->formID);
			if (valuableLoot == SpecialObjectHandling::GlowTarget)
			{
				DBG_VMESSAGE("glow valuable object {}/0x{:08x}", m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->formID);
				UpdateGlowReason(GlowReason::Valuable);
			}

			if (!IsSpecialObjectLootable(valuableLoot))
			{
				skipLooting = true;
				// in this case, Collectibility can override the decision
				result = Lootability::CannotLootValuableObject;
			}
		}

		if (objType == ObjectType::ammo)
		{
			if (data->SkipAmmoLooting(m_candidate))
			{
				skipLooting = true;
				result = Lootability::CannotLootAmmo;
			}
		}

		// Order is important to ensure we glow correctly even if blocked. Collectibility may override the initial result.
		Lootability forbidden(ItemLootingLegality(collectible.first));
		if (forbidden != Lootability::Lootable)
		{
			skipLooting = true;
			result = forbidden;
		}

		if (!dryRun && m_glowReason != GlowReason::None)
		{
			ScanGovernor::Instance().GlowObject(m_candidate, ObjectGlowDurationSpecialSeconds, m_glowReason);
		}

		// Harvesting and mining is allowed in settlements. We really just want to not auto-loot entire
		// buildings of friendly factions, and the like. Mines and farms mostly self-identify as Settlements.
		if (!LocationTracker::Instance().IsPlayerInWhitelistedPlace(LocationTracker::Instance().PlayerCell()) &&
			LocationTracker::Instance().IsPlayerInRestrictedLootSettlement(LocationTracker::Instance().PlayerCell()) &&
			!IsItemLootableInPopulationCenter(m_candidate->GetBaseObject(), objType))
		{
			DBG_VMESSAGE("Player location is excluded as restricted population center for this item");
			result = Lootability::PopulousLocationRestrictsLooting;
			skipLooting = true;
		}

		LootingType lootingType(LootingType::LeaveBehind);
		if (collectible.first)
		{
			CollectibleHandling collectibleAction(collectible.second);
			// ** if configured as permitted ** collectible objects are always looted silently
			if (CanLootCollectible(collectibleAction))
			{
				skipLooting = forbidden != Lootability::Lootable;
				DBG_VMESSAGE("Lootable REFR to collectible 0x{:08x}, skip = {}", m_candidate->GetBaseObject()->formID,
				    skipLooting ? "true" : "false");
				lootingType = LootingType::LootAlwaysSilent;
			}
			else
			{
				DBG_VMESSAGE("Unlootable REFR to collectible 0x{:08x}", m_candidate->GetBaseObject()->formID);
				skipLooting = true;
			}
		}
		else if (ManagedList::WhiteList().Contains(m_candidate->GetBaseObject()))
		{
			// ** if configured as permitted ** whitelisted objects are always looted silently
			DBG_VMESSAGE("check REFR 0x{:08x} to whitelisted {}/0x{:08x}",
				m_candidate->GetFormID(), m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->formID);
			skipLooting = forbidden != Lootability::Lootable;
			if (skipLooting)
			{
				result = Lootability::LawAbidingSoNoWhitelistItemLooting;
			}
			lootingType = LootingType::LootAlwaysSilent;
		}
		else if (ManagedList::BlackList().Contains(m_candidate->GetBaseObject()))
		{
			// blacklisted objects are never looted
			DBG_VMESSAGE("disallow blacklisted Base {}/0x{:08x} for REFR 0x{:08x}",
				m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->GetFormID(), m_candidate->GetFormID());
			skipLooting = true;
			result = Lootability::ItemIsBlacklisted;
			lootingType = LootingType::LeaveBehind;
		}
		else if (!skipLooting)
		{
			lootingType = LootingTypeFromIniSetting(
				INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::itemObjects, m_typeName.c_str()));
			if (lootingType == LootingType::LeaveBehind)
			{
				if (!dryRun)
				{
					DBG_VMESSAGE("Block REFR : LeaveBehind for 0x{:08x}", m_candidate->GetBaseObject()->formID);
					data->BlockReference(m_candidate, Lootability::ItemTypeIsSetToPreventLooting);
				}
				skipLooting = true;
				result = Lootability::ItemTypeIsSetToPreventLooting;
			}
			else if (LootingDependsOnValueWeight(lootingType, objType))
			{
				TESFormHelper helper(m_candidate->GetBaseObject(), m_targetType);
				if (helper.ValueWeightTooLowToLoot())
				{
					if (!dryRun)
					{
						DBG_VMESSAGE("block - v/w excludes harvest for 0x{:08x}", m_candidate->GetBaseObject()->formID);
						data->BlockForm(m_candidate->GetBaseObject(), Lootability::ValueWeightPreventsLooting);
					}
					skipLooting = true;
					result = Lootability::ValueWeightPreventsLooting;
				}
				DBG_VMESSAGE("{}/0x{:08x} value:{}", m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->formID, int(helper.GetWorth()));
			}
		}

		if (skipLooting || dryRun)
			return result;

		// Check if we should attempt to steal the item. If we skip it due to looting rules, it's immune from stealing.
		// If we wish to auto-steal an item we must check we are not detected, which requires a scripted check. If this
		// is the delayed autoloot operation after we find we are undetected, don't trigger that check again here.
		if (!m_stolen && m_candidate->IsOffLimits() &&
			PlayerState::Instance().EffectiveOwnershipRule() == OwnershipRule::AllowCrimeIfUndetected)
		{
			DBG_VMESSAGE("REFR to be stolen if undetected");
			TheftCoordinator::Instance().DelayStealableItem(m_candidate, m_targetType);
			return Lootability::ItemTheftTriggered;
		}

		// don't try to re-harvest excluded, depleted or malformed ore vein again until we revisit the cell
		if (objType == ObjectType::oreVein)
		{
			DBG_VMESSAGE("loot oreVein - do not process again during this cell visit: 0x{:08x}", m_candidate->formID);
			data->BlockReference(m_candidate, Lootability::CannotMineTwiceInSameCellVisit);
			bool manualLootNotify(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "ManualLootTargetNotify") != 0);
			EventPublisher::Instance().TriggerMining(m_candidate, data->OreVeinResourceType(m_candidate->GetBaseObject()->As<RE::TESObjectACTI>()), manualLootNotify);
		}
		else
		{
			bool isSilent = !LootingRequiresNotification(lootingType);
			// don't let the backlog of messages get too large, it's about 1 per second
			// Event handler in Papyrus script unlocks the task - do not issue multiple concurrent events on the same REFR
			if (!ScanGovernor::Instance().LockHarvest(m_candidate, isSilent))
				return Lootability::HarvestOperationPending;
			DBG_VMESSAGE("SmartHarvest {}/0x{:08x} for REFR 0x{:08x}, collectible={}", m_candidate->GetBaseObject()->GetName(),
				m_candidate->GetBaseObject()->GetFormID(), m_candidate->GetFormID(), collectible.first ? "true" : "false");
			EventPublisher::Instance().TriggerHarvest(m_candidate, objType, refrEx.GetItemCount(),
				isSilent || ScanGovernor::Instance().PendingHarvestNotifications() > ScanGovernor::HarvestSpamLimit,
				collectible.first, PlayerState::Instance().PerkIngredientMultiplier());
		}
	}
	else if (m_targetType == INIFile::SecondaryType::containers || m_targetType == INIFile::SecondaryType::deadbodies)
	{
		DBG_MESSAGE("scanning container/body {}/0x{:08x}", m_candidate->GetName(), m_candidate->formID);
#if _DEBUG
		DumpContainer(LootableREFR(m_candidate, m_targetType));
#endif
		bool requireQuestItemAsTarget = INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "questObjectScope") != 0;
		bool skipLooting(false);
		// INI defaults exclude nudity by not looting armor from dead bodies
		bool excludeArmor(m_targetType == INIFile::SecondaryType::deadbodies &&
			DeadBodyLootingFromIniSetting(INIFile::GetInstance()->GetSetting(
				INIFile::PrimaryType::common, INIFile::SecondaryType::config, "EnableLootDeadbody")) == DeadBodyLooting::LootExcludingArmor);
		static const bool checkSpecials(true);
		ContainerLister lister(m_targetType, m_candidate, requireQuestItemAsTarget, checkSpecials);
		size_t lootableItems(lister.AnalyzeLootableItems());
		if (lootableItems == 0)
		{
			if (!dryRun)
			{
				// Nothing lootable here
				DBG_MESSAGE("container {}/0x{:08x} is empty", m_candidate->GetName(), m_candidate->formID);
				// record looting so we don't rescan
				ScanGovernor::Instance().MarkContainerLooted(m_candidate);
			}
			return Lootability::ContainerHasNoLootableItems;
		}

		// initially no glow - flag using synthetic value with highest precedence
		m_glowReason = GlowReason::None;
		if (m_targetType == INIFile::SecondaryType::containers)
		{
			// If a container is once found locked, it remains treated the same way according to the looting rules. This means a chest that player unlocked
			// will continue to glow if not auto-looted.
			if (ScanGovernor::Instance().IsReferenceLockedContainer(m_candidate))
			{
				SpecialObjectHandling lockedChestLoot =
					SpecialObjectHandlingFromIniSetting(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "lockedChestLoot"));
				if (lockedChestLoot == SpecialObjectHandling::GlowTarget)
				{
					DBG_VMESSAGE("glow locked container {}/0x{:08x}", m_candidate->GetName(), m_candidate->formID);
					UpdateGlowReason(GlowReason::LockedContainer);
				}

				if (!IsSpecialObjectLootable(lockedChestLoot))
				{
					skipLooting = true;
					result = Lootability::ContainerIsLocked;
				}
			}

			if (IsBossContainer(m_candidate))
			{
				SpecialObjectHandling bossChestLoot =
					SpecialObjectHandlingFromIniSetting(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "bossChestLoot"));
				if (bossChestLoot == SpecialObjectHandling::GlowTarget)
				{
					DBG_VMESSAGE("glow boss container {}/0x{:08x}", m_candidate->GetName(), m_candidate->formID);
					UpdateGlowReason(GlowReason::BossContainer);
				}

				if (!IsSpecialObjectLootable(bossChestLoot))
				{
					skipLooting = true;
					result = Lootability::ContainerIsBossChest;
				}
			}
		}

		if (lister.HasQuestItem())
		{
			SpecialObjectHandling questObjectLoot =
				SpecialObjectHandlingFromIniSetting(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "questObjectLoot"));
			if (questObjectLoot == SpecialObjectHandling::GlowTarget)
			{
				DBG_VMESSAGE("glow container with quest object {}/0x{:08x}", m_candidate->GetName(), m_candidate->formID);
				UpdateGlowReason(GlowReason::QuestObject);
			}

			if (!IsSpecialObjectLootable(questObjectLoot))
			{
				// this is not a blocker for looting of non-special items
				lister.ExcludeQuestItems();
				result = Lootability::ContainerHasQuestObject;
			}
		}

		if (lister.HasEnchantedItem())
		{
			int32_t enchantItemGlow = static_cast<int>(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "enchantItemGlow"));
			if (enchantItemGlow == 1)
			{
				DBG_VMESSAGE("glow container with enchanted object {}/0x{:08x}", m_candidate->GetName(), m_candidate->formID);
				UpdateGlowReason(GlowReason::EnchantedItem);
				// this is not a blocker for looting of non-special items
				lister.ExcludeEnchantedItems();
			}
		}

		if (lister.HasValuableItem())
		{
			SpecialObjectHandling valuableLoot =
				SpecialObjectHandlingFromIniSetting(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "ValuableItemLoot"));
			if (valuableLoot == SpecialObjectHandling::GlowTarget)
			{
				DBG_VMESSAGE("glow container with valuable object {}/0x{:08x}", m_candidate->GetName(), m_candidate->formID);
				UpdateGlowReason(GlowReason::Valuable);
				// this is not a blocker for looting of non-special items
				lister.ExcludeValuableItems();
			}

			if (!IsSpecialObjectLootable(valuableLoot))
			{
				result = Lootability::ContainerHasValuableObject;
			}
		}

		if (lister.HasCollectibleItem())
		{
			if (!CanLootCollectible(lister.CollectibleAction()))
			{
				// this is not a blocker for looting of non-special items
				lister.ExcludeCollectibleItems();

				if (lister.CollectibleAction() == CollectibleHandling::Glow)
				{
					DBG_VMESSAGE("glow container with collectible object {}/0x{:08x}", m_candidate->GetName(), m_candidate->formID);
					UpdateGlowReason(GlowReason::Collectible);
					result = Lootability::CollectibleItemSetToGlow;
				}
				else if (lister.CollectibleAction() == CollectibleHandling::Print)
				{
					result = Lootability::ManualLootTarget;
				}
				else
				{
					result = Lootability::ItemInBlacklistCollection;
				}
			}
		}

		// Order is important to ensure we glow correctly even if blocked - IsLootingForbidden must come first.
		// Check here is on the container, skip all contents if looting not permitted
		Lootability forbidden(LootingLegality(m_targetType));
		if (forbidden != Lootability::Lootable)
		{
			skipLooting = true;
			result = forbidden;
		}
		else if (DataCase::GetInstance()->ReferencesBlacklistedContainer(m_candidate))
		{
			skipLooting = true;
			result = Lootability::ContainerIsBlacklisted;
		}

		// Always allow auto-looting of dead bodies, e.g. Solitude Hall of the Dead in LCTN Solitude has skeletons that we
		// should be able to murder/plunder. And don't forget Margret in Markarth.
		if (!skipLooting && m_targetType != INIFile::SecondaryType::deadbodies &&
			!LocationTracker::Instance().IsPlayerInWhitelistedPlace(LocationTracker::Instance().PlayerCell()) &&
			LocationTracker::Instance().IsPlayerInRestrictedLootSettlement(LocationTracker::Instance().PlayerCell()))
		{
			DBG_VMESSAGE("Player location is excluded as restricted population center for this target type");
			skipLooting = true;
			result = Lootability::PopulousLocationRestrictsLooting;
		}

		if (!dryRun && m_glowReason != GlowReason::None)
		{
			ScanGovernor::Instance().GlowObject(m_candidate, ObjectGlowDurationSpecialSeconds, m_glowReason);
		}

		// If it contains white-listed items we must nonetheless skip, due to legality checks at the container level
		if (dryRun || skipLooting)
			return result;

		// Check if we should attempt to loot the target's contents the item. If we skip it due to looting rules, it's
		// immune from stealing.
		// If we wish to auto-steal an item we must check we are not detected, which requires a scripted check. If this
		// is the delayed autoloot operation after we find we are undetected, don't trigger that check again here.
		if (!m_stolen && m_candidate->IsOffLimits() &&
			PlayerState::Instance().EffectiveOwnershipRule() == OwnershipRule::AllowCrimeIfUndetected)
		{
			DBG_VMESSAGE("Container/deadbody contents {}/0x{:08x} to be stolen if undetected", m_candidate->GetName(), m_candidate->formID);
			TheftCoordinator::Instance().DelayStealableItem(m_candidate, m_targetType);
			return Lootability::ItemTheftTriggered;
		}

		// Build list of lootable targets with notification, collectibility flag & count for each
		std::vector<std::tuple<InventoryItem, bool, bool, size_t>> targets;
		targets.reserve(lootableItems);
		for (auto& targetItemInfo : lister.GetLootableItems())
		{
			RE::TESBoundObject* target(targetItemInfo.BoundObject());
			if (!target)
				continue;

			if (ManagedList::BlackList().Contains(target))
			{
				DBG_VMESSAGE("skip 0x{:08x} due to BlackList", target->formID);
				continue;
			}

			ObjectType objType = targetItemInfo.LootObjectType();
			if (excludeArmor && (objType == ObjectType::armor || objType == ObjectType::enchantedArmor))
			{
				// obey SFW setting, for this REFR on this pass - state resets on game reload/cell re-entry/MCM update
				DBG_VMESSAGE("block looting of armor from dead body {}/0x{:08x}", target->GetName(), target->GetFormID());
				continue;
			}

			if (objType == ObjectType::weapon || objType == ObjectType::armor || objType == ObjectType::jewelry)
			{
				bool hasEnchantment = GetEnchantmentFromExtraLists(targetItemInfo.GetExtraDataLists()) != nullptr;
				if (hasEnchantment) {
					DBG_VMESSAGE("{}/0x{:08x} has player-created enchantment",
						targetItemInfo.BoundObject()->GetName(), targetItemInfo.BoundObject()->GetFormID());
					switch (objType)
					{
					case ObjectType::weapon:
						objType = ObjectType::enchantedWeapon;
						break;
					case ObjectType::armor:
						objType = ObjectType::enchantedArmor;
						break;
					case ObjectType::jewelry:
						objType = ObjectType::enchantedJewelry;
						break;
					default:
						break;
					}
				}
			}

			LootingType lootingType(LootingType::LeaveBehind);
			const auto collectible(CollectionManager::Instance().TreatAsCollectible(
				ConditionMatcher(targetItemInfo.BoundObject(), m_targetType)));
			if (collectible.first)
			{
				if (CanLootCollectible(collectible.second))
				{
					DBG_VMESSAGE("Collectible Item 0x{:08x}", targetItemInfo.BoundObject()->formID);
					lootingType = LootingType::LootAlwaysSilent;
				}
				else
				{
					// blacklisted or 'glow'
					DBG_VMESSAGE("Collectible Item 0x{:08x} skipped", targetItemInfo.BoundObject()->formID);
					continue;
				}
			}
			else if (ManagedList::WhiteList().Contains(target))
			{
				// whitelisted objects are always looted silently
				DBG_VMESSAGE("transfer whitelisted 0x{:08x}", target->formID);
				lootingType = LootingType::LootAlwaysSilent;
			}
			else if (ManagedList::BlackList().Contains(target))
			{
				// blacklisted objects are never looted
				DBG_VMESSAGE("skip blacklisted target {}/0x{:08x}", target->GetName(), target->GetFormID());
				continue;
			}
			else
			{
				std::string typeName = GetObjectTypeName(objType);
				lootingType = LootingTypeFromIniSetting(
					INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::itemObjects, typeName.c_str()));

				if (lootingType == LootingType::LeaveBehind)
				{
					DBG_VMESSAGE("block - typename {} excluded for 0x{:08x}", typeName.c_str(), target->formID);
					data->BlockForm(target, Lootability::ItemTypeIsSetToPreventLooting);
					continue;
				}
				else if (LootingDependsOnValueWeight(lootingType, objType) &&
					TESFormHelper(target, m_targetType).ValueWeightTooLowToLoot())
				{
					DBG_VMESSAGE("block - v/w excludes for 0x{:08x}", target->formID);
					data->BlockForm(target, Lootability::ValueWeightPreventsLooting);
					continue;
				}
			}

			// crime-check this REFR from the container as individual object, respecting collectibility if not a crime
			if (ItemLootingLegality(collectible.first) != Lootability::Lootable)
			{
				continue;
			}

			// item count unknown at this point
			targets.push_back({ targetItemInfo, LootingRequiresNotification(lootingType), collectible.first, 0 });
			DBG_MESSAGE("get {} ({}) from container {}/0x{:08x}", target->GetName(), targetItemInfo.Count(),
				m_candidate->GetName(), m_candidate->formID);
		}

		if (!targets.empty())
		{
			// check highlighting for dead NPC or container
			int playContainerAnimation(static_cast<int>(INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::harvest, INIFile::SecondaryType::config, "PlayContainerAnimation")));
			if (playContainerAnimation > 0)
			{
				if (m_targetType == INIFile::SecondaryType::containers)
				{
					if (!GetTimeController(m_candidate))
					{
						// no container animation feasible, highlight it instead
						playContainerAnimation = 2;
					}
				}
				else
				{
					// Dead NPCs cannot be animated, but highlighting requested
					playContainerAnimation = 2;
				}
			}
			// use inline transfer for containers on first attempt - fills in item counts
			GetLootFromContainer(targets, playContainerAnimation, m_targetType == INIFile::SecondaryType::containers);
		}

		// Avoid re-looting without a player cell or config change. Sometimes auto-looting here may fail, so we just copy the
		// items and blacklist the REFR to avoid revisiting. Confirm looting by checking lootable target count now vs start
		// value. This logic only applies to containers: NPC auto-looting is scripted and not known to fail.
		if (m_targetType == INIFile::SecondaryType::containers && !targets.empty() &&
		    lister.AnalyzeLootableItems() >= lootableItems)
		{
			// nothing looted - make copies of targets and blacklist the reference (e.g. MrB's Lootable Things)
			REL_WARNING("looting {} items from container {}/0x{:08x} resulted in no-op, make copies", targets.size(),
				m_candidate->GetName(), m_candidate->formID);
			CopyLootFromContainer(targets);
			DataCase::GetInstance()->BlacklistReference(m_candidate);
		}
		else
		{
			DBG_MESSAGE("block looted container/NPC {}/0x{:08x}", m_candidate->GetName(), m_candidate->formID);
			ScanGovernor::Instance().MarkContainerLooted(m_candidate);
		}
	}
	return result;
}

void TryLootREFR::GetLootFromContainer(std::vector<std::tuple<InventoryItem, bool, bool, size_t>>& targets,
	const int animationType, const bool inlineTransfer)
{
	if (!m_candidate)
		return;

	// visual notification, if requested
	if (animationType == 1)
	{
		m_candidate->PlayAnimation("Close", "Open");
	}
	else if (animationType == 2)
	{
		// glow looted object briefly after looting
		ScanGovernor::Instance().GlowObject(m_candidate, ObjectGlowDurationLootedSeconds, GlowReason::SimpleTarget);
	}

	// avoid sound spam
	bool madeSound(false);
	for (auto& target : targets)
	{
		// Play sound first as this uses InventoryItemData on the source container
		InventoryItem& itemInfo(std::get<0>(target));
		bool notify(std::get<1>(target));
		bool collectible(std::get<2>(target));
		if (!madeSound)
		{
			RE::PlayerCharacter::GetSingleton()->PlayPickUpSound(itemInfo.BoundObject(), true, false);
			madeSound = true;
		}
		std::string name(itemInfo.BoundObject()->GetName());
		size_t count(itemInfo.TakeAll(m_candidate, RE::PlayerCharacter::GetSingleton(), collectible, inlineTransfer));
		// save count in case we have to copy these after failure to transfer (e.g. MrB's Lootable Things)
		std::get<3>(target) = count;
		if (notify)
		{
			std::string notificationText;
			if (count > 1)
			{
				static RE::BSFixedString multiActivate(DataCase::GetInstance()->GetTranslation("$SHSE_ACTIVATE(COUNT)_MSG"));
				if (!multiActivate.empty())
				{
					notificationText = multiActivate;
					StringUtils::Replace(notificationText, "{ITEMNAME}", name.c_str());
					std::ostringstream intStr;
					intStr << count;
					StringUtils::Replace(notificationText, "{COUNT}", intStr.str());
				}
			}
			else
			{
				static RE::BSFixedString singleActivate(DataCase::GetInstance()->GetTranslation("$SHSE_ACTIVATE_MSG"));
				if (!singleActivate.empty())
				{
					notificationText = singleActivate;
					StringUtils::Replace(notificationText, "{ITEMNAME}", name.c_str());
				}
			}
			if (!notificationText.empty())
			{
				RE::DebugNotification(notificationText.c_str());
			}
		}
	}
}

void TryLootREFR::CopyLootFromContainer(std::vector<std::tuple<InventoryItem, bool, bool, size_t>>& targets)
{
	if (!m_candidate)
		return;

	for (auto& target : targets)
	{
		InventoryItem& itemInfo(std::get<0>(target));
		itemInfo.MakeCopies(RE::PlayerCharacter::GetSingleton(), std::get<3>(target));
	}
}

Lootability TryLootREFR::ItemLootingLegality(const bool isCollectible)
{
	Lootability result(LootingLegality(INIFile::SecondaryType::itemObjects));
	if (isCollectible && LootOwnedItemIfCollectible(result))
	{
		DBG_VMESSAGE("Collectible REFR 0x{:08x} overrides Legality {} for {}/0x{:08x}", m_candidate->GetFormID(), LootabilityName(result).c_str(),
			m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->GetFormID());
		result = Lootability::Lootable;
	}
	return result;
}

Lootability TryLootREFR::LootingLegality(const INIFile::SecondaryType targetType)
{
	// Already trying to steal this - bypass repeat check, known to be OK modulo actor or player state change
	// in the world
	if (m_stolen)
		return Lootability::Lootable;

	Lootability legality(Lootability::Lootable);
	// Perform crime checks - this is done after checks for quest object glowing, as many quest-related objects are owned.
	// Ownership expires with the target, e.g. Francis the Horse from Interesting NPCs was killed by a wolf in Solitude
	// and becomes lootable thereafter.
	// For non-dead targets, check law-abiding settings vs criminality of target and player-ownership settings vs ownership
	if (targetType != INIFile::SecondaryType::deadbodies)
	{
		// check up to three ownership conditions depending on config
		bool playerOwned(IsPlayerOwned(m_candidate));
		// Fired arrows are marked as player owned but we don't want to prevent pickup, ever
		bool firedArrow(m_candidate->formType == RE::FormType::ProjectileArrow);
		bool lootingIsCrime(m_candidate->IsOffLimits());
		if (!lootingIsCrime && playerOwned && !firedArrow)
		{
			// can configure to not loot my own belongings even though it's always legal
			if (!IsSpecialObjectLootable(PlayerState::Instance().BelongingsCheck()))
			{
				DBG_VMESSAGE("Player-owned {}, looting belongings disallowed: {}/0x{:08x}",
					playerOwned ? "true" : "false", m_candidate->GetBaseObject()->GetName(), m_candidate->GetBaseObject()->formID);
				legality = Lootability::PlayerOwned;
				// Glow if configured
				if (PlayerState::Instance().BelongingsCheck() == SpecialObjectHandling::GlowTarget)
					UpdateGlowReason(GlowReason::PlayerProperty);
			}
		}
		// if restricted to law-abiding citizenship, check if OK to loot
		else if (PlayerState::Instance().EffectiveOwnershipRule() != OwnershipRule::AllowCrimeIfUndetected)
		{
			if (lootingIsCrime)
			{
				// never commit a crime unless crimeCheck is 0
				DBG_VMESSAGE("Crime to loot REFR, cannot loot");
				legality = Lootability::CrimeToLoot;
			}
			else if (PlayerState::Instance().EffectiveOwnershipRule() == OwnershipRule::Ownerless)
			{
				if (!playerOwned && !firedArrow &&
					(m_candidate->GetOwner() != nullptr || !LocationTracker::Instance().IsPlayerInFriendlyCell()))
				{
					// owner of item or cell is not player/player-friendly - disallow owned item
					DBG_VMESSAGE("REFR or Cell is not player-owned, cannot loot");
					legality = Lootability::CellOrItemOwnerPreventsOwnerlessLooting;
				}
			}
		}
	}
	return legality;
}

bool TryLootREFR::IsBookGlowable() const
{
	RE::BGSKeywordForm* keywordForm(m_candidate->GetBaseObject()->As<RE::BGSKeywordForm>());
	if (!keywordForm)
		return false;
	for (uint32_t index = 0; index < keywordForm->GetNumKeywords(); ++index)
	{
		std::optional<RE::BGSKeyword*> keyword(keywordForm->GetKeywordAt(index));
		if (!keyword || !keyword.has_value())
			continue;
		if (DataCase::GetInstance()->IsBookGlowableKeyword(keyword.value()))
			return true;
	}
	return false;
}

}
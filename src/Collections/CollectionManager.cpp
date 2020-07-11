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

#include <filesystem>
#include <regex>

#include "Utilities/LogStackWalker.h"
#include "Utilities/Exception.h"
#include "Utilities/utils.h"
#include "WorldState/LocationTracker.h"
#include "VM/EventPublisher.h"
#include "Collections/CollectionManager.h"
#include "Collections/CollectionFactory.h"
#include "Data/DataCase.h"
#include "Looting/ManagedLists.h"

namespace shse
{

std::unique_ptr<CollectionManager> CollectionManager::m_instance;

CollectionManager& CollectionManager::Instance()
{
	if (!m_instance)
	{
		m_instance = std::make_unique<CollectionManager>();
	}
	return *m_instance;
}

CollectionManager::CollectionManager() : m_ready(false), m_enabled(false), m_gameTime(0.0)
{
}

// Generate Collection Definitions from JSON Config
void CollectionManager::ProcessDefinitions(void)
{
	// call only once
	if (IsActive())
		return;

	__try {
		if (!LoadData())
			return;

		// data validated and loaded
		m_ready = true;
	}
	__except (LogStackWalker::LogStack(GetExceptionInformation())) {
		REL_FATALERROR("JSON Collection Definitions threw structured exception");
	}
}

void CollectionManager::Refresh() const
{
	// request added items and game time to be pushed to us while we are sleeping
	if (IsActive())
		EventPublisher::Instance().TriggerFlushAddedItems();
}

void CollectionManager::UpdateGameTime(const float gameTime)
{
	RecursiveLockGuard guard(m_collectionLock);
	m_gameTime = gameTime;
}

void CollectionManager::CheckEnqueueAddedItem(const RE::FormID formID)
{
	if (!IsActive())
		return;
	RecursiveLockGuard guard(m_collectionLock);
	// only pass this along if it is in >= 1 collection
	if (m_collectionsByFormID.contains(formID))
	{
		EnqueueAddedItem(formID);
	}
}

void CollectionManager::EnqueueAddedItem(const RE::FormID formID)
{
	m_addedItemQueue.emplace_back(formID);
}

void CollectionManager::ProcessAddedItems()
{
	if (!IsActive())
		return;

#ifdef _PROFILING
	WindowsUtils::ScopedTimer elapsed("Collection checks");
#endif
	RecursiveLockGuard guard(m_collectionLock);
	constexpr std::chrono::milliseconds InventoryReconciliationIntervalMillis(3000LL);
	const auto nowTime(std::chrono::high_resolution_clock::now());
	if (nowTime - m_lastInventoryCheck >= InventoryReconciliationIntervalMillis)
	{
		DBG_MESSAGE("Inventory reconciliation required");
		m_lastInventoryCheck = nowTime;
		const auto inventoryAdds(ReconcileInventory());
		if (!inventoryAdds.empty())
		{
			m_addedItemQueue.insert(m_addedItemQueue.end(), inventoryAdds.cbegin(), inventoryAdds.cend());
		}
	}

	decltype(m_addedItemQueue) queuedItems;
	queuedItems.swap(m_addedItemQueue);
	for (const auto formID : queuedItems)
	{
		// only process items known to be a member of at least one collection
		if (m_collectionsByFormID.contains(formID))
		{
			DBG_VMESSAGE("Check collectability of added item 0x%08x", formID);
			AddToRelevantCollections(formID);
		}
		else if (m_nonCollectionForms.insert(formID).second)
		{
			DBG_VMESSAGE("Recorded 0x%08x as non-collectible", formID);
		}
	}
}

// bucket newly-received items in any matching collections
void CollectionManager::AddToRelevantCollections(const RE::FormID itemID)
{
	// resolve ID to Form
	RE::TESForm* form(RE::TESForm::LookupByID(itemID));
	if (!form)
		return;
	RecursiveLockGuard guard(m_collectionLock);
	const auto targets(m_collectionsByFormID.equal_range(form->GetFormID()));
	for (auto collection = targets.first; collection != targets.second; ++collection)
	{
		// Do not record if the policy indicates to LeaveBehind (blacklist)
		if (collection->second->Policy().Action() != SpecialObjectHandling::DoNotLoot &&
			collection->second->IsMemberOf(form))
		{
			// record membership
			collection->second->RecordItem(itemID, form, m_gameTime, LocationTracker::Instance().CurrentPlayerPlace());
		}
	}
}

std::pair<bool, SpecialObjectHandling> CollectionManager::TreatAsCollectible(const ConditionMatcher& matcher)
{
	if (!IsActive() || !matcher.Form())
		return NotCollectible;
	RecursiveLockGuard guard(m_collectionLock);
	if (m_nonCollectionForms.contains(matcher.Form()->GetFormID()))
		return NotCollectible;

	// find Collections that match this Form
	const auto targets(m_collectionsByFormID.equal_range(matcher.Form()->GetFormID()));
	if (targets.first == m_collectionsByFormID.cend())
	{
		DBG_VMESSAGE("Record %s/0x%08x as non-collectible", matcher.Form()->GetName(), matcher.Form()->GetFormID());
		m_nonCollectionForms.insert(matcher.Form()->GetFormID());
		return NotCollectible;
	}

	// It is in at least one collection. Find the most aggressive action for any where we are in scope and a usable member.
	SpecialObjectHandling action(SpecialObjectHandling::DoNotLoot);
	bool actionable(false);
	for (auto collection = targets.first; collection != targets.second; ++collection)
	{
		if (collection->second->InScopeAndCollectibleFor(matcher))
		{
			actionable = true;
			action = UpdateSpecialObjectHandling(collection->second->Policy().Action(), action);
		}
	}
	return std::make_pair(actionable, action);
}

// Player inventory can get objects from Loot menus and other sources than our harvesting, we need to account for them
// We don't do this on every pass as it's a decent amount of work
std::vector<RE::FormID> CollectionManager::ReconcileInventory()
{
	RE::PlayerCharacter* player(RE::PlayerCharacter::GetSingleton());
	if (!player)
		return std::vector<RE::FormID>();

	// use delta vs last pass to speed this up (resets on game reload)
	decltype(m_lastInventoryItems) newInventoryItems;
	std::vector<RE::FormID> candidates;
	const auto inv = player->GetInventory([&](RE::TESBoundObject* candidate) -> bool {
		RE::FormID formID(candidate->GetFormID());
		newInventoryItems.insert(formID);
		if (!m_lastInventoryItems.contains(formID) && m_collectionsByFormID.contains(formID))
		{
			DBG_VMESSAGE("Collectible %s/0x%08x new in inventory", candidate->GetName(), formID);
			candidates.push_back(formID);
		}
		return false;
	});
	m_lastInventoryItems.swap(newInventoryItems);
	return candidates;
}

bool CollectionManager::LoadCollectionsFromFile(
	const std::filesystem::path& defFile, const std::string& groupName, nlohmann::json_schema::json_validator& validator)
{
	try {
		std::ifstream collectionFile(defFile);
		if (collectionFile.fail()) {
			throw FileNotFound(defFile.generic_string().c_str());
		}
		nlohmann::json collectionDefinitions(nlohmann::json::parse(collectionFile));
		validator.validate(collectionDefinitions);
		BuildDecisionTrees(collectionDefinitions, groupName);
		m_fileNamesByGroupName.insert(std::make_pair(groupName, defFile.string()));
		return true;
	}
	catch (const std::exception& e) {
		REL_ERROR("JSON Collection Definitions %s not loadable, error:\n%s", defFile.generic_string().c_str(), e.what());
		return false;
	}
}

bool CollectionManager::LoadData(void)
{
	// Validate the schema
	const std::string schemaFileName("SHSE.SchemaCollections.json");
	std::string filePath(FileUtils::GetPluginPath() + schemaFileName);
	nlohmann::json_schema::json_validator validator;
	try {
		std::ifstream schemaFile(filePath);
		if (schemaFile.fail()) {
			throw FileNotFound(filePath.c_str());
		}
		nlohmann::json schema(nlohmann::json::parse(schemaFile));
		validator.set_root_schema(schema); // insert root-schema
	}
	catch (const std::exception& e) {
		REL_ERROR("JSON Schema %s not loadable, error:\n%s", filePath.c_str(), e.what());
		return false;
	}

	REL_MESSAGE("JSON Schema %s parsed and validated", filePath.c_str());

	// Find and Load Collection Definitions using the validated schema
	const std::regex collectionsFilePattern("SHSE.Collections\\.(.*)\\.json$");
	for (const auto& nextFile : std::filesystem::directory_iterator(FileUtils::GetPluginPath()))
	{
		if (!std::filesystem::is_regular_file(nextFile))
		{
			DBG_MESSAGE("Skip %s, not a regular file", nextFile.path().generic_string().c_str());
			continue;
		}
		std::string fileName(nextFile.path().filename().generic_string());
		std::smatch matches;
		if (!std::regex_search(fileName, matches, collectionsFilePattern))
		{
			DBG_MESSAGE("Skip %s, does not match Collections filename pattern", fileName.c_str());
				continue;
		}
		// capture string at index 1 is the Collection Name, always present after a regex match
		REL_MESSAGE("Load JSON Collection Definitions %s for Group %s", fileName.c_str(), matches[1].str().c_str());
		if (LoadCollectionsFromFile(nextFile, matches[1].str(), validator))
		{
			REL_MESSAGE("JSON Collection Definitions %s/%s parsed and validated", fileName.c_str(), matches[1].str().c_str());
		}
	}
	PrintDefinitions();
	RecordPlacedObjects();
	ResolveMembership();
	return true;
}

void CollectionManager::PrintDefinitions(void) const
{
	for (const auto& collection : m_allCollectionsByLabel)
	{
		REL_MESSAGE("Collection %s:\n%s", collection.first.c_str(), collection.second->PrintDefinition().c_str());
	}
}

void CollectionManager::PrintMembership(void) const
{
	for (const auto& collection : m_allCollectionsByLabel)
	{
		REL_MESSAGE("Collection %s:\n%s", collection.first.c_str(), collection.second->PrintMembers().c_str());
	}
}

int CollectionManager::NumberOfFiles(void) const
{
	RecursiveLockGuard guard(m_collectionLock);
	return static_cast<int>(m_fileNamesByGroupName.size());
}

std::string CollectionManager::GroupNameByIndex(const int fileIndex) const
{
	RecursiveLockGuard guard(m_collectionLock);
	size_t index(0);
	for (const auto& group : m_fileNamesByGroupName)
	{
		if (index == fileIndex)
			return group.first;
		++index;
	}
	return std::string();
}

std::string CollectionManager::GroupFileByIndex(const int fileIndex) const
{
	RecursiveLockGuard guard(m_collectionLock);
	size_t index(0);
	for (const auto& group : m_fileNamesByGroupName)
	{
		if (index == fileIndex)
			return group.second;
		++index;
	}
	return std::string();
}

int CollectionManager::NumberOfCollections(const std::string& groupName) const
{
	RecursiveLockGuard guard(m_collectionLock);
	return static_cast<int>(m_collectionsByGroupName.count(groupName));
}

std::string CollectionManager::NameByGroupIndex(const std::string& groupName, const int collectionIndex) const
{
	RecursiveLockGuard guard(m_collectionLock);
	const auto matches(m_collectionsByGroupName.equal_range(groupName));
	size_t index(0);
	for (auto group = matches.first; group != matches.second; ++group)
	{
		if (index == collectionIndex)
		{
			// strip the group name and separator
			return group->second.substr(groupName.length() + 1);
		}
		++index;
	}
	return std::string();
}

std::string CollectionManager::MakeLabel(const std::string& groupName, const std::string& collectionName)
{
	std::ostringstream labelStream;
	labelStream << groupName << '/' << collectionName;
	return labelStream.str();
}

bool CollectionManager::PolicyRepeat(const std::string& groupName, const std::string& collectionName) const
{
	const std::string label(MakeLabel(groupName, collectionName));
	RecursiveLockGuard guard(m_collectionLock);
	const auto matched(m_allCollectionsByLabel.find(label));
	if (matched != m_allCollectionsByLabel.cend())
	{
		return matched->second->Policy().Repeat();
	}
	return false;
}

bool CollectionManager::PolicyNotify(const std::string& groupName, const std::string& collectionName) const
{
	const std::string label(MakeLabel(groupName, collectionName));
	RecursiveLockGuard guard(m_collectionLock);
	const auto matched(m_allCollectionsByLabel.find(label));
	if (matched != m_allCollectionsByLabel.cend())
	{
		return matched->second->Policy().Notify();
	}
	return false;
}

SpecialObjectHandling CollectionManager::PolicyAction(const std::string& groupName, const std::string& collectionName) const
{
	const std::string label(MakeLabel(groupName, collectionName));
	RecursiveLockGuard guard(m_collectionLock);
	const auto matched(m_allCollectionsByLabel.find(label));
	if (matched != m_allCollectionsByLabel.cend())
	{
		return matched->second->Policy().Action();
	}
	return SpecialObjectHandling::DoNotLoot;
}

void CollectionManager::PolicySetRepeat(const std::string& groupName, const std::string& collectionName, const bool allowRepeats)
{
	const std::string label(MakeLabel(groupName, collectionName));
	RecursiveLockGuard guard(m_collectionLock);
	auto matched(m_allCollectionsByLabel.find(label));
	if (matched != m_allCollectionsByLabel.cend())
	{
		matched->second->Policy().SetRepeat(allowRepeats);
	}
}

void CollectionManager::PolicySetNotify(const std::string& groupName, const std::string& collectionName, const bool notify)
{
	const std::string label(MakeLabel(groupName, collectionName));
	RecursiveLockGuard guard(m_collectionLock);
	auto matched(m_allCollectionsByLabel.find(label));
	if (matched != m_allCollectionsByLabel.cend())
	{
		return matched->second->Policy().SetNotify(notify);
	}
}

void CollectionManager::PolicySetAction(const std::string& groupName, const std::string& collectionName, const SpecialObjectHandling action)
{
	const std::string label(MakeLabel(groupName, collectionName));
	RecursiveLockGuard guard(m_collectionLock);
	auto matched(m_allCollectionsByLabel.find(label));
	if (matched != m_allCollectionsByLabel.cend())
	{
		matched->second->Policy().SetAction(action);
	}
}

size_t CollectionManager::TotalItems(const std::string& groupName, const std::string& collectionName) const
{
	const std::string label(MakeLabel(groupName, collectionName));
	RecursiveLockGuard guard(m_collectionLock);
	const auto matched(m_allCollectionsByLabel.find(label));
	if (matched != m_allCollectionsByLabel.cend())
	{
		return matched->second->Count();
	}
	return 0;
}

size_t CollectionManager::ItemsObtained(const std::string& groupName, const std::string& collectionName) const
{
	const std::string label(MakeLabel(groupName, collectionName));
	RecursiveLockGuard guard(m_collectionLock);
	const auto matched(m_allCollectionsByLabel.find(label));
	if (matched != m_allCollectionsByLabel.cend())
	{
		return matched->second->Observed();
	}
	return 0;
}

void CollectionManager::BuildDecisionTrees(nlohmann::json& collectionDefinitions, const std::string& groupName)
{
	for (const auto& definition : collectionDefinitions["collections"])
	{
		try {
			std::shared_ptr<Collection> filter(CollectionFactory::Instance().ParseCollection(definition));
			const std::string label(MakeLabel(groupName, definition["name"].get<std::string>()));
			if (m_allCollectionsByLabel.insert(std::make_pair(label, filter)).second)
			{
				REL_MESSAGE("Decision Tree built for Collection %s", label.c_str());
				m_collectionsByGroupName.insert(std::make_pair(groupName, label));
			}
			else
			{
				REL_WARNING("Discarded Decision Tree for duplicate Collection %s", label.c_str());
			}
		}
		catch (const std::exception& exc) {
			REL_ERROR("Error %s building Decision Tree for Collection\n%s", exc.what(), definition.dump(2).c_str());
		}
	}
}

// record all the Placed instances, so we can validate Collections and send Player to find items later
void CollectionManager::RecordPlacedItem(const RE::TESForm* item, const RE::TESObjectREFR* refr)
{
	m_placedItems.insert(item);
	m_placedObjects.insert(std::make_pair(item, refr));
}

void CollectionManager::SaveREFRIfPlaced(const RE::TESObjectREFR* refr)
{
	// skip if empty REFR
	if (!refr)
	{
		DBG_VMESSAGE("REFR invalid");
		return;
	}
	// skip if no BaseObject
	if (!refr->GetBaseObject())
	{
		DBG_VMESSAGE("REFR 0x%08x no base", refr->GetFormID());
		return;
	}
	// skip if not a valid BaseObject for Collections, or a placed Container or Corpse that we need to introspect
	if (!SignatureCondition::IsValidFormType(refr->GetBaseObject()->GetFormType()) &&
		refr->GetBaseObject()->GetFormType() != RE::FormType::Container &&
		refr->GetFormType() != RE::FormType::ActorCharacter)
	{
		DBG_VMESSAGE("REFR 0x%08x Base %s/0x%08x invalid FormType %d", refr->GetFormID(), refr->GetBaseObject()->GetName(),
			refr->GetBaseObject()->GetFormID(), refr->GetBaseObject()->GetFormType());
		return;
	}
	// skip if not enabled at start of game - different checks for Actor and REFR
	if (refr->GetFormType() == RE::FormType::ActorCharacter &&
		(refr->As<RE::Actor>()->formFlags & RE::Actor::RecordFlags::kStartsDead) != RE::Actor::RecordFlags::kStartsDead)
	{
		DBG_VMESSAGE("Actor 0x%08x Base %s/0x%08x does not Start Dead", refr->GetFormID(), refr->GetBaseObject()->GetName(), refr->GetBaseObject()->GetFormID());
		return;
	}
	if ((refr->formFlags & RE::TESObjectREFR::RecordFlags::kInitiallyDisabled) == RE::TESObjectREFR::RecordFlags::kInitiallyDisabled)
	{
		DBG_VMESSAGE("REFR 0x%08x Base %s/0x%08x initially disabled", refr->GetFormID(), refr->GetBaseObject()->GetName(), refr->GetBaseObject()->GetFormID());
		return;
	}
	if (refr->GetBaseObject()->GetFormType() == RE::FormType::Container || refr->GetFormType() == RE::FormType::ActorCharacter)
	{
		if (DataCase::GetInstance()->IsOffLimitsContainer(refr))
		{
			DBG_VMESSAGE("Container REFR %s/0x%08x is off-limits", refr->GetName(), refr->GetFormID());
			return;
		}
		const RE::TESContainer* container(const_cast<RE::TESObjectREFR*>(refr)->GetContainer());
		container->ForEachContainerObject([&](RE::ContainerObject* entry) -> bool {
			auto entryContents(entry->obj);
			if (!SignatureCondition::IsValidFormType(entryContents->GetFormType()))
			{
				DBG_VMESSAGE("Container/NPC %s/0x%08x item %s/0x%08x FormType %d invalid", refr->GetName(), refr->GetFormID(), entryContents->GetName(),
					entryContents->GetFormID(), entryContents->GetFormType());
			}
			else
			{
				DBG_VMESSAGE("Container/NPC %s/0x%08x item %s/0x%08x is a Placed Object", refr->GetName(), refr->GetFormID(), entryContents->GetName(),
					entryContents->GetFormID());
				RecordPlacedItem(entryContents, refr);
			}
			// continue the scan
			return true;
		});
	}
	else
	{
		DBG_VMESSAGE("Loose 0x%08x item %s/0x%08x is a Placed Object", refr->GetFormID(), refr->GetBaseObject()->GetName(),
			refr->GetBaseObject()->GetFormID());
		RecordPlacedItem(refr->GetBaseObject(), refr);
	}
}

void CollectionManager::RecordPlacedObjectsForCell(const RE::TESObjectCELL* cell)
{
	if (!m_checkedForPlacedObjects.insert(cell).second)
		return;

	if (ManagedList::BlackList().Contains(cell))
		return;

	if (!IsCellLocatable(cell))
	{
		// no obvious way to locate - save possibly-reachable doors for the cell
		bool connected(false);
		for (const RE::TESObjectREFRPtr& refptr : cell->references)
		{
			const RE::TESObjectREFR* refr(refptr.get());
			if (refr->GetBaseObject() && refr->GetBaseObject()->GetFormType() == RE::FormType::Door)
			{
				if (refr->extraList.HasType<RE::ExtraTeleport>())
				{
					const auto teleport(refr->extraList.GetByType<RE::ExtraTeleport>());
					const RE::TESObjectREFR* target(teleport->teleportData->linkedDoor.get().get());
					if (!target)
					{
						DBG_VMESSAGE("REFR 0x%08x in CELL %s/0x%08x teleport unusable via RefHandle %d", refr->GetFormID(),
							FormUtils::SafeGetFormEditorID(cell).c_str(), cell->GetFormID(), teleport->teleportData->linkedDoor);
						continue;
					}
					if (!target->parentCell)
					{
						DBG_VMESSAGE("REFR 0x%08x in CELL %s/0x%08x teleport unusable via REFR 0x%08x", refr->GetFormID(), FormUtils::SafeGetFormEditorID(cell).c_str(),
							cell->GetFormID(), target->GetFormID());
						continue;
					}
					if (!IsCellLocatable(target->parentCell))
					{
						DBG_VMESSAGE("REFR 0x%08x in CELL %s/0x%08x teleport unusable via %s/0x%08x", refr->GetFormID(), FormUtils::SafeGetFormEditorID(cell).c_str(),
							cell->GetFormID(), FormUtils::SafeGetFormEditorID(target->parentCell).c_str(), target->parentCell->GetFormID());
						continue;
					}
					DBG_VMESSAGE("REFR 0x%08x in CELL %s/0x%08x teleport connects to CELL %s/0x%08x", refr->GetFormID(), FormUtils::SafeGetFormEditorID(cell).c_str(),
						cell->GetFormID(), FormUtils::SafeGetFormEditorID(target->parentCell).c_str(), target->parentCell->GetFormID());
					connected = true;
					break;
				}
			}
		}
		if (!connected)
			return;
	}

	size_t actors(std::count_if(cell->references.cbegin(), cell->references.cend(),
		[&](const auto refr) -> bool { return refr->GetFormType() == RE::FormType::ActorCharacter; }));
	DBG_MESSAGE("Process %d REFRs including %d actors in CELL %s/0x%08x", cell->references.size(), actors, FormUtils::SafeGetFormEditorID(cell).c_str(), cell->GetFormID());
	for (const RE::TESObjectREFRPtr& refptr : cell->references)
	{
		const RE::TESObjectREFR* refr(refptr.get());
		SaveREFRIfPlaced(refr);
	}
}

// skip if not in a valid worldspace or Location
// TODO might miss some stray CELLs
bool CollectionManager::IsCellLocatable(const RE::TESObjectCELL* cell)
{
	const RE::ExtraLocation* extraLocation(cell->extraList.GetByType<RE::ExtraLocation>());
	if (extraLocation && extraLocation->location)
	{
		DBG_VMESSAGE("CELL %s/0x%08x is in Location %s/0x%08x", FormUtils::SafeGetFormEditorID(cell).c_str(), cell->GetFormID(),
			extraLocation->location->GetName(), extraLocation->location->GetFormID());
		return true;
	}
	else if (cell->worldSpace)
	{
		DBG_VMESSAGE("CELL %s/0x%08x is in WorldSpace %s", FormUtils::SafeGetFormEditorID(cell).c_str(), cell->GetFormID(), cell->worldSpace->GetName());
		return true;
	}
	DBG_VMESSAGE("CELL %s/0x%08x unlocatable", FormUtils::SafeGetFormEditorID(cell).c_str(), cell->GetFormID());
	return false;
}

void CollectionManager::RecordPlacedObjects(void)
{
#ifdef _PROFILING
	WindowsUtils::ScopedTimer elapsed("Record Placed Objects");
#endif

	// list all placed objects of interest for Collections - don't quest for anything we cannot see
	for (const auto worldSpace : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESWorldSpace>())
	{
		DBG_MESSAGE("Process %d CELLs in WorldSpace Map for %s/0x%08x", worldSpace->cellMap.size(), worldSpace->GetName(), worldSpace->GetFormID());
		for (const auto cellEntry : worldSpace->cellMap)
		{
			RecordPlacedObjectsForCell(cellEntry.second);
		}
	}
	DBG_MESSAGE("Process %d Interior CELLs", RE::TESDataHandler::GetSingleton()->interiorCells.size());
	for (const auto cell : RE::TESDataHandler::GetSingleton()->interiorCells)
	{
		RecordPlacedObjectsForCell(cell);
	}
	REL_MESSAGE("%d Placed Objects recorded for %d Items", m_placedItems.size(), m_placedObjects.size());
}

bool CollectionManager::IsPlacedObject(const RE::TESForm* form) const
{
	RecursiveLockGuard guard(m_collectionLock);
	return m_placedObjects.contains(form);
}

void CollectionManager::ResolveMembership(void)
{
#ifdef _PROFILING
	WindowsUtils::ScopedTimer elapsed("Resolve Collection Membership");
#endif
	std::unordered_set<RE::TESForm*> uniquePlaced;
	std::unordered_set<RE::TESForm*> uniqueMembers;
	for (const auto& signature : SignatureCondition::ValidSignatures())
	{
		for (const auto form : RE::TESDataHandler::GetSingleton()->GetFormArray(signature.second))
		{
			for (const auto& collection : m_allCollectionsByLabel)
			{
				// record collection membership for any that match this object - ignore whitelist
				ConditionMatcher matcher(form);
				if (collection.second->MatchesFilter(matcher))
				{
					// Any condition on this collection that has a scope has aggregated the valid scopes in the matcher
					collection.second->SetScopes(matcher.ScopesSeen());

					DBG_VMESSAGE("Record %s/0x%08x as collectible", form->GetName(), form->GetFormID());
					m_collectionsByFormID.insert(std::make_pair(form->GetFormID(), collection.second));
					collection.second->AddMemberID(form);
					if (CollectionManager::Instance().IsPlacedObject(form))
					{
						uniquePlaced.insert(form);
					}
					uniqueMembers.insert(form);
				}
			}
		}
	}
	REL_MESSAGE("Collections contain %d unique objects, %d of which are placed in the world via %d REFRs",
		uniqueMembers.size(), uniquePlaced.size(), m_placedObjects.size());

	PrintMembership();
}

// for game reload, we reset the checked items
// TODO process SKSE co-save data
void CollectionManager::OnGameReload()
{
	RecursiveLockGuard guard(m_collectionLock);
	/// reset player inventory last-known-good
	m_lastInventoryItems.clear();

	// logic depends on prior and new state
	bool wasEnabled(m_enabled);
	m_enabled = INIFile::GetInstance()->GetSetting(INIFile::PrimaryType::common, INIFile::SecondaryType::config, "CollectionsEnabled") != 0.;
	REL_MESSAGE("Collections are %s", m_enabled ? "enabled" : "disabled");
	if (m_enabled)
	{
		// TODO load Collections data from saved game
		// Flush membership state to allow testing
		for (auto collection : m_allCollectionsByLabel)
		{
			collection.second->Reset();
		}
	}
	else
	{
		// TODO maybe more state to clean out
	}
}

}

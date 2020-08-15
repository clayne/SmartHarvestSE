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
#pragma once

#include "PluginFacade.h"

#include "Looting/containerLister.h"
#include "Looting/IRangeChecker.h"
#include "VM/EventPublisher.h"
#include "VM/UIState.h"

#include <mutex>


namespace shse
{
struct pair_hash
{
	template <class T1, class T2>
	std::size_t operator() (const std::pair<T1, T2>& pair) const
	{
		return std::hash<T1>()(pair.first) ^ std::hash<T2>()(pair.second);
	}
};

class ScanGovernor
{
public:
	static ScanGovernor& Instance();
	ScanGovernor();
#if _DEBUG
	// make sure load spike handling works OK
	static constexpr size_t MaxREFRSPerPass = 25;
#else
	static constexpr size_t MaxREFRSPerPass = 75;
#endif

	void Clear();

	static constexpr int HarvestSpamLimit = 10;
	size_t PendingHarvestNotifications() const;
	bool LockHarvest(const RE::TESObjectREFR* refr, const bool isSilent);
	bool IsLockedForHarvest(const RE::TESObjectREFR* refr) const;
	bool UnlockHarvest(const RE::TESObjectREFR* refr, const bool isSilent);

	void ToggleCalibration(const bool glowDemo);
	void InvokeLootSense(void);
	void DisplayLootability(RE::TESObjectREFR* refr);

	void Allow();
	void Disallow();
	bool IsAllowed() const;
	void DoPeriodicSearch(const ReferenceScanType scanType);
	const RE::Actor* ActorByIndex(const int actorIndex) const;
	inline bool Calibrating() const {
		RecursiveLockGuard guard(m_searchLock);
		return m_calibrating;
	}

	RE::FormID LootedDynamicREFRFormID(const RE::TESObjectREFR* refr) const;
	void MarkContainerLooted(const RE::TESObjectREFR* refr);
	bool IsLootedContainer(const RE::TESObjectREFR* refr) const;
	bool IsReferenceLockedContainer(const RE::TESObjectREFR* refr) const;
	bool HasDynamicData(RE::TESObjectREFR* refr) const;
	void ResetLootedDynamicREFRs();
	void ResetLootedContainers();
	void ForgetLockedContainers();
	void ClearPendingHarvestNotifications();
	void GlowObject(RE::TESObjectREFR* refr, const int duration, const GlowReason glowReason);
	void ClearGlowExpiration();

	void SetSPERGKeyword(const RE::BGSKeyword* keyword);
	void SPERGStoreInitial();
	void SPERGCheckNew();

private:
	void ProgressGlowDemo();
	void LootAllEligible();
	void TrackActors();

	Lootability ValidateTarget(RE::TESObjectREFR*& refr, std::vector<RE::TESObjectREFR*>& possibleDupes, const bool dryRun, const bool glowOnly);
	void MarkDynamicREFRLooted(const RE::TESObjectREFR* refr) const;

	void RegisterActorTimeOfDeath(RE::TESObjectREFR* refr);

	static std::unique_ptr<ScanGovernor> m_instance;

	std::unordered_set<const RE::TESObjectREFR*> m_HarvestLock;
	int m_pendingNotifies;

	bool m_searchAllowed;
	INIFile::SecondaryType m_targetType;

	// for dry run - ordered by proximity to player at time of recording
	std::vector<const RE::Actor*> m_detectiveWannabes;

	mutable RecursiveLock m_searchLock;
	std::unordered_map<const RE::TESObjectREFR*, std::chrono::time_point<std::chrono::high_resolution_clock>> m_glowExpiration;

	// Record looted REFRs to avoid re-scan of empty or looted chest and dead body.
	// Dynamic REFRs - reset on cell change - includes REFR and BaseObject FormIDs to make this less likely to silently malfunction
	mutable std::unordered_set<std::pair<RE::FormID, RE::FormID>, pair_hash> m_lootedDynamicREFRs;
	// Non-dynamic - reset on game reload or MCM settings update.
	std::unordered_set<const RE::TESObjectREFR*> m_lootedContainers;

	// BlackList for Locked Containers. Never auto-loot unless config permits. Reset on game reload.
	mutable std::unordered_map<const RE::TESObjectREFR*, size_t> m_lockedContainers;

	std::vector<const RE::BGSKeyword*> m_spergKeywords;
	std::unique_ptr<ContainerLister> m_spergInventory;
	// handle concurrent ore vein mining by reconciling versus initial inventory snapshot after the last completes
	size_t m_spergQueued;

	// Loot Range calibration setting
	bool m_calibrating;
	int m_calibrateRadius;
	int m_calibrateDelta;
	bool m_glowDemo;
	GlowReason m_nextGlow;
	static constexpr int CalibrationRangeDelta = 3;
	static constexpr int MaxCalibrationRange = 100;
	static constexpr int GlowDemoRange = 30;

	// short glow for loot range calibration and glow demo
	static constexpr int ObjectGlowDurationCalibrationSeconds = int(PluginFacade::CalibrationThreadDelaySeconds) - 2;
};

}
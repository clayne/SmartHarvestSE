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

#include "FormHelpers/IHasValueWeight.h"
#include "Data/iniSettings.h"

namespace shse
{

class LootableREFR : public IHasValueWeight
{
public:
	explicit LootableREFR(const RE::TESObjectREFR* ref, const INIFile::SecondaryType scope);

	SInt16 GetItemCount();
	RE::NiTimeController* GetTimeController(void) const;
	bool IsQuestItem(const bool requireFullQuestFlags);
	std::pair<bool, SpecialObjectHandling> TreatAsCollectible(void) const;
	bool IsValuable(void) const;

	RE::TESForm* GetLootable() const;
	void SetLootable(RE::TESForm* lootable);
	virtual double GetWeight(void) const override;
	inline const RE::TESObjectREFR* GetReference() const { return m_ref; }
	inline INIFile::SecondaryType Scope() const { return m_scope; }

protected:
	virtual const char* GetName() const override;
	virtual UInt32 GetFormID() const override;
	virtual SInt32 CalculateWorth(void) const override;

private:
	const RE::TESObjectREFR* m_ref;
	const INIFile::SecondaryType m_scope;
	RE::TESForm* m_lootable;
};

}
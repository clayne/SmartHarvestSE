#pragma once

#include "CommonLibSSE/include/RE/BGSKeywordForm.h"
#include "CommonLibSSE/include/RE/EnchantmentItem.h"

class TESFormHelper : public IHasValueWeight
{
public:
	TESFormHelper(RE::TESForm* form);

	RE::BGSKeywordForm* GetKeywordForm(void) const;
	RE::EnchantmentItem* GetEnchantment(void);
	UInt32 GetGoldValue(void) const;

	virtual double GetWeight(void) const override;
	virtual double GetWorth(void) const override;

	RE::TESForm* m_form;

protected:
	virtual const char* GetName() const;
	virtual UInt32 GetFormID() const;
};

bool IsPlayable(const RE::TESForm* pForm);

template <typename FORM> RE::BGSKeywordForm* KeywordFormCast(const RE::TESForm* form)
{
	FORM* waypoint(form->As<FORM>());
	if (waypoint)
		return waypoint->As<RE::BGSKeywordForm>();
	return nullptr;
}
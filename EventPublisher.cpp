#include "PrecompiledHeaders.h"
#include "EventPublisher.h"

std::unique_ptr<EventPublisher> EventPublisher::m_instance;

EventPublisher& EventPublisher::Instance()
{
	if (!m_instance)
	{
		m_instance = std::make_unique<EventPublisher>();
	}
	return *m_instance;
}

EventPublisher::EventPublisher() : m_eventTarget(nullptr),
	m_onGetCritterIngredient("OnGetCritterIngredient"),
	m_onCarryWeightDelta("OnCarryWeightDelta"),
	m_onResetCarryWeight("OnResetCarryWeight"),
	m_onHarvest("OnHarvest"),
	m_onMining("OnMining"),
	m_onLootFromNPC("OnLootFromNPC"),
	m_onFlushAddedItems("OnFlushAddedItems"),
	m_onObjectGlow("OnObjectGlow")
{
}

RE::BGSRefAlias* EventPublisher::GetScriptTarget(const char* espName, RE::FormID questID)
{
	static RE::TESQuest* quest(nullptr);
	static RE::BGSRefAlias* alias(nullptr);
	if (!quest)
	{
		RE::FormID formID = InvalidForm;
		std::optional<UInt8> idx = RE::TESDataHandler::GetSingleton()->GetLoadedModIndex(espName);
		if (idx.has_value())
		{
			formID = (idx.value() << 24) | questID;
			DBG_MESSAGE("Got formID for questID %08.2x", questID);
		}
		if (formID != InvalidForm)
		{
			RE::TESForm* questForm = RE::TESForm::LookupByID(formID);
			DBG_MESSAGE("Got Base Form %s", questForm ? questForm->GetFormEditorID() : "nullptr");
			quest = questForm ? questForm->As<RE::TESQuest>() : nullptr;
			DBG_MESSAGE("Got Quest Form %s", quest ? quest->GetFormEditorID() : "nullptr");
		}
	}
	if (quest && quest->IsRunning())
	{
		DBG_MESSAGE("Quest %s is running", quest->GetFormEditorID());
		RE::BGSBaseAlias* baseAlias(quest->aliases[0]);
		if (!baseAlias)
		{
			DBG_MESSAGE("Quest has no alias at index 0");
			return nullptr;
		}

		alias = static_cast<RE::BGSRefAlias*>(baseAlias);
		if (!alias)
		{
			REL_WARNING("Quest is not type BGSRefAlias");
			return nullptr;
		}
		DBG_MESSAGE("Got BGSRefAlias for Mod's Quest");
	}
	return alias;
}

bool EventPublisher::GoodToGo()
{
	if (!m_eventTarget)
	{
		m_eventTarget = GetScriptTarget(MODNAME, QuestAliasFormID);
		// register the events
		if (m_eventTarget)
		{
			HookUp();
		}
	}
	return m_eventTarget != nullptr;
}

void EventPublisher::HookUp()
{
	m_onGetCritterIngredient.Register(m_eventTarget);
	m_onCarryWeightDelta.Register(m_eventTarget);
	m_onResetCarryWeight.Register(m_eventTarget);
	m_onObjectGlow.Register(m_eventTarget);
	m_onHarvest.Register(m_eventTarget);
	m_onMining.Register(m_eventTarget);
	m_onLootFromNPC.Register(m_eventTarget);
	m_onFlushAddedItems.Register(m_eventTarget);
}

void EventPublisher::TriggerGetCritterIngredient(RE::TESObjectREFR* refr)
{
	m_onGetCritterIngredient.SendEvent(refr);
}

void EventPublisher::TriggerCarryWeightDelta(const int delta)
{
	m_onCarryWeightDelta.SendEvent(delta);
}

void EventPublisher::TriggerResetCarryWeight()
{
	m_onResetCarryWeight.SendEvent();
}

void EventPublisher::TriggerMining(RE::TESObjectREFR* refr, const ResourceType resourceType, const bool manualLootNotify)
{
	// We always block the REFR before firing this
	m_onMining.SendEvent(refr, static_cast<int>(resourceType), manualLootNotify);
}

void EventPublisher::TriggerHarvest(RE::TESObjectREFR* refr, const ObjectType objType, int itemCount, const bool isSilent, const bool manualLootNotify)
{
	// We always lock the REFR from more harvesting before firing this
	m_onHarvest.SendEvent(refr, static_cast<int>(objType), itemCount, isSilent, manualLootNotify);
}

void EventPublisher::TriggerFlushAddedItems()
{
	m_onFlushAddedItems.SendEvent();
}

void EventPublisher::TriggerLootFromNPC(RE::TESObjectREFR* npc, RE::TESForm* item, int itemCount, ObjectType objectType)
{
	m_onLootFromNPC.SendEvent(npc, item, itemCount, static_cast<int>(objectType));
}

void EventPublisher::TriggerObjectGlow(RE::TESObjectREFR* refr, const int duration, const GlowReason glowReason)
{
	m_onObjectGlow.SendEvent(refr, duration, static_cast<int>(glowReason));
}
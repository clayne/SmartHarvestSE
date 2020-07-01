#include "PrecompiledHeaders.h"

#include "Collections/Collection.h"
#include "Utilities/utils.h"
#include "VM/papyrus.h"
#include "WorldState/PlayerState.h"

namespace shse
{

void CollectionPolicy::AsJSON(nlohmann::json& j) const
{
	j["action"] = SpecialObjectHandlingJSON(m_action);
	j["notify"] = m_notify;
	j["repeat"] = m_repeat;
}

void to_json(nlohmann::json& j, const CollectionPolicy& policy)
{
	policy.AsJSON(j);
}

Collection::Collection(const std::string& name, const std::string& description, const CollectionPolicy& policy, std::unique_ptr<ConditionTree> filter) :
	m_name(name), m_description(description), m_policy(policy), m_rootFilter(std::move(filter))
{
}

bool Collection::AddMemberID(const RE::TESForm* form)const 
{
	if (form && m_members.insert(form->GetFormID()).second)
	{
		return true;
	}
	return false;
}

bool Collection::IsMemberOf(const RE::TESForm* form) const
{
	// Check static list of IDs
	return form && m_members.contains(form->GetFormID());
}

bool Collection::InScopeAndCollectibleFor(const ConditionMatcher& matcher) const
{
	if (!matcher.Form())
		return false;

	// check Scope - if Collection is scoped, scope for this autoloot check must be valid
	if (!m_scopes.empty() && std::find(m_scopes.cbegin(), m_scopes.cend(), matcher.Scope()) == m_scopes.cend())
	{
		DBG_VMESSAGE("%s/0x%08x has invalid scope %d", matcher.Form()->GetName(), matcher.Form()->GetFormID(), int(matcher.Scope()));
		return false;
	}

	// if (always collectible OR not observed) AND a member of this collection
	return (m_policy.Repeat() || !m_observed.contains(matcher.Form()->GetFormID())) && IsMemberOf(matcher.Form());
}

bool Collection::MatchesFilter(const ConditionMatcher& matcher) const
{
	if (matcher.Form() && m_rootFilter->operator()(matcher))
	{
		AddMemberID(matcher.Form());
		return true;
	}
	return false;
}

void Collection::RecordItem(const RE::FormID itemID, const RE::TESForm* form, const float gameTime, const RE::TESForm* place)
{
	DBG_VMESSAGE("Collect %s/0x%08x in %s", form->GetName(), form->GetFormID(), m_name.c_str());
	if (m_observed.insert(
		std::make_pair(itemID, CollectionEntry(form, gameTime, place, PlayerState::Instance().GetPosition()))).second)
	{
		if (m_policy.Notify())
		{
			// notify about these, just once
			std::string notificationText;
			static RE::BSFixedString newMemberText(papyrus::GetTranslation(nullptr, RE::BSFixedString("$SHSE_ADDED_TO_COLLECTION")));
			if (!newMemberText.empty())
			{
				notificationText = newMemberText;
				StringUtils::Replace(notificationText, "{ITEMNAME}", form->GetName());
				StringUtils::Replace(notificationText, "{COLLECTION}", m_name);
				if (!notificationText.empty())
				{
					RE::DebugNotification(notificationText.c_str());
				}
			}
		}
	}
}

void Collection::Reset()
{
	m_observed.clear();
}

std::string Collection::Name(void) const
{
	return m_name;
}

std::string Collection::PrintDefinition() const
{
	std::ostringstream collectionStr;
	collectionStr << *this;
	return collectionStr.str();
}

std::string Collection::PrintMembers(void) const
{
	std::ostringstream collectionStr;
	collectionStr << m_members.size() << " members\n";
	if (!m_scopes.empty())
	{
		collectionStr << "Scope: ";
		size_t scopes(m_scopes.size());
		for (const auto scope : m_scopes)
		{
			std::string scopeStr;
			INIFile::GetInstance()->GetIsSecondaryTypeString(scope, scopeStr);
			collectionStr << scopeStr;
			if (--scopes)
			{
				collectionStr << ', ';
			}
			else
			{
				collectionStr << '\n';
			}
		}
	}
	for (const auto member : m_members)
	{
		collectionStr << "  0x" << std::hex << std::setw(8) << std::setfill('0') << member;
		RE::TESForm* form(RE::TESForm::LookupByID(member));
		if (form)
		{
			collectionStr << ":" << form->GetName();
		}
		collectionStr << '\n';
	}
	return collectionStr.str();
}

nlohmann::json Collection::MakeJSON() const
{
	return nlohmann::json(*this);
}

void Collection::AsJSON(nlohmann::json& j) const
{
	j["name"] = m_name;
	j["description"] = m_description;
	j["policy"] = nlohmann::json(m_policy);
	j["rootFilter"] = nlohmann::json(*m_rootFilter);
}

void to_json(nlohmann::json& j, const Collection& collection)
{
	collection.AsJSON(j);
}

}

std::ostream& operator<<(std::ostream& os, const shse::Collection& collection)
{
	os << collection.MakeJSON().dump(2);
	return os;
}

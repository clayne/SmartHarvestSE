#include "PrecompiledHeaders.h"

namespace shse
{

Collection::Collection(const std::string& name, const std::string& description, std::unique_ptr<ConditionTree> filter) :
	m_name(name), m_description(description), m_rootFilter(std::move(filter))
{
}

bool Collection::IsMemberOf(const RE::TESForm* form, const ObjectType objectType) const
{
	return m_rootFilter->operator()(form, objectType);
}

void Collection::RecordNewMember(const RE::FormID itemID, const RE::TESForm* form)
{
	if (m_members.insert(std::make_pair(itemID, form)).second)
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

nlohmann::json Collection::MakeJSON() const
{
	return nlohmann::json(*this);
}

void Collection::AsJSON(nlohmann::json& j) const
{
	j["name"] = m_name;
	j["description"] = m_description;
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

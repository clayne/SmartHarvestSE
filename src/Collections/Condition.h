#pragma once

#include "Data/iniSettings.h"

namespace shse {

	class ConditionMatcher;

	bool CanBeCollected(RE::TESForm* form);

	class Condition {
	public:
		virtual ~Condition();

		virtual bool operator()(const ConditionMatcher& matcher) const = 0;
		virtual nlohmann::json MakeJSON() const = 0;
		virtual void AsJSON(nlohmann::json& j) const = 0;
	protected:
		Condition();
	};

	class PluginCondition : public Condition {
	public:
		PluginCondition(const std::vector<std::string>& plugins);
		virtual bool operator()(const ConditionMatcher& matcher) const;
		virtual nlohmann::json MakeJSON() const;
		virtual void AsJSON(nlohmann::json& j) const override;

	private:
		std::unordered_map<std::string, RE::FormID> m_formIDMaskByPlugin;
	};

	class FormListCondition : public Condition {
	public:
		FormListCondition(const std::string& plugin, const std::string& formListID);
		virtual bool operator()(const ConditionMatcher& matcher) const;
		virtual nlohmann::json MakeJSON() const;
		virtual void AsJSON(nlohmann::json& j) const override;

	private:
		void FlattenMembers(const RE::BGSListForm* formList);
		std::string m_plugin;
		RE::BGSListForm* m_formList;
		std::unordered_set<const RE::TESForm*> m_listMembers;
	};

	class KeywordCondition : public Condition {
	public:
		KeywordCondition(const std::vector<std::string>& keywords);
		virtual bool operator()(const ConditionMatcher& matcher) const;
		virtual nlohmann::json MakeJSON() const;
		virtual void AsJSON(nlohmann::json& j) const override;

	private:
		std::unordered_set<const RE::BGSKeyword*> m_keywords;
	};

	class SignatureCondition : public Condition {
	private:
		static const std::unordered_map<std::string, RE::FormType> m_validSignatures;
		std::vector<RE::FormType> m_formTypes;

	public:
		// Store Form Types to match for this collection. Schema enforces uniqueness and validity in input list.
		// The list below must match the JSON schema and CommonLibSSE RE::FormType.

		SignatureCondition(const std::vector<std::string>& signatures);
		virtual bool operator()(const ConditionMatcher& matcher) const;
		static const decltype(m_validSignatures) ValidSignatures();
		virtual nlohmann::json MakeJSON() const;
		virtual void AsJSON(nlohmann::json& j) const override;
	};

	class ScopeCondition : public Condition {
	public:
		ScopeCondition(const std::vector<std::string>& scopes);
		virtual bool operator()(const ConditionMatcher& matcher) const;
		virtual nlohmann::json MakeJSON() const;
		virtual void AsJSON(nlohmann::json& j) const override;

	private:
		std::vector<INIFile::SecondaryType> m_scopes;
		static const std::unordered_map<std::string, INIFile::SecondaryType> m_validScopes;
	};

	class ConditionTree : public Condition {
	public:
		enum class Operator {
			And,
			Or
		};

		ConditionTree(const Operator op, const unsigned int depth);
		virtual bool operator()(const ConditionMatcher& matcher) const;
		void AddCondition(std::unique_ptr<Condition> condition);
		virtual nlohmann::json MakeJSON() const;
		virtual void AsJSON(nlohmann::json& j) const override;

	private:
		std::vector<std::unique_ptr<Condition>> m_conditions;
		Operator m_operator;
		unsigned int m_depth;
	};

	void to_json(nlohmann::json& j, const PluginCondition& p);
	void to_json(nlohmann::json& j, const FormListCondition& p);
	void to_json(nlohmann::json& j, const KeywordCondition& p);
	void to_json(nlohmann::json& j, const SignatureCondition& p);
	void to_json(nlohmann::json& j, const ScopeCondition& p);
	void to_json(nlohmann::json& j, const ConditionTree& p);

	class ConditionMatcher {
	public:
		ConditionMatcher(const RE::TESForm* form);
		ConditionMatcher(const RE::TESForm* form, const INIFile::SecondaryType scope);
		inline const RE::TESForm* Form() const { return m_form; }
		inline INIFile::SecondaryType Scope() const { return m_scope; }
		void AddScope(const INIFile::SecondaryType scope) const;
		inline const std::vector<INIFile::SecondaryType>& ScopesSeen() const { return m_scopesSeen; }

	private:
		const RE::TESForm* m_form;
		// INIFile::SecondaryType::NONE2 is a sentinel to indicate no filtering on scope, during game-data load
		const INIFile::SecondaryType m_scope;
		mutable std::vector<INIFile::SecondaryType> m_scopesSeen;
	};
}

std::ostream& operator<<(std::ostream& os, const shse::Condition& condition);

#include "nlohmann/json-schema.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <regex>

void ParsePlugin(const nlohmann::json& pluginRule)
{
	std::vector<std::string> plugins;
	plugins.reserve(pluginRule.size());
	std::transform(pluginRule.begin(), pluginRule.end(), std::back_inserter(plugins),
		[&](const nlohmann::json& next) { return next.get<std::string>(); });
}

void ParseFormList(const nlohmann::json& formListRule)
{
	std::vector<std::pair<std::string, std::string>> formLists;
	formLists.reserve(formListRule.size());
	std::transform(formListRule.begin(), formListRule.end(), std::back_inserter(formLists),
		[&](const nlohmann::json& next)
	{ 
		return std::make_pair(next["listPlugin"].get<std::string>(), next["formID"].get<std::string>());
	});
}

void ParseForms(const nlohmann::json& formsRule)
{
	std::vector<std::pair<std::string, std::vector<std::string>>> forms;
	forms.reserve(formsRule.size());
	std::transform(formsRule.begin(), formsRule.end(), std::back_inserter(forms),
		[&](const nlohmann::json& next)
	{
		std::vector<std::string> formIDs;
		formIDs.reserve(next["form"].size());
		for (const std::string& form : next["form"])
		{
			formIDs.push_back(form);
		}
		return std::make_pair(next["plugin"].get<std::string>(), formIDs);
	});
}

void ParseKeyword(const nlohmann::json& keywordRule)
{
	std::vector<std::string> keywords;
	keywords.reserve(keywordRule.size());
	std::transform(keywordRule.begin(), keywordRule.end(), std::back_inserter(keywords),
		[&](const nlohmann::json& next) { return next.get<std::string>(); });
}

void ParseSignature(const nlohmann::json& signatureRule)
{
	std::vector<std::string> signatures;
	signatures.reserve(signatureRule.size());
	std::transform(signatureRule.begin(), signatureRule.end(), std::back_inserter(signatures),
		[&](const nlohmann::json& next) { return next.get<std::string>(); });
}

void ParseScope(const nlohmann::json& scopeRule)
{
	std::vector<std::string> scopes;
	scopes.reserve(scopeRule.size());
	std::transform(scopeRule.begin(), scopeRule.end(), std::back_inserter(scopes),
		[&](const nlohmann::json& next) { return next.get<std::string>(); });
}

void ParseLootCategory(const nlohmann::json& lootCategoryRule)
{
	std::vector<std::string> lootCategories;
	lootCategories.reserve(lootCategoryRule.size());
	std::transform(lootCategoryRule.begin(), lootCategoryRule.end(), std::back_inserter(lootCategories),
		[&](const nlohmann::json& next) { return next.get<std::string>(); });
}

void ParsePolicy(const nlohmann::json& policy)
{
	std::string action(policy["action"].get<std::string>());
	bool notify(policy["notify"].get<bool>());
	bool repeat(policy["repeat"].get<bool>());
}

void ParseFilter(const nlohmann::json& filter)
{
	std::string op(filter["operator"].get<std::string>());
	for (const auto& condition : filter["condition"].items())
	{
		if (condition.key() == "subFilter")
		{
			for (const auto& subFilter : condition.value())
			{
				ParseFilter(subFilter);
			}
		}
		else if (condition.key() == "plugin")
		{
			ParsePlugin(condition.value());
		}
		else if (condition.key() == "formList")
		{
			ParseFormList(condition.value());
		}
		else if (condition.key() == "forms")
		{
			ParseForms(condition.value());
		}
		else if (condition.key() == "keyword")
		{
			ParseKeyword(condition.value());
		}
		else if (condition.key() == "signature")
		{
			ParseSignature(condition.value());
		}
		else if (condition.key() == "scope")
		{
			ParseScope(condition.value());
		}
	}
}

void ParseCollection(const nlohmann::json& collection)
{
	if (collection.find("policy") != collection.cend())
 		ParsePolicy(collection["policy"]);
	ParseFilter(collection["rootFilter"]);
}

void ParseCollectionGroup(const nlohmann::json& collectionGroup)
{
	ParsePolicy(collectionGroup["groupPolicy"]);
	bool useMCM(collectionGroup["useMCM"].get<bool>());
	for (const auto& collection : collectionGroup["collections"])
	{
		ParseCollection(collection);
	}
}

void CheckCollections()
{
	// Validate the schema
	const std::string schemaFileName("SHSE.SchemaCollections.json");
	std::ifstream schemaFile(schemaFileName);
	if (schemaFile.fail())
	{
		std::cerr << "JSON Collections Schema " << schemaFileName << " cannot be opened\n";
		return;
	}
	nlohmann::json_schema::json_validator validator;
	try {
		nlohmann::json schema(nlohmann::json::parse(schemaFile));
		validator.set_root_schema(schema); // insert root-schema
	}
	catch (const std::exception& e) {
		std::cerr << "JSON Collections Schema " << schemaFileName << " validation error\n" << e.what() << '\n';
		return;
	}
	std::cout << "JSON Collections Schema " << schemaFileName << " parsed and validated\n";

	// Find and Load Collection Definitions using the validated schema
	try {
		const std::regex collectionsFilePattern(".*\\SHSE.Collections\\..*\\.json$");
		for (const auto& nextFile : std::filesystem::directory_iterator("."))
		{
			std::string collectionFileName(nextFile.path().generic_string());
			if (!std::filesystem::is_regular_file(nextFile))
			{
				std::cout << "Skip " << collectionFileName << ", not a regular file\n";
				continue;
			}
			if (!std::regex_match(collectionFileName, collectionsFilePattern))
			{
				std::cout << "Skip " << collectionFileName << ", does not match Collections filename pattern\n";
				continue;
			}
			std::ifstream collectionFile(collectionFileName);
			nlohmann::json collectionGroup;
			try {
				collectionGroup = nlohmann::json::parse(collectionFile);
				validator.validate(collectionGroup);
			}
			catch (const std::exception& e) {
				std::cerr << "JSON Collections " << collectionFileName << " validation error\n" << e.what() << '\n';
				continue;
			}
			std::cout << "JSON Collections " << collectionFileName << " parsed and validated\n";

			// walk the tree
			ParseCollectionGroup(collectionGroup);
			std::cout << "JSON Collections " << collectionFileName << " walked OK\n";
		}
	}
	catch (const std::exception& e) {
		std::cerr << "JSON Collections Processing Error\n" << e.what() << '\n';
	}
}

void CheckFilters()
{
	// Validate the schema
	const std::string schemaFileName("SHSE.SchemaFilters.json");
	std::ifstream schemaFile(schemaFileName);
	if (schemaFile.fail())
	{
		std::cerr << "JSON Filters Schema " << schemaFileName << " cannot be opened\n";
		return;
	}
	nlohmann::json_schema::json_validator validator;
	try {
		nlohmann::json schema(nlohmann::json::parse(schemaFile));
		validator.set_root_schema(schema); // insert root-schema
	}
	catch (const std::exception& e) {
		std::cerr << "JSON Filters Schema " << schemaFileName << " validation error\n" << e.what() << '\n';
		return;
	}
	std::cout << "JSON Filters Schema " << schemaFileName << " parsed and validated\n";

	// Find and Load Filter Definitions using the validated schema
	try {
		const std::regex filtersFilePattern(".*\\SHSE.Filter\\..*\\.json$");
		for (const auto& nextFile : std::filesystem::directory_iterator("."))
		{
			std::string filterFileName(nextFile.path().generic_string());
			if (!std::filesystem::is_regular_file(nextFile))
			{
				std::cout << "Skip " << filterFileName << ", not a regular file\n";
				continue;
			}
			if (!std::regex_match(filterFileName, filtersFilePattern))
			{
				std::cout << "Skip " << filterFileName << ", does not match Filters filename pattern\n";
				continue;
			}
			std::ifstream filterFile(filterFileName);
			nlohmann::json filter;
			try {
				filter = nlohmann::json::parse(filterFile);
				validator.validate(filter);
			}
			catch (const std::exception& e) {
				std::cerr << "JSON Filters " << filterFileName << " validation error\n" << e.what() << '\n';
				continue;
			}
			std::cout << "JSON Filters " << filterFileName << " parsed and validated\n";
		}
	}
	catch (const std::exception& e) {
		std::cerr << "JSON Filters Processing Error\n" << e.what() << '\n';
	}
}

int main(int argc, const char** argv)
{
	CheckCollections();
	CheckFilters();

	return 0;
}
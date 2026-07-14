// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include "nosUtilities/ListDirectory_generated.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <regex>

namespace nos::utilities
{
NOS_REGISTER_NAME(Directory);
NOS_REGISTER_NAME(Pattern);
NOS_REGISTER_NAME(SortBy);
NOS_REGISTER_NAME(SortDescending);
NOS_REGISTER_NAME(Paths);
NOS_REGISTER_NAME(Count);
NOS_REGISTER_NAME(string);

// Translates a glob pattern (* and ? wildcards) into an ECMAScript regex that matches it in full.
std::regex GlobToRegex(std::string const& glob)
{
	std::string regex = "^";
	for (char c : glob)
	{
		switch (c)
		{
		case '*': regex += ".*"; break;
		case '?': regex += '.'; break;
		case '.': case '^': case '$': case '|': case '(': case ')': case '[': case ']':
		case '{': case '}': case '+': case '\\':
			regex += '\\'; regex += c; break;
		default: regex += c; break;
		}
	}
	regex += '$';
	return std::regex(regex);
}

// Lists the files directly inside Directory whenever it changes.
struct ListDirectoryNode : NodeContext
{
	using NodeContext::NodeContext;

	nosResult ExecuteNode(NodeExecuteParams const& execParams) override
	{
		const char* directory = execParams.GetPinValue<const char>(NSN_Directory);
		const char* pattern = execParams.GetPinValue<const char>(NSN_Pattern);

		std::optional<std::regex> re;
		bool patternValid = true;
		if (pattern && *pattern)
		{
			try
			{
				re = GlobToRegex(pattern);
			}
			catch (std::regex_error const& e)
			{
				patternValid = false;
				SetNodeStatusMessage(std::string("Invalid Pattern: ") + e.what(), fb::NodeStatusMessageType::FAILURE);
			}
		}
		if (patternValid)
			ClearNodeStatusMessages();

		auto sortBy = *execParams.GetPinValue<ListDirectorySortBy>(NSN_SortBy);
		bool descending = *execParams.GetPinValue<bool>(NSN_SortDescending);

		std::vector<std::filesystem::directory_entry> entries;
		std::error_code ec;
		if (patternValid && std::filesystem::is_directory(directory, ec))
			for (auto const& entry : std::filesystem::directory_iterator(directory, ec))
				if (entry.is_regular_file(ec) && (!re || std::regex_match(entry.path().filename().string(), *re)))
					entries.push_back(entry);

		auto isLess = [sortBy](std::filesystem::directory_entry const& a, std::filesystem::directory_entry const& b) {
			switch (sortBy)
			{
			case ListDirectorySortBy::ModifiedTime: return a.last_write_time() < b.last_write_time();
			case ListDirectorySortBy::Size: return a.file_size() < b.file_size();
			default: return a.path().filename() < b.path().filename();
			}
		};
		std::sort(entries.begin(), entries.end(), [&isLess, descending](auto const& a, auto const& b) {
			return descending ? isLess(b, a) : isLess(a, b);
		});

		std::vector<std::string> paths;
		for (auto const& entry : entries)
			paths.push_back(nos::PathToUtf8(entry.path()));

		SetPinValue(NSN_Count, nos::Buffer::From((uint32_t)paths.size()));

		// Paths is an array pin: build one string object per path and wrap them in an
		// array object. Element refs are held until CreateArrayObject has taken its own.
		std::vector<PrimitiveObjectRef> elementRefs;
		std::vector<nosObjectId> elementIds;
		elementRefs.reserve(paths.size());
		elementIds.reserve(paths.size());
		for (auto const& path : paths)
		{
			auto element = PrimitiveObjectRef::Create(NSN_string, nos::Buffer(path.c_str(), path.size() + 1));
			if (!element)
				return NOS_RESULT_FAILED;
			elementIds.push_back(element->GetObjectId());
			elementRefs.push_back(std::move(*element));
		}

		auto it = execParams.find(NSN_Paths);
		if (it == execParams.end())
			return NOS_RESULT_FAILED;
		ObjectRef pathsArray;
		auto res = nosEngine.ObjectAPI->CreateArrayObject(
			it->second.TypeName, elementIds.data(), elementIds.size(), &pathsArray.GetStorage());
		if (res != NOS_RESULT_SUCCESS)
			return res;
		SetPinObject(NSN_Paths, pathsArray);

		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterListDirectory(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("ListDirectory"), ListDirectoryNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::utilities

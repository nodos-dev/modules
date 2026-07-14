// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/Helpers.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace nos::filesystem
{

// Reads a file from disk into a host-visible buffer on the Data output. Pure I/O: it does no
// decoding, so reading and decoding a media file can be split across nodes and pipelined /
// buffered independently. Re-reads only when Path changes.
struct ReadFileNode : NodeContext
{
	TypedObjectRef<sys::vulkan::Buffer> OutputBuffer;
	std::string LoadedPath;
	std::string StatusText;
	int StatusType = -1;

	using NodeContext::NodeContext;

	// Formats a byte count as a human-readable size (1000-based SI), e.g. 42.0 MB.
	static std::string HumanSize(uint64_t bytes)
	{
		const char* units[] = { "B", "KB", "MB", "GB", "TB" };
		double value = double(bytes);
		int u = 0;
		while (value >= 1000.0 && u < 4) { value /= 1000.0; ++u; }
		char buf[32];
		snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", value, units[u]);
		return buf;
	}

	void ShowStatus(const std::string& text, fb::NodeStatusMessageType type)
	{
		if (int(type) == StatusType && text == StatusText)
			return;
		StatusText = text;
		StatusType = int(type);
		SetNodeStatusMessage(text, type);
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		const char* pathData = params.GetPinValue<const char>(NOS_NAME("Path"));
		std::string path = pathData ? pathData : "";
		if (path.empty())
		{
			ShowStatus("Set file path", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS;
		}
		if (path == LoadedPath && OutputBuffer)
			return NOS_RESULT_SUCCESS; // output already holds this file

		std::string fileName = nos::PathToUtf8(nos::Utf8ToPath(path).filename());
		std::ifstream file(nos::Utf8ToPath(path), std::ios::binary | std::ios::ate);
		if (!file)
		{
			ShowStatus(fileName + " not found", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_SUCCESS;
		}
		std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);
		if (size <= 0)
		{
			ShowStatus(fileName + " is empty", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS;
		}

		nosBufferInfo info = {};
		info.Size = uint32_t(size);
		info.Usage = NOS_BUFFER_USAGE_TRANSFER_SRC;
		info.MemoryFlags = NOS_MEMORY_FLAGS_HOST_VISIBLE;
		auto buffer = sys::vulkan::CreateBuffer(info, "ReadFile");
		if (!buffer)
		{
			ShowStatus("Failed to allocate buffer", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_SUCCESS;
		}

		uint8_t* dst = nosVulkan->Map(buffer);
		if (!dst || !file.read(reinterpret_cast<char*>(dst), size))
		{
			ShowStatus("Failed to read " + fileName, fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_SUCCESS;
		}

		OutputBuffer = std::move(buffer);
		SetPinObject(NOS_NAME("Data"), OutputBuffer);
		LoadedPath = path;
		ShowStatus("Read " + fileName + " (" + HumanSize(uint64_t(size)) + ")",
			fb::NodeStatusMessageType::INFO);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterReadFile(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("ReadFile"), ReadFileNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::filesystem

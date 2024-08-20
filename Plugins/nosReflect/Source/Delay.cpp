// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"

#include <nosVulkanSubsystem/nosVulkanSubsystem.h>
#include <nosVulkanSubsystem/Helpers.hpp>

namespace nos::reflect
{
NOS_REGISTER_NAME(Delay)
NOS_REGISTER_NAME_SPACED(TextureTypeName, "nos.sys.vulkan.Texture")
NOS_REGISTER_NAME_SPACED(BufferTypeName, "nos.sys.vulkan.Buffer")

template <typename T>
struct RingBuffer {
	std::deque<T> Ring;
	std::queue<T> FreeList;
	uint32_t Size;
	RingBuffer(uint32_t sz) : Size(sz) {}
	RingBuffer(const RingBuffer&) = delete;
	bool GetLastPopped(T& out)
	{
		if (FreeList.empty())
			return false;
		out = std::move(FreeList.front());
		FreeList.pop();
		return true;
	}
	bool CanPush()
	{
		return Ring.size() < Size;
	}
	void Push(T val)
	{
		Ring.push_back(std::move(val));
	}
	bool BeginPop(T& out)
	{
		if (Ring.empty())
			return false;
		if (Ring.size() < Size)
			return false;
		out = std::move(Ring.front());
		Ring.pop_front();
		return true;
	}
	void EndPop(T popped)
	{
		if (FreeList.size() >= Size)
			return;
		FreeList.push(std::move(popped));
	}
};

struct AnySlot
{
	nos::Buffer Buffer;
	virtual ~AnySlot() = default;
	AnySlot(const AnySlot&) = delete;
	AnySlot& operator=(const AnySlot&) = delete;
	AnySlot(nosBuffer const& buf) : Buffer(buf) {}
	virtual void CopyFrom(nosBuffer const& buf, nosUUID const& nodeId) = 0;
};

struct TriviallyCopyableSlot : AnySlot
{
	using AnySlot::AnySlot;
	void CopyFrom(nosBuffer const& other, nosUUID const& nodeId) override
	{
		Buffer = other;
	}
};

template <typename T>
struct Resource
{
	nosResourceShareInfo Desc;
	
	Resource(T r) : Desc{}
	{
		if constexpr (std::is_same_v<T, nosBufferInfo>)
		{
			Desc.Info.Type = NOS_RESOURCE_TYPE_BUFFER;
			Desc.Info.Buffer = r;
		}
		else
		{
			Desc.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
			Desc.Info.Texture = r;
		}
		nosVulkan->CreateResource(&Desc);
	}
        
	~Resource()
	{ 
		nosVulkan->DestroyResource(&Desc);
	}
};

template <typename T>
requires std::is_same_v<T, nosTextureInfo> || std::is_same_v<T, nosBufferInfo>
struct ResourceSlot : AnySlot
{
	std::unique_ptr<Resource<T>> Res;
	ResourceSlot(nosBuffer const& buf) : AnySlot(buf)
	{
		if constexpr (std::is_same_v<T, nosBufferInfo>) {
			auto desc = vkss::ConvertToResourceInfo(*InterpretPinValue<sys::vulkan::Buffer>(buf.Data));
			reinterpret_cast<int&>(desc.Info.Buffer.Usage) |= nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST);
			Res = std::make_unique<Resource<T>>(desc.Info.Buffer);
		}
		if constexpr (std::is_same_v<T, nosTextureInfo>) {
			auto desc = vkss::DeserializeTextureInfo(Buffer.Data());
			// desc.Info.Texture.FieldType = NOS_TEXTURE_FIELD_TYPE_UNKNOWN;
			Res = std::make_unique<Resource<T>>(desc.Info.Texture);
		}
	}
	void CopyFrom(nosBuffer const& other, nosUUID const& nodeId) override
	{
		// TODO: Interlaced.
		nosCmd cmd{};
		nosCmdBeginParams beginParams{
			.Name = NOS_NAME_STATIC("Delay Copy"),
			.AssociatedNodeId = nodeId,
			.OutCmdHandle = &cmd
		};
		nosResourceShareInfo src{};
		if constexpr (std::is_same_v<T, nosBufferInfo>)
			src = vkss::ConvertToResourceInfo(*InterpretPinValue<sys::vulkan::Buffer>(other.Data));
		if constexpr (std::is_same_v<T, nosTextureInfo>)
			src = vkss::DeserializeTextureInfo(other.Data);
		nosVulkan->Begin2(&beginParams);
		nosVulkan->Copy(cmd, &src, &Res->Desc, nullptr);
		//nosCmdEndParams endParams{.ForceSubmit = NOS_FALSE, .OutGPUEventHandle = &Texture->Params.WaitEvent};
		//nosVulkan->End(cmd, &endParams);
		nosVulkan->End(cmd, nullptr);
		if constexpr (std::is_same_v<T, nosTextureInfo>)
			Buffer = nos::Buffer::From(vkss::ConvertTextureInfo(Res->Desc));
		if constexpr (std::is_same_v<T, nosBufferInfo>)
			Buffer = nos::Buffer::From(vkss::ConvertBufferInfo(Res->Desc));
	}
};

struct DelayNode : NodeContext
{
    nosName TypeName = NSN_VOID;
	RingBuffer<std::unique_ptr<AnySlot>> Ring;

	DelayNode(const nosFbNode* node) : NodeContext(node), Ring(0) {
		for (auto pin : *node->pins())
		{
			auto name = nos::Name(pin->name()->c_str());
			if (NSN_Delay == name)
			{
				Ring.Size = *(u32*)pin->data()->data();
			}
			if (NSN_Output == name)
			{
				if (pin->type_name()->c_str() == NSN_VOID.AsString())
					continue;
				TypeName = nos::Name(pin->type_name()->c_str());
			}
		}
	}


	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
	{
		if(NSN_VOID == TypeName)
			return;
		if (NSN_Delay == pinName)
		{
			Ring.Size = *static_cast<u32*>(value.Data);
		}
	}

	void OnPinUpdated(nosPinUpdate const* update) override
	{
		if (TypeName != NSN_VOID)
			return;
		if (update->UpdatedField == NOS_PIN_FIELD_TYPE_NAME)
		{
			if (update->PinName != NSN_Input)
				return;
			TypeName = update->TypeName;
		}
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);

		auto& inputBuffer = *execParams[NSN_Input].Data;
		auto delay = *InterpretPinValue<uint32_t>(*execParams[NSN_Delay].Data);

		if (0 == delay)
		{
			SetPinValue(NSN_Output, inputBuffer);
			return NOS_RESULT_SUCCESS;
		}

		std::unique_ptr<AnySlot> slot = nullptr;
		if (!Ring.GetLastPopped(slot))
		{
			if (TypeName == NSN_TextureTypeName)
				slot = std::make_unique<ResourceSlot<nosTextureInfo>>(inputBuffer);
			else if (TypeName == NSN_BufferTypeName)
				slot = std::make_unique<ResourceSlot<nosBufferInfo>>(inputBuffer);
			else
				slot = std::make_unique<TriviallyCopyableSlot>(inputBuffer);
		}
		slot->CopyFrom(inputBuffer, NodeId);
		while (!Ring.CanPush())
		{
			std::unique_ptr<AnySlot> popped;
			if (!Ring.BeginPop(popped))
				break;
			Ring.EndPop(std::move(popped));
		}
		Ring.Push(std::move(slot));
		std::unique_ptr<AnySlot> popped;
		if (Ring.BeginPop(popped))
		{
			SetPinValue(NSN_Output, popped->Buffer);
			Ring.EndPop(std::move(popped));
			return NOS_RESULT_SUCCESS;
		}
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterDelay(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Delay, DelayNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos
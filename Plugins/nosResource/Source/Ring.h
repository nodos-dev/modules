/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
#include <Nodos/Plugin.hpp>

 // External
#include <nosSysVulkan/Helpers.hpp>

#include "Nodos/Utils/Stopwatch.hpp"

NOS_REGISTER_NAME(Input)
NOS_REGISTER_NAME(Output)
NOS_REGISTER_NAME(Size)
NOS_REGISTER_NAME_SPACED(Generic, "nos.Generic")
NOS_REGISTER_NAME(Alignment)

namespace nos::resource
{
struct ResourceInterface {
	virtual ~ResourceInterface() = default;

	enum class ResourceType : uint32_t {
		GPUBuffer = 1,
		GPUTexture,
		POD
	};
	ResourceType Type;

	struct ResourceBase {
		virtual ~ResourceBase() = default;
		ResourceType ResourceType;
		std::atomic_uint64_t FrameNumber;
	};


	template<typename T>
	static T::Resource* GetResource(ResourceBase* res) {
		if (res->ResourceType == T::RESOURCE_TYPE) {
			return static_cast<T::Resource*>(res);
		}
		return nullptr;
	}

	ResourceInterface(ResourceType type) : Type(type) {}
	virtual rc<ResourceBase> CreateResource() = 0;
	virtual void Reset(ResourceBase* res) = 0;
	virtual void WaitForDownloadToEnd(ResourceBase* res, const std::string& nodeTypeName, const std::string& nodeDisplayName, nosCopyFromInfo* cpy) = 0;
	virtual void Copy(ResourceBase* res, nosCopyFromInfo* cpy, uuid NodeId) = 0;
	virtual nosResult Push(ResourceBase* r, ObjectRef inObj, NodeExecuteParams const& params, nos::Name ringExecuteName, bool pushEventForCopyFrom) = 0;
	virtual nosResult SkipExecute(NodeExecuteParams const& params) { return NOS_RESULT_SUCCESS; }
	virtual ObjectRef ValidateAndGetPinObject(nosPinInfo const& pin, bool rejectFieldMismatch) = 0;
	// Returns false if resource is compatible with the current sample
	virtual bool CheckNewResource(nos::Name updateName, nosObjectId inObj) = 0;
	virtual bool BeginCopyFrom(ResourceBase* r, nosObjectId curPinObj, ObjectRef& outPinObj) = 0;
	virtual void OnRepeatPinValue(nosCopyFromInfo* cpy) {}
	virtual std::pair<uint32_t, std::string> GetRequiredRingSize(nosObjectId inputPinData, uint32_t ringSize) const { return {ringSize, ""}; }
	virtual void OnPathStart() {}
};

inline std::pair<uint32_t, std::string> GetRequiredRingSizeForFieldType(nosTextureFieldType fieldType, uint32_t ringSize)
{
	std::stringstream message;
	if (sys::vulkan::IsTextureFieldTypeInterlaced(fieldType))
	{
		// Because ring delays by "size - 1" and what comes in should come out from the ring
		auto preAdjustedSize = ringSize;
		ringSize = ringSize | 0b1;
		if (preAdjustedSize != ringSize)
			message << "Effective ring size was adjusted from " << preAdjustedSize << " to " << ringSize << "\nto maintain full-frame interlaced output delays"; 
	}
	return {ringSize, message.str()};
}

struct GPUTextureResource : ResourceInterface
{
	static constexpr ResourceType RESOURCE_TYPE = ResourceInterface::ResourceType::GPUTexture;
	struct Resource : ResourceBase
	{
		TypedObjectRef<sys::vulkan::Texture> TexObj;
		struct
		{
			nosTextureFieldType FieldType = NOS_TEXTURE_FIELD_TYPE_UNKNOWN;
			nosGPUEvent WaitEvent = 0;
		} Params{};
		Resource(TypedObjectRef<sys::vulkan::Texture> texObj) : TexObj(std::move(texObj)) { ResourceType = RESOURCE_TYPE; }
		~Resource()
		{
			if (Params.WaitEvent)
				nosVulkan->WaitGpuEvent(&Params.WaitEvent, UINT64_MAX);
		}
	};

	nosTextureFieldType WantedField = NOS_TEXTURE_FIELD_TYPE_UNKNOWN;
	nosTextureInfo SampleTexture = nosTextureInfo{
		.Width = 1920,
		.Height = 1080,
		.Format = NOS_FORMAT_R16G16B16A16_SFLOAT,
		.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST),
	};

	GPUTextureResource() : ResourceInterface(ResourceType::GPUTexture)
	{
	}
	rc<ResourceBase> CreateResource() override
	{
		auto texture = sys::vulkan::CreateTexture(SampleTexture, "Texture Ring Resource");
		if (!texture.IsValid())
			return nullptr;
		return MakeShared<Resource>(std::move(texture));
	}
	void Reset(ResourceBase* r) override
	{
		Resource* res = GetResource<GPUTextureResource>(r);
		if (res->Params.WaitEvent)
			nosVulkan->WaitGpuEvent(&res->Params.WaitEvent, UINT64_MAX);
		res->Params = {};
		res->FrameNumber = 0;
	}

	void OnPathStart() override { WantedField = NOS_TEXTURE_FIELD_TYPE_UNKNOWN; }

	void WaitForDownloadToEnd(ResourceBase* r,
							  const std::string& nodeTypeName,
							  const std::string& nodeDisplayName,
							  nosCopyFromInfo* cpy) override
	{
		Resource* res = GetResource<GPUTextureResource>(r);
		if (res->Params.WaitEvent)
		{
			nos::util::Stopwatch sw;
			nosVulkan->WaitGpuEvent(&res->Params.WaitEvent, UINT64_MAX);

			auto elapsed = sw.Elapsed();
			nosEngine.WatchLog((nodeTypeName + " Copy From GPU Wait: " + nodeDisplayName).c_str(),
							   nos::util::Stopwatch::ElapsedString(elapsed).c_str());
		}

		SetPinObject(cpy->ID, res->TexObj);
	}

	void Copy(ResourceBase* r, nosCopyFromInfo* cpy, uuid NodeId) override
	{
		Resource* res = GetResource<GPUTextureResource>(r);
		auto outputTex = *cpy->PinObjectHandle;
		nosCmd cmd;
		nosCmdBeginParams beginParams = {NOS_NAME("BoundedQueue"), NodeId, &cmd};
		nosVulkan->Begin(&beginParams);
		nosVulkan->Copy(cmd, res->TexObj, outputTex, 0);
		nosCmdEndParams end{.ForceSubmit = NOS_TRUE, .OutGPUEventHandle = &res->Params.WaitEvent};
		nosVulkan->End(cmd, &end);
		nosVulkan->SetResourceFieldType(outputTex, res->Params.FieldType);
		// TODO: Make output unscaled
	}

	ObjectRef ValidateAndGetPinObject(nosPinInfo const& pin, bool rejectFieldMismatch) override
	{
		auto inTex = TypedObjectRef<sys::vulkan::Texture>::FromObjectId(*pin.Object);

		if (!inTex.IsValid())
			return {};
		auto resInfo = *sys::vulkan::GetResourceInfo(inTex);
		nosTextureFieldType incomingField = sys::vulkan::GetResourceFieldType(inTex);

		if (rejectFieldMismatch)
		{
			if (WantedField == NOS_TEXTURE_FIELD_TYPE_UNKNOWN)
				WantedField = incomingField;

			auto outInterlaced = sys::vulkan::IsTextureFieldTypeInterlaced(WantedField);
			auto inInterlaced = sys::vulkan::IsTextureFieldTypeInterlaced(incomingField);
			if ((inInterlaced && outInterlaced) && incomingField != WantedField)
			{
				nosEngine.LogW("Field mismatch. Waiting for a new frame.");
				return {};
			}
			WantedField = sys::vulkan::FlippedField(WantedField);
		}

		return inTex;
	}

	nosResult Push(ResourceBase* r,
				   ObjectRef inputObj,
				   NodeExecuteParams const& params,
				   nos::Name ringExecuteName,
				   bool pushEventForCopyFrom) override
	{
		Resource* res = GetResource<GPUTextureResource>(r);
		res->FrameNumber = params.FrameNumber;

		auto inTex = TypedObjectRef<sys::vulkan::Texture>::FromObjectId(inputObj);
		assert(inTex.IsValid());
		auto inputInfo = *sys::vulkan::GetResourceInfo(inTex);
		nosTextureFieldType incomingField = sys::vulkan::GetResourceFieldType(inTex);
		res->Params.FieldType = incomingField;

		if (res->Params.WaitEvent)
		{
			nos::util::Stopwatch sw;
			nosVulkan->WaitGpuEvent(&res->Params.WaitEvent, UINT64_MAX);
			auto elapsed = sw.Elapsed();
			nosEngine.WatchLog(
				(ringExecuteName.AsString() + " Execute GPU Wait: " + nos::Name(params.NodeName).AsString()).c_str(),
				nos::util::Stopwatch::ElapsedString(elapsed).c_str());
		}
		nosCmd cmd;
		nosCmdBeginParams beginParams;
		beginParams = {ringExecuteName, params.NodeId, &cmd};

		nosVulkan->Begin(&beginParams);
		nosVulkan->Copy(cmd, inTex, res->TexObj, 0);
		nosCmdEndParams end{.ForceSubmit = NOS_TRUE,
							.OutGPUEventHandle = pushEventForCopyFrom ? &res->Params.WaitEvent : nullptr};
		nosVulkan->End(cmd, &end);
		return NOS_RESULT_SUCCESS;
	}

	bool CheckNewResource(nos::Name updateName, nosObjectId newObject) override
	{
		if (updateName != NSN_Input)
			return false;
		auto newTex = TypedObjectRef<sys::vulkan::Texture>::FromObjectId(newObject);
		if (!newTex.IsValid())
			return false;
		auto info = *sys::vulkan::GetResourceInfo(newTex);
		if (SampleTexture.Format == info.Format &&
			SampleTexture.Height == info.Height && SampleTexture.Width == info.Width)
			return false;
		SampleTexture.Format = info.Format;
		SampleTexture.Width = info.Width;
		SampleTexture.Height = info.Height;
		return true;
	}

	bool BeginCopyFrom(ResourceBase* r, nosObjectId curPinObj, ObjectRef& outPinObj) override
	{
		Resource* res = GetResource<GPUTextureResource>(r);
		auto curTex = TypedObjectRef<sys::vulkan::Texture>::FromObjectId(curPinObj);
		auto currentInfo = *sys::vulkan::GetResourceInfo(curTex);
		auto resInfo = *sys::vulkan::GetResourceInfo(res->TexObj);
		if (resInfo.Height != currentInfo.Height ||
			resInfo.Width != currentInfo.Width ||
			resInfo.Format != currentInfo.Format)
		{
			currentInfo = resInfo;
			currentInfo.Usage =
				nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST | NOS_IMAGE_USAGE_SAMPLED);

			outPinObj = sys::vulkan::CreateTexture(currentInfo, "Texture Ring Output");
			return true;
		}
		return false;
	}

	void OnRepeatPinValue(nosCopyFromInfo* cpy) override
	{
		if (*cpy->PinObjectHandle == 0)
			return;
		nosVulkan->SetResourceFieldType(*cpy->PinObjectHandle, NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE);
	}

	std::pair<uint32_t, std::string> GetRequiredRingSize(nosObjectId inputPinObj, uint32_t ringSize) const override
	{
		std::stringstream message;
		if (inputPinObj == 0)
			return {ringSize, message.str()};
		return GetRequiredRingSizeForFieldType(sys::vulkan::GetResourceFieldType(inputPinObj), ringSize);
	}

	nosResult SkipExecute(NodeExecuteParams const& executeParams) override
	{
		// Force submit to ensure passes run correctly
		sys::vulkan::EndCmd(sys::vulkan::BeginCmd(NOS_NAME("SkipExecute"), executeParams.NodeId), true, nullptr);
		return NOS_RESULT_SUCCESS;
	}
};

struct GPUBufferResource : ResourceInterface {
	static constexpr ResourceType RESOURCE_TYPE = ResourceInterface::ResourceType::GPUBuffer;
	struct Resource : ResourceBase
	{
		Resource(TypedObjectRef<sys::vulkan::Buffer> bufObj) : BufObj(std::move(bufObj))
		{
			ResourceType = RESOURCE_TYPE;
		}
		~Resource()
		{
			if (Params.WaitEvent)
				nosVulkan->WaitGpuEvent(&Params.WaitEvent, UINT64_MAX);
		}
		TypedObjectRef<sys::vulkan::Buffer> BufObj;
		struct {
			nosTextureFieldType FieldType = NOS_TEXTURE_FIELD_TYPE_UNKNOWN;
			nosGPUEvent WaitEvent = 0;
		} Params{};
	};
	nosTextureFieldType WantedField = NOS_TEXTURE_FIELD_TYPE_UNKNOWN;
	nosBufferInfo SampleBuffer =
		nosBufferInfo{ .Size = 1,
					  .Alignment = 0,
					  .Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_STORAGE_BUFFER),
					  .MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_DOWNLOAD | NOS_MEMORY_FLAGS_HOST_VISIBLE) };

	GPUBufferResource() : ResourceInterface(RESOURCE_TYPE) {
	}
	rc<ResourceBase> CreateResource() override {
		auto buf = sys::vulkan::CreateBuffer(SampleBuffer, "Buffer Ring Sample");
		if (!buf.IsValid())
			return nullptr;
		return MakeShared<Resource>(std::move(buf));
	}
	void Reset(ResourceBase* res) override
	{
		auto r = GetResource<GPUBufferResource>(res);
		if (r->Params.WaitEvent)
			nosVulkan->WaitGpuEvent(&r->Params.WaitEvent, UINT64_MAX);
		r->Params = {};
		r->FrameNumber = 0;
	}

	void OnPathStart() override
	{
		WantedField = NOS_TEXTURE_FIELD_TYPE_UNKNOWN;
	}

	void WaitForDownloadToEnd(ResourceBase* res, const std::string& nodeTypeName, const std::string& nodeDisplayName, nosCopyFromInfo* cpy) override {
		auto r = GetResource<GPUBufferResource>(res);
		if (r->Params.WaitEvent) {
			nos::util::Stopwatch sw;
			nosVulkan->WaitGpuEvent(&r->Params.WaitEvent, UINT64_MAX);

			auto elapsed = sw.Elapsed();
			nosEngine.WatchLog((nodeTypeName + " Copy From GPU Wait: " + nodeDisplayName).c_str(),
				nos::util::Stopwatch::ElapsedString(elapsed).c_str());
		}

		SetPinObject(cpy->ID, r->BufObj);
	}

	void Copy(ResourceBase* res, nosCopyFromInfo* cpy, uuid NodeId) override {
		auto r = GetResource<GPUBufferResource>(res);
		auto outBuf = TypedObjectRef<sys::vulkan::Buffer>::FromObjectId(*cpy->PinObjectHandle);
		if (!outBuf.IsValid())
			return;
		auto outBufInfo = *sys::vulkan::GetResourceInfo(outBuf);
		{
			nosCmd cmd{};
			nosCmdBeginParams beginParams = { NOS_NAME("BoundedQueue"), NodeId, &cmd };
			nosVulkan->Begin(&beginParams);
			nosVulkan->Copy(cmd, r->BufObj, outBuf, 0);
			nosCmdEndParams end{ .ForceSubmit = NOS_TRUE, .OutGPUEventHandle = &r->Params.WaitEvent };
			nosVulkan->End(cmd, &end);
		}

		nosTextureFieldType outFieldType = r->Params.FieldType;
		nosVulkan->SetResourceFieldType(outBuf, outFieldType);
	}

	ObjectRef ValidateAndGetPinObject(nosPinInfo const& pin, bool rejectFieldMismatch) override{
		auto bufObj = TypedObjectRef<sys::vulkan::Buffer>::FromObjectId(*pin.Object);
		if (!bufObj.IsValid())
			return {};
		auto bufInfo = *sys::vulkan::GetResourceInfo(bufObj);

		nosTextureFieldType incomingField = sys::vulkan::GetResourceFieldType(bufObj);

		if (rejectFieldMismatch)
		{
			if (WantedField == NOS_TEXTURE_FIELD_TYPE_UNKNOWN)
				WantedField = incomingField;

			auto outInterlaced = sys::vulkan::IsTextureFieldTypeInterlaced(WantedField);
			auto inInterlaced = sys::vulkan::IsTextureFieldTypeInterlaced(incomingField);
			if ((inInterlaced && outInterlaced) && incomingField != WantedField)
			{
				nosEngine.LogW("Field mismatch. Waiting for a new frame.");
				return {};
			}
			WantedField = sys::vulkan::FlippedField(WantedField);
		}

		return bufObj;
	}

	nosResult Push(ResourceBase* r,
				   ObjectRef pinInfo,
				   NodeExecuteParams const& params,
				   nos::Name ringExecuteName,
				   bool pushEventForCopyFrom) override
	{
		Resource* res = GetResource<GPUBufferResource>(r);
		res->FrameNumber = params.FrameNumber;

		auto inBuf = TypedObjectRef<sys::vulkan::Buffer>::FromObjectId(pinInfo);
		assert(inBuf.IsValid());
		auto inBufInfo = *sys::vulkan::GetResourceInfo(inBuf);
		nosVulkan->SetResourceFieldType(res->BufObj, sys::vulkan::GetResourceFieldType(inBuf));

		if (res->Params.WaitEvent)
		{
			nos::util::Stopwatch sw;
			nosVulkan->WaitGpuEvent(&res->Params.WaitEvent, UINT64_MAX);
			auto elapsed = sw.Elapsed();
			nosEngine.WatchLog((ringExecuteName.AsString() + " Execute GPU Wait: " + nos::Name(params.NodeName).AsString()).c_str(),
				nos::util::Stopwatch::ElapsedString(elapsed).c_str());
		}
		{
			nosCmd cmd;
			nosCmdBeginParams beginParams;
			beginParams = { ringExecuteName, params.NodeId, &cmd, NOS_CMD_QUEUE_TYPE_TRANSFER };
			nosVulkan->Begin(&beginParams);
			nosVulkan->Copy(cmd, inBuf, res->BufObj, 0);
			nosCmdEndParams end{ .ForceSubmit = NOS_TRUE, .OutGPUEventHandle = pushEventForCopyFrom ? &res->Params.WaitEvent : nullptr };
			nosVulkan->End(cmd, &end);
		}
		return NOS_RESULT_SUCCESS;
	}

	bool CheckNewResource(nos::Name updateName, nosObjectId newObj)
	{
		if (updateName == NSN_Input)
		{
			auto newBuf = TypedObjectRef<sys::vulkan::Buffer>::FromObjectId(newObj);
			if (!newBuf.IsValid())
				return false;
			auto newBufInfo = *sys::vulkan::GetResourceInfo(newBuf);
			if (SampleBuffer.Size == newBufInfo.Size)
				return false;

			SampleBuffer.Size = newBufInfo.Size;
			SampleBuffer.ElementType = newBufInfo.ElementType;
			SampleBuffer.Usage = (nosBufferUsage)(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST);
			SampleBuffer.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_DOWNLOAD | NOS_MEMORY_FLAGS_HOST_VISIBLE);
		}
		else if (updateName == NSN_Alignment)
		{
			uint32_t alignment = *InterpretObject<uint32_t>(newObj);
			if (SampleBuffer.Alignment == alignment)
				return false;
			SampleBuffer.Alignment = alignment;
		}
		else
			return false;
		return true;
	}

	bool BeginCopyFrom(ResourceBase* r, nosObjectId curPinObj, ObjectRef& outPinObj) override
	{
		Resource* res = GetResource<GPUBufferResource>(r);
		auto curBufInfo = sys::vulkan::GetResourceInfo(curPinObj)->Buffer;
		auto resBufInfo = *sys::vulkan::GetResourceInfo(res->BufObj);
		if (resBufInfo.Size != curBufInfo.Size)
		{
			outPinObj = sys::vulkan::CreateBuffer(curBufInfo, "Buffer Ring Output");
			return true;
		}
		return false;
	}

	void OnRepeatPinValue(nosCopyFromInfo* cpy) override
	{
		if (*cpy->PinObjectHandle == 0)
			return;
		nosVulkan->SetResourceFieldType(*cpy->PinObjectHandle, NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE);
	}

	std::pair<uint32_t, std::string> GetRequiredRingSize(nosObjectId inputPinObj, uint32_t ringSize) const override
	{
		std::stringstream message;
		if (!inputPinObj)
			return {ringSize, message.str()};
		return GetRequiredRingSizeForFieldType(sys::vulkan::GetResourceFieldType(inputPinObj), ringSize);
	}
	nosResult SkipExecute(NodeExecuteParams const& executeParams) override
	{
		// Force submit to ensure passes run correctly
		sys::vulkan::EndCmd(sys::vulkan::BeginCmd(NOS_NAME("SkipExecute"), executeParams.NodeId), true, nullptr);
		return NOS_RESULT_SUCCESS;
	}
};

struct PODResource : ResourceInterface
{
	static constexpr ResourceType RESOURCE_TYPE = ResourceInterface::ResourceType::POD;
	struct Resource : ResourceBase
	{
		PrimitiveObjectRef PODObj{};
		Resource() { ResourceType = RESOURCE_TYPE; }
	};

	PODResource() : ResourceInterface(RESOURCE_TYPE) {}
	rc<ResourceBase> CreateResource() override
	{
		rc<Resource> res = MakeShared<Resource>();
		return res;
	}
	void Reset(ResourceBase* res) override
	{
		auto r = GetResource<PODResource>(res);
		r->FrameNumber = 0;
		r->PODObj = {};
	}

	void WaitForDownloadToEnd(ResourceBase* res,
							  const std::string& nodeTypeName,
							  const std::string& nodeDisplayName,
							  nosCopyFromInfo* cpy) override
	{
		auto r = GetResource<PODResource>(res);
		if (!r->PODObj.IsValid())
		{
			NOS_SOFT_CHECK(false, "POD object is not valid in WaitForDownloadToEnd of PODResource.");
			return;
		}
		SetPinObject(cpy->ID, r->PODObj);
	}

	void Copy(ResourceBase* res, nosCopyFromInfo* cpy, uuid NodeId) override
	{
		auto r = GetResource<PODResource>(res);
		if (!r->PODObj.IsValid())
		{
			NOS_SOFT_CHECK(false, "POD object is not valid in WaitForDownloadToEnd of PODResource.");
			return;
		}
		SetPinObject(cpy->ID, r->PODObj);
	}

	ObjectRef ValidateAndGetPinObject(nosPinInfo const& pin, bool rejectFieldMismatch) override
	{
		return *pin.Object;
	}

	nosResult Push(ResourceBase* r,
				   ObjectRef pinInfo,
				   NodeExecuteParams const& params,
				   nos::Name ringExecuteName,
				   bool pushEventForCopyFrom) override
	{
		Resource* res = GetResource<PODResource>(r);
		res->FrameNumber = params.FrameNumber;
		res->PODObj = std::move(pinInfo);
		return NOS_RESULT_SUCCESS;
	}
	bool CheckNewResource(nos::Name updateName, nosObjectId newObj) { return false; }

	bool BeginCopyFrom(ResourceBase* r, nosObjectId curPinObj, ObjectRef& outPinObj) override
	{
		outPinObj = curPinObj;
		return true;
	}
};

struct TRing
{
	std::shared_ptr<ResourceInterface> ResInterface;
	bool RejectFieldMismatch = false;

    void Resize(uint32_t size)
    {
        Size = size;
        Write.Pool = {};
        Read.Pool = {};
        Resources.clear();
        for (uint32_t i = 0; i < size; ++i)
		{
			auto res = ResInterface->CreateResource();
			if (!res) {
				nosEngine.LogE("Failed to create resource for ring buffer.");
				Resources.clear();
				Write.Pool = {};
				Read.Pool = {};
				Exit = true;
				return;
			}
			Resources.push_back(res);
            Write.Pool.push_back(res.get());
        }
    }
    
	TRing(uint32_t ringSize, std::shared_ptr<ResourceInterface> resourceManager) : ResInterface(std::move(resourceManager))
    {
        Resize(ringSize);
    }

    struct
    {
        std::deque<ResourceInterface::ResourceBase *> Pool;
        std::mutex Mutex;
        std::condition_variable CV;
    } Write, Read;

    std::vector<rc<ResourceInterface::ResourceBase>> Resources;

    uint32_t Size = 0;
    nosVec2u Extent;
    std::atomic_bool Exit = false;
    std::atomic_bool ResetFrameCount = true;

    ~TRing()
    {
        Stop();
        Resources.clear();
    }

	bool IsResourcesValid()
	{ 
		return Resources.size();
	}

    void Stop()
    {
        {
            std::unique_lock l1(Write.Mutex);
            std::unique_lock l2(Read.Mutex);
            Exit = true;
        }
		Write.CV.notify_all();
		Read.CV.notify_all();
    }

    bool IsFull()
    {
        std::unique_lock lock(Read.Mutex);
		return Read.Pool.size() == Resources.size(); 
    }

	bool HasEmptySlots()
	{
		return EmptyFrames() != 0;
	}

	uint32_t EmptyFrames()
	{
		std::unique_lock lock(Write.Mutex);
		return Write.Pool.size();
	}

    bool IsEmpty()
    {
        std::unique_lock lock(Read.Mutex);
        return Read.Pool.empty();
    }

    uint32_t ReadyFrames()
    {
        std::unique_lock lock(Read.Mutex);
        return Read.Pool.size();
    }

    uint32_t TotalFrameCount()
    {
        std::unique_lock lock(Write.Mutex);
        return Size - Write.Pool.size();
    }

	ResourceInterface::ResourceBase* BeginPush(uint64_t timeoutMs)
    {
        std::unique_lock lock(Write.Mutex);
		if (!Write.CV.wait_for(
				lock, std::chrono::milliseconds(timeoutMs), [this]() { return !Write.Pool.empty() || Exit; }))
            return 0;
        ResourceInterface::ResourceBase* res = Write.Pool.front();
        Write.Pool.pop_front();
        return res;
    }

    void EndPush(ResourceInterface::ResourceBase* res)
    {
        {
            std::unique_lock lock(Read.Mutex);
            Read.Pool.push_back(res);
			assert(Read.Pool.size() <= Resources.size());
        }
        Read.CV.notify_one();
    }

    void CancelPush(ResourceInterface::ResourceBase* res)
	{
		{
			std::unique_lock lock(Write.Mutex);
			res->FrameNumber = 0;
			Write.Pool.push_front(res);
			assert(Write.Pool.size() <= Resources.size());
		}
		Write.CV.notify_one();
	}
	void CancelPop(ResourceInterface::ResourceBase* res)
	{
		{
			std::unique_lock lock(Read.Mutex);
			Read.Pool.push_front(res);
			assert(Read.Pool.size() <= Resources.size());
		}
		Read.CV.notify_one();
	}

	ResourceInterface::ResourceBase*BeginPop(uint64_t timeoutMilliseconds)
    {
        std::unique_lock lock(Read.Mutex);
        if (!Read.CV.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), [this]() {return !Read.Pool.empty() || Exit; }))
            return 0;
        if (Exit)
            return 0;
        auto res = Read.Pool.front();
        Read.Pool.pop_front();
        return res;
    }

    void EndPop(ResourceInterface::ResourceBase*res)
    {
        {
            std::unique_lock lock(Write.Mutex);
            res->FrameNumber = 0;
            Write.Pool.push_back(res);
			assert(Write.Pool.size() <= Resources.size());
        }
        Write.CV.notify_one();
    }

    void Reset(bool fill)
    {
        auto& from = fill ? Write : Read;
		auto& to = fill ? Read : Write;
		std::unique_lock l1(Write.Mutex);
		std::unique_lock l2(Read.Mutex);
		while (!from.Pool.empty())
		{
			auto* slot = from.Pool.front();
			from.Pool.pop_front();
			ResInterface->Reset(slot);
			to.Pool.push_back(slot);
		}
    }
};

struct RingNodeBase : NodeContext
{
	enum class RingMode
	{
		CONSUME,
		FILL,
	};
	std::unique_ptr<TRing> Ring = nullptr;
	std::atomic_bool IsOutLive = false;

	// If reset, then reset the ring on path stop
	// If wait until full, do not output until ring is full & then start consuming
	enum class OnRestartType
	{
		RESET,
		WAIT_UNTIL_FULL
	} OnRestart;

	std::optional<uint32_t> RequestedRingSize = std::nullopt;
	bool NeedsRecreation = false;

	std::atomic_uint32_t SpareCount = 0;

	std::condition_variable ModeCV;
	std::mutex ModeMutex;
	std::atomic<RingMode> Mode = RingMode::CONSUME;
	std::size_t RemainingRepeatableCount = 0;

	std::atomic_bool RepeatWhenFilling = false;
	TypeInfo TypeInfo;

	enum class Status
	{
		Ok,
		EffectiveRingSizeAdjusted,
	} CurrentStatus = Status::Ok;
	std::string CurrentStatusMessage;

	void SetStatus(Status newStatus, std::string message = "")
	{
		if (CurrentStatus == newStatus)
		{
			if (newStatus == Status::EffectiveRingSizeAdjusted && CurrentStatusMessage != message)
			{
				CurrentStatusMessage = std::move(message);
				ClearNodeStatusMessages();
				SetNodeStatusMessage(CurrentStatusMessage, fb::NodeStatusMessageType::WARNING);
			}
			return;
		}

		CurrentStatus = newStatus;
		CurrentStatusMessage = std::move(message);
		switch (CurrentStatus)
		{
		case Status::Ok: {
			CurrentStatusMessage.clear();
			ClearNodeStatusMessages();
			return;
		}
		case Status::EffectiveRingSizeAdjusted: {
			ClearNodeStatusMessages();
			SetNodeStatusMessage(CurrentStatusMessage,
				fb::NodeStatusMessageType::WARNING);
			return;
		}
		default: return;
		}
	}

	void RequestRingResize(uint32_t size)
	{
		if (size == 0)
		{
			nosEngine.LogW((GetName() + " size cannot be 0").c_str());
			return;
		}
		if (Ring->Size != size && (!RequestedRingSize.has_value() || *RequestedRingSize != size))
		{
			nosPathCommand ringSizeChange{ .Event = NOS_RING_SIZE_CHANGE, .RingSize = size };
			nosEngine.SendPathCommand(PinName2Id[NSN_Input], ringSizeChange);
			SendPathRestart();
			RequestedRingSize = size;
			Ring->Stop();
		}
	}

	void TypeResolved()
	{
		std::shared_ptr<ResourceInterface> resource;
		if (TypeInfo->TypeName == NOS_NAME(sys::vulkan::Buffer::GetFullyQualifiedName()))
			resource = std::make_unique<GPUBufferResource>();
		else if (TypeInfo->TypeName == NOS_NAME(sys::vulkan::Texture::GetFullyQualifiedName()))
			resource = std::make_unique<GPUTextureResource>();
		else
			resource = std::make_unique<PODResource>();

		Ring = std::make_unique<TRing>(1, std::move(resource));

		Ring->Stop();
		AddPinValueWatcher(NSN_Size, [this](nos::Buffer const& newSize, std::optional<nos::Buffer> oldVal) {
			uint32_t size = *newSize.As<uint32_t>();
			if (oldVal && oldVal == newSize)
				return;
			RequestRingResize(size);
		});
		AddPinObjectWatcher(NSN_Input, [this](ObjectRef newObj, std::optional<ObjectRef>) {
			if (Ring->ResInterface->CheckNewResource(NSN_Input, newObj))
			{
				SendPathRestart();
				Ring->Stop();
				NeedsRecreation = true;
			}
		});
		AddPinObjectWatcher(NSN_Alignment, [this](ObjectRef newObj, std::optional<ObjectRef>) {
			if (Ring->ResInterface->CheckNewResource(NSN_Alignment, newObj))
			{
				SendPathRestart();
				Ring->Stop();
				NeedsRecreation = true;
			}
		});
		AddPinValueWatcher(NOS_NAME_STATIC("RepeatWhenFilling"), [this](nos::Buffer const& newVal, std::optional<nos::Buffer> oldVal) {
			RepeatWhenFilling = *newVal.As<bool>();
		});
	}

	RingNodeBase(OnRestartType onRestart) : OnRestart(onRestart), TypeInfo(NSN_Generic) {}
	nosResult OnCreate(nosFbNodePtr node) override
	{
		nos::Name typeName = NSN_Generic;
		if(auto* pins = node->pins())
			for (auto* pin : *pins)
				if (pin->name()->c_str() == NSN_Output)
					IsOutLive = pin->live();
		for (auto& pin : Pins | std::views::values)
			if (pin.TypeName != NSN_Generic && (pin.Name == NSN_Output || pin.Name == NSN_Input))
				typeName = pin.TypeName;
		if (typeName != NSN_Generic) {
			TypeInfo = nos::TypeInfo(typeName);
			TypeResolved();
		}
		return NOS_RESULT_SUCCESS;
	}

	virtual std::string GetName() const = 0;

	void SendRingStats(std::string_view state) const
	{
		auto nodeName = NodeName.AsString();
		nosEngine.WatchLog((nodeName + " Read Size").c_str(), std::to_string(Ring->Read.Pool.size()).c_str());
		nosEngine.WatchLog((nodeName + " Write Size").c_str(), std::to_string(Ring->Write.Pool.size()).c_str());
		nosEngine.WatchLog((nodeName + " Total Frame Count").c_str(), std::to_string(Ring->TotalFrameCount()).c_str());
		nosEngine.WatchLog((nodeName + " State").c_str(), state.data());
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		if (TypeInfo.TypeName != NSN_Generic)
			return NOS_RESULT_FAILED;

		TypeInfo = nos::TypeInfo(params->IncomingTypeName);

		for (size_t i = 0; i < params->PinCount; i++)
		{
			auto& pinInfo = params->Pins[i];
			std::string pinName = nosEngine.GetString(pinInfo.Name);
			if (pinName == "Input" || pinName == "Output")
				pinInfo.OutResolvedTypeName = TypeInfo->TypeName;
		}

		return NOS_RESULT_SUCCESS;
	}

	void OnPinUpdated(const nosPinUpdate* pinUpdate) {
		if (TypeInfo->TypeName == NSN_Generic || Ring)
			return;

		TypeResolved();
	}

	nosResult SkipExecuteRingNode(NodeExecuteParams const& params, nosName ringExecuteName) 
	{
		if (Ring->Exit || !Ring->IsResourcesValid() || !TypeInfo)
			return NOS_RESULT_FAILED;
		return Ring->ResInterface->SkipExecute(params);
	}

	nosResult ExecuteRingNode(NodeExecuteParams const& params, bool pushEventForCopyFrom, nosName ringExecuteName, bool rejectFieldMismatch)
	{
		if (Ring->Exit || !Ring->IsResourcesValid() || !TypeInfo)
			return NOS_RESULT_FAILED;

		auto& inputPin = params[NSN_Input];

		auto inputObj = Ring->ResInterface->ValidateAndGetPinObject(params[NSN_Input], rejectFieldMismatch);
		if (!inputObj.IsValid())
		{
			SendScheduleRequest(0);
			return NOS_RESULT_FAILED;
		}
		
		uint32_t requestedSize = *params.GetPinValue<uint32_t>(NSN_Size);

		auto [requiredSize, message] = Ring->ResInterface->GetRequiredRingSize(inputObj, requestedSize);
		bool effectiveSizeAdjusted = requiredSize != requestedSize;
		if (effectiveSizeAdjusted)
			SetStatus(Status::EffectiveRingSizeAdjusted, message);
		else
			SetStatus(Status::Ok);

		if (Ring->Size != requiredSize)
		{
			RequestRingResize(requiredSize);
			if (effectiveSizeAdjusted)
				nosEngine.LogW("%s", message.c_str());
			return NOS_RESULT_FAILED;
		}

		if (Ring->IsFull())
		{
			nosEngine.LogI("Trying to push while ring is full");
		}

		typename ResourceInterface::ResourceBase* slot = nullptr;
		
		SendRingStats("Pre Push");
		{
			nos::util::Stopwatch sw;
			ScopedProfilerEvent _({ .Name = "Wait For Empty Slot" });
			slot = Ring->BeginPush(100);
			nosEngine.WatchLog((GetName() + " Begin Push").c_str(), nos::util::Stopwatch::ElapsedString(sw.Elapsed()).c_str());
		}
		if (!slot)
		{
			if (Ring->Exit)
				return NOS_RESULT_FAILED;
			else
				return NOS_RESULT_PENDING;
		}
		Ring->ResInterface->Push(slot, inputObj, params, ringExecuteName, pushEventForCopyFrom);
		
		bool isFillComplete = false;
		if(Mode == RingMode::FILL)
			isFillComplete = Ring->Write.Pool.size() == 0;
		Ring->EndPush(slot);
		SendRingStats("Post Push");

		if (isFillComplete)
		{
			std::unique_lock lock(ModeMutex);
			Mode = RingMode::CONSUME;
			ModeCV.notify_all();
		}
		if (!IsOutLive)
		{
			ChangePinLiveness(NSN_Output, true);
			IsOutLive = true;
		}

		return NOS_RESULT_SUCCESS;
	}

	nosResult CommonCopyFrom(nosCopyFromInfo* cpy, ResourceInterface::ResourceBase** foundSlot) {
		if (!Ring || Ring->Exit)
			return NOS_RESULT_FAILED;

		// This is needed since out pins are created as dirty and CopyFrom is called for dirty pins.
		if (!IsOutLive)
			return NOS_RESULT_SUCCESS;

		if (OnRestart == OnRestartType::WAIT_UNTIL_FULL && RepeatWhenFilling)
		{
			if (RemainingRepeatableCount > 0)
			{
				Ring->ResInterface->OnRepeatPinValue(cpy);
				RemainingRepeatableCount--;
				return NOS_RESULT_SUCCESS;
			}
		}
		else if (Mode == RingMode::FILL)
		{
			//Sleep for 100 ms & if still Fill, return pending
			std::unique_lock lock(ModeMutex);
			if (!ModeCV.wait_for(lock, std::chrono::milliseconds(100), [this] { return Mode != RingMode::FILL; }))
				return NOS_RESULT_PENDING;
		}

		ResourceInterface::ResourceBase* slot;
		SendRingStats("Pre Begin Pop");
		{
			ScopedProfilerEvent _({ .Name = "Wait For Filled Slot" });
			slot = Ring->BeginPop(100);
		}
		// If timeout or exit
		if (!slot)
			return Ring->Exit ? NOS_RESULT_FAILED : NOS_RESULT_PENDING;
		SendRingStats("Post Begin Pop");

		ObjectRef outPinObj{};
		bool changePinValue = Ring->ResInterface->BeginCopyFrom(slot, *cpy->PinObjectHandle, outPinObj);
		if (changePinValue) {
			nosEngine.SetPinObjectByName(NodeId, NSN_Output, outPinObj);
		}
		*foundSlot = slot;
		return NOS_RESULT_SUCCESS;
	}

	void OnPathCommand(const nosPathCommand* command) override
	{
		switch (command->Event)
		{
		case NOS_RING_SIZE_CHANGE: {
			if (command->RingSize == 0)
			{
				nosEngine.LogW((GetName() + " size cannot be 0.").c_str());
				return;
			}
			RequestedRingSize = command->RingSize;
			nosEngine.SetPinValue(*GetPinId(NSN_Size), nos::Buffer::From(command->RingSize));
			break;
		}
		default: return;
		}
	}

	void SendScheduleRequest(uint32_t count, bool reset = false) const
	{
		nosScheduleNodeParams schedule{
			.NodeId = NodeId,
			.AddScheduleCount = count,
			.Reset = reset
		};
		nosEngine.ScheduleNode(&schedule);
	}

	void OnPathStop() override
	{
		if (OnRestart == OnRestartType::WAIT_UNTIL_FULL)
			Mode = RingMode::FILL;
		if (Ring)
			Ring->Stop();
	}

	void OnPathStart() override
	{
		if (!Ring) return;
		// Reset read pool for Queues(OnRestart::RESET) and Rings(OnRestart::WAIT_UNTIL_FULL) with Repeat too, if repeat, then size-1 CopyFroms will be repeated from the last buffer.
		if (OnRestart == OnRestartType::RESET || RepeatWhenFilling)
			Ring->Reset(false);
		// We must wait for at least a frame to be sure that providing path is started and running smoothly
		else if (Ring->IsFull() && !Ring->Read.Pool.empty())
		{
			Ring->Write.Pool.push_back(Ring->Read.Pool.front());
			Ring->Read.Pool.pop_front();
		}
		if (RequestedRingSize)
		{
			Ring->Resize(*RequestedRingSize);
			RequestedRingSize = std::nullopt;
			NeedsRecreation = false;
		}
		if (NeedsRecreation)
		{
			Ring = std::make_unique<TRing>(Ring->Size, Ring->ResInterface);
			NeedsRecreation = false;
		}
		if (!Ring->IsResourcesValid())
		{
			// This is here since invalid state might be solved after execution
			SendScheduleRequest(1);
			return;
		}
		auto emptySlotCount = Ring->Write.Pool.size();
		if (RepeatWhenFilling)
			RemainingRepeatableCount = std::max(emptySlotCount, (size_t)1) - 1;
		nosScheduleNodeParams schedule{ .NodeId = NodeId, .AddScheduleCount = emptySlotCount };
		nosEngine.ScheduleNode(&schedule);
		Ring->Exit = false;
		Ring->ResInterface->OnPathStart();
	}

	void SendPathRestart()
	{
		nosEngine.SendPathRestart(PinName2Id[NSN_Input]);
	}

	void OnEndFrame(uuid const& pinId, nosEndFrameCause cause) override
	{
		if (cause != NOS_END_FRAME_FAILED)
			return;
		if (pinId == PinName2Id[NSN_Output])
			return;
		if(!IsOutLive)
			return;
		ChangePinLiveness(NSN_Output, false);
		IsOutLive = false;
	}

	~RingNodeBase() override
	{
		if (Ring)
			Ring->Stop();
	}
};

} // namespace nos::resource

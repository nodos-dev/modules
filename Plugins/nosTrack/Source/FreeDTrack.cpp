// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <nosTrack/Track.h>


using asio::ip::udp;
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint32_t uint32;
typedef int32_t int32;

namespace nos::track
{

	struct FreeDFloat
	{
		uint8 Value1;

		uint8 Value2;

		uint8 Value3;

		FreeDFloat()
			: Value1(0), Value2(0), Value3(0)
		{
		}

		float GetValue() const
		{
			return (float)((Value1 << 16) | (Value2 << 8) | (Value3));
		}
	};

	struct FreeDFloat_Rotation
	{
		uint8 Value1;

		uint8 Value2;

		uint8 Value3;

		FreeDFloat_Rotation()
			: Value1(0), Value2(0), Value3(0)
		{
		}

		float GetValue(const float& Constant) const
		{
			uint8 PreValue = ((Value1 & 0x80) == 0x80) ? 0xff : 0x00;
			uint32 Value = (PreValue << 24) | (Value1 << 16) | (Value2 << 8) | (Value3);
			return (*(int32*)(&Value)) / Constant;
		}
	};

	struct FreeDFloat_Location
	{
		uint8 Value1;

		uint8 Value2;

		uint8 Value3;

		FreeDFloat_Location()
			: Value1(0), Value2(0), Value3(0)
		{
		}

		float GetValue(const float& Constant) const
		{
			uint8 PreValue = ((Value1 & 0x80) == 0x80) ? 0xff : 0x00;
			uint32 Value = (PreValue << 24) | (Value1 << 16) | (Value2 << 8) | (Value3);
			return (*(int32*)(&Value)) / Constant;
		}
	};



	struct FreeDIntegerAndFraction
	{
		int8 Value1;

		uint8 Value2;

		int8 Value3;

		uint8 Value4;

		FreeDIntegerAndFraction()
			: Value1(0), Value2(0), Value3(0), Value4(0)
		{
		}

		int32 GetFractionValue() const
		{
			int8 PreValue = ((Value1 & 0x00) == 0x00) ? 0xff : 0x00;
			uint32 Value = (PreValue << 16) | (Value1 << 8) | (Value2);
			return (*(int32*)(&Value));
		}

		int32 GetIntegerValue() const
		{
			int8 PreValue = ((Value3 & 0x80) == 0x80) ? 0xff : 0x00;
			int32 Value = (PreValue << 16) | (Value3 << 8) | (Value4);
			return (*(int32*)(&Value));
		}

		// TODO: needs professional eyes.
		float GetValue(const float& Constant) const
		{
			auto IntegerValue = GetIntegerValue();
			auto FractionValue = GetFractionValue();
			float midvalue = 0.0f;
			if (IntegerValue >= 0)
			{
				midvalue = (float)IntegerValue + ((float)FractionValue / UINT16_MAX) + 1.0f;
			}
			else
			{
				midvalue = (float)IntegerValue - ((float)FractionValue / UINT16_MAX) - 1.0f;
			}

			return midvalue / Constant;
		}
	};

	struct FreeDInteger
	{
		uint8 Value1;

		uint8 Value2;

		uint8 Value3;

		FreeDInteger()
			: Value1(0), Value2(0), Value3()
		{
		}

		uint32 GetIntegerValue() const
		{
			return (Value1 << 16) | (Value2 << 8) | (Value3);
		}

		// Zoom ve focus icin test edilmedi.
		float GetValue(const float& MinConstant, const float& MaxConstant) const
		{
			auto IntegerValue = GetIntegerValue();
			return (float)(IntegerValue - MinConstant) / (MaxConstant - MinConstant);
		}
	};


	struct FreeDMessage_D1
	{
		uint8 Header;

		uint8 CameraID;

		FreeDFloat_Rotation Pan;

		FreeDFloat_Rotation Tilt;

		FreeDFloat_Rotation Roll;

		FreeDFloat_Location X;

		FreeDFloat_Location Y;

		FreeDFloat_Location Z;

		FreeDInteger Zoom;

		FreeDInteger Focus;

		uint8 SpareData1;

		uint8 SpareData2;

		uint8 Checksum;

		glm::vec3 GetLocation(const glm::vec3& Constants) const
		{
			return glm::vec3(X.GetValue(Constants.x), Y.GetValue(Constants.y), Z.GetValue(Constants.z));
		}

		glm::vec3 GetRotation(const glm::vec3& Constants) const
		{
			return glm::vec3(Roll.GetValue(Constants.z), Tilt.GetValue(Constants.x), Pan.GetValue(Constants.y));
		}

		bool IsChecksumOK() const
		{
			//The checksum is calculated by subtracting (modulo 256) each byte 
			//of the message, including the messageype, from 40 (hex).
			const uint8* Buffer = (const uint8*)this;
			uint8 TotalSum = 0x40;
			for (uint32 Index = 0; Index < sizeof(FreeDMessage_D1) - 1; ++Index)
			{
				TotalSum -= Buffer[Index];
			}
			if (TotalSum != Checksum)
			{
				return false;
			}
			return true;
		}

	};

NOS_REGISTER_NAME(InvertZoom);
NOS_REGISTER_NAME(InvertFocus);

struct FreeDNodeContext : public TrackNodeContext
{
	public:
		nosResult OnCreate(nos::fb::Node const* node) override
		{
			TrackNodeContext::OnCreate(node);
			for (auto* pin : *node->pins())
			{
				LoadField<glm::vec2>(pin, NSN_ZoomRange, ZoomRange);
				LoadField<glm::vec2>(pin, NSN_FocusRange, FocusRange);
				LoadField<bool>(pin, NSN_InvertZoom, InvertZoom);
				LoadField<bool>(pin, NSN_InvertFocus, InvertFocus);
			}
			return NOS_RESULT_SUCCESS;
		}

        glm::vec2 ZoomRange = {0, 60000};
        glm::vec2 FocusRange = {0, 60000};

		bool InvertZoom = false;
		bool InvertFocus = false;

        bool Parse(std::vector<uint8_t> const& data, track::TTrack& TrackData) override
        {
            if(data.size() < sizeof(FreeDMessage_D1))
            {
                return false;
            }
			FreeDMessage_D1 d1msg = *(FreeDMessage_D1*)data.data();
			auto Location = d1msg.GetLocation(glm::vec3(640));
			auto Rotation = d1msg.GetRotation(glm::vec3(32768));

			TrackData.zoom = InvertZoom ? 
				d1msg.Zoom.GetValue(ZoomRange.y, ZoomRange.x) :
				d1msg.Zoom.GetValue(ZoomRange.x, ZoomRange.y);

			TrackData.focus = InvertFocus ?
				d1msg.Focus.GetValue(FocusRange.y, FocusRange.x) :
				d1msg.Focus.GetValue(FocusRange.x, FocusRange.y);

			TrackData.location = reinterpret_cast<nos::fb::vec3&>(Location);
			TrackData.rotation = reinterpret_cast<nos::fb::vec3&>(Rotation);
            return true;
        }

        void OnPinValueChanged(nos::Name pinName, uuid const& pinId, nosBuffer val)  override
        {
            if(NSN_ZoomRange == pinName)
            {
                ZoomRange = *(glm::vec2*)val.Data;
            }
            if(NSN_FocusRange == pinName)
            {
                FocusRange = *(glm::vec2*)val.Data;
            }
			if(NSN_InvertZoom == pinName)
			{
				InvertZoom = *(bool*)val.Data;
			}
			if(NSN_InvertFocus == pinName)
			{
				InvertFocus = *(bool*)val.Data;
			}
            TrackNodeContext::OnPinValueChanged(pinName, pinId, val);
        }

		~FreeDNodeContext() override {
			Stop();
		}
};

void RegisterFreeDNode(nosNodeFunctions* functions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.track.FreeD"), FreeDNodeContext, functions);
}

} // namespace zd
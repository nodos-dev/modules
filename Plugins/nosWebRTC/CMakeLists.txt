# Copyright MediaZ Teknoloji A.S. All Rights Reserved.


if(WIN32)
	add_compile_definitions(
	USE_AURA=1 _HAS_EXCEPTIONS=0 __STD_C _CRT_RAND_S
	_CRT_SECURE_NO_DEPRECATE _SCL_SECURE_NO_DEPRECATE
	_ATL_NO_OPENGL _WINDOWS CERT_CHAIN_PARA_HAS_EXTRA_FIELDS
	PSAPI_VERSION=2 WIN32 _SECURE_ATL WINUWP
	__WRL_NO_DEFAULT_LIB__ WINAPI_FAMILY=WINAPI_FAMILY_PC_APP
	WIN10=_WIN32_WINNT_WIN10 WIN32_LEAN_AND_MEAN
	NOMINMAX _UNICODE UNICODE NTDDI_VERSION=NTDDI_WIN10_RS2
	_WIN32_WINNT=0x0A00 WINVER=0x0A00 NVALGRIND
	DYNAMIC_ANNOTATIONS_ENABLED=0 WEBRTC_ENABLE_PROTOBUF=0
	WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE RTC_ENABLE_VP9
	HAVE_SCTP WEBRTC_LIBRARY_IMPL WEBRTC_NON_STATIC_TRACE_EVENT_HANDLERS=0
	WEBRTC_WIN ABSL_ALLOCATOR_NOTHROW=1
	_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS
	_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
	)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/External)
find_library(webrtc NAMES webrtc.lib PATHS ${CMAKE_CURRENT_LIST_DIR}/External/lib/Release REQUIRED NO_DEFAULT_PATH)
find_library(jsoncpp_static NAMES jsoncpp_static.lib PATHS ${CMAKE_CURRENT_LIST_DIR}/External/lib/Release REQUIRED NO_DEFAULT_PATH)
find_library(websockets_static NAMES websockets_static.lib PATHS ${CMAKE_CURRENT_LIST_DIR}/External/lib/Release REQUIRED NO_DEFAULT_PATH)
find_library(libssl NAMES libssl.lib PATHS ${CMAKE_CURRENT_LIST_DIR}/External/lib/Release REQUIRED NO_DEFAULT_PATH)
find_library(libcrypto NAMES libcrypto.lib PATHS ${CMAKE_CURRENT_LIST_DIR}/External/lib/Release REQUIRED NO_DEFAULT_PATH)
link_directories(${CMAKE_CURRENT_LIST_DIR}/External/lib/Release)

##Ordering of the 4 libraries (webrtc, websockets, libssl, libcrypto) matters!!!
nos_add_plugin("nosWebRTC" 
			"${NOS_SYS_VULKAN_TARGET_5_8};${NOS_PLUGIN_SDK_TARGET};jsoncpp_static.lib;webrtc.lib;websockets_static.lib;libssl.lib;libcrypto.lib;WS2_32.LIB;GDI32.lib;USER32.lib;WinMM.Lib;dmoguids.lib;AdvAPI32.Lib;Secur32.lib;amstrmid.lib;strmiids.lib;Crypt32.Lib;wmcodecdspuuid.lib;iphlpapi.lib;msdmo.lib;"
 "${CMAKE_CURRENT_SOURCE_DIR}")

#Always force MD (because webrtc.lib built in Release => MD )
target_compile_options(nosWebRTC PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/MD>)
set_target_properties(nosWebRTC PROPERTIES OUTPUT_NAME "nosWebRTC")

include_directories(
	${CMAKE_CURRENT_LIST_DIR}/External/include
    ${SUBSYSTEMS_SOURCE_DIR}/nosVulkanSubsystem/Include)

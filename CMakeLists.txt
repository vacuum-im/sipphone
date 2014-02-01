project(sipphone)

set(PLUGIN_NAME "sipphone")
set(PLUGIN_DISPLAY_NAME "SIP Phone")

if (WIN32)
	add_definitions(-DPJ_WIN32=1) 
	
	if (CMAKE_BUILD_TYPE STREQUAL "Debug")
		set(ADD_LIBS libpjproject-i386-Win32-vc8-Debug-Dynamic)
	else(CMAKE_BUILD_TYPE STREQUAL "Debug")
		set(ADD_LIBS libpjproject-i386-Win32-vc8-Release-Dynamic)
	endif(CMAKE_BUILD_TYPE STREQUAL "Debug")

	list(APPEND ADD_LIBS ws2_32 ole32 oleaut32 uuid odbc32 odbccp32 winmm Iphlpapi dsound dxguid netapi32 mswsock user32 gdi32 advapi32)
	link_directories("${PJ_DEV_DIR}/msdirectx/lib/x86" "${PJ_DEV_DIR}/pjproject/lib" "${PJ_DEV_DIR}/ffmpeg/lib")

	include_directories(AFTER "${PJ_DEV_DIR}/msdirectx/include" "${PJ_DEV_DIR}/pjproject/include" "${PJ_DEV_DIR}/ffmpeg/include")
endif (WIN32)

include("sipphone.cmake")
include("${CMAKE_SOURCE_DIR}/src/plugins/plugins.cmake")

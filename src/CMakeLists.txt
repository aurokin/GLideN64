cmake_minimum_required(VERSION 3.10)

option(PANDORA "Set to ON if targeting an OpenPandora" ${PANDORA})
option(ODROID "Set to ON if targeting an Odroid" ${ODROID})
option(MUPENPLUSAPI "Set to ON for Mupen64Plus plugin" ${MUPENPLUSAPI})
option(MUPENPLUSAPI_GLIDENUI "Set to ON for GLideNUI for Mupen64Plus" ${MUPENPLUSAPI_GLIDENUI})
option(GLIDENUI_QT6 "Set to ON to use Qt6 for GLideNUI" ${GLIDENUI_QT6})
option(MESA "Set to ON to disable Raspberry Pi autodetection" ${MESA})
option(VERO4K "Set to ON if targeting a Vero4k" ${VERO4K})
option(ANDROID "Set to ON if targeting an Android device" ${ANDROID})
option(DEBUG_DUMP "Set to ON to enable debug dump" ${DEBUG_DUMP})
option(NO_OSD "Set to ON to disable OSD text drawing" ${NO_OSD})
option(USE_IPO "Enables IPO/LTO when supported" ON)
option(REALITYVK_BUILD_RVK2_TESTS "Build RealityVK2 unit tests" ON)

if(DEFINED REALITYVK_BUILD_LEGACY_RENDERER AND REALITYVK_BUILD_LEGACY_RENDERER)
  message(FATAL_ERROR "Vulkan-only branch: REALITYVK_BUILD_LEGACY_RENDERER is not supported.")
endif()

# run script to generate Revision.h
set(PATH_REVISION "${CMAKE_CURRENT_BINARY_DIR}/inc/Revision.h")
find_package(Git)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/../.git")
	add_custom_command(
		OUTPUT ${PATH_REVISION}
		COMMAND
		env OUTPUT=${PATH_REVISION} ${CMAKE_SOURCE_DIR}/getRevision.sh
		COMMENT "Generate Git version"
	)
else(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/../.git")
	add_custom_command(
		OUTPUT ${PATH_REVISION}
		COMMAND
		env OUTPUT=${PATH_REVISION} ${CMAKE_SOURCE_DIR}/getRevision.sh --nogit
		COMMENT "Generate version"
	)
endif(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/../.git")
set_property(SOURCE ${PATH_REVISION} PROPERTY SKIP_AUTOGEN ON)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/inc")

project( RealityVK )

include(GNUInstallDirs)

find_package(Vulkan QUIET)
if (Vulkan_FOUND)
  set(REALITYVK_VULKAN_LIBRARIES Vulkan::Vulkan)
  message(STATUS "Vulkan loader: found")
else()
  set(REALITYVK_VULKAN_LIBRARIES "")
  message(STATUS "Vulkan loader: not found (Vulkan backend core init will be unavailable)")
endif()

message(STATUS "RealityVK backend: Vulkan-first branch")

set(RealityVK_SOURCES
  3DMath.cpp
  Combiner.cpp
  CombinerKey.cpp
  CommonPluginAPI.cpp
  Config.cpp
  convert.cpp
  CRC32.cpp
  DebugDump.cpp
  Debugger.cpp
  DepthBuffer.cpp
  DisplayWindow.cpp
  DisplayLoadProgress.cpp
  FrameBuffer.cpp
  FrameBufferInfo.cpp
  GBI.cpp
  gDP.cpp
  RealityVK.cpp
  GraphicsDrawer.cpp
  gSP.cpp
  Log.cpp
  N64.cpp
  PaletteTexture.cpp
  Performance.cpp
  PostProcessor.cpp
  RDP.cpp
  RSP.cpp
  RSP_LoadMatrix.cpp
  SoftwareRender.cpp
  TexrectDrawer.cpp
  TextDrawer.cpp
  TextureFilterHandler.cpp
  Textures.cpp
  VI.cpp
  ZlutTexture.cpp
  BufferCopy/BlueNoiseTexture.cpp
  BufferCopy/ColorBufferToRDRAM.cpp
  BufferCopy/DepthBufferToRDRAM.cpp
  BufferCopy/RDRAMtoColorBuffer.cpp
  DepthBufferRender/ClipPolygon.cpp
  DepthBufferRender/DepthBufferRender.cpp
  common/CommonAPIImpl_common.cpp
  Graphics/Context.cpp
  Graphics/ContextFactory.cpp
  Graphics/ColorBufferReader.cpp
  Graphics/CombinerProgram.cpp
  Graphics/RealityVK2/rvk2_CommandStream.cpp
  Graphics/RealityVK2/rvk2_ContextImpl.cpp
  Graphics/RealityVK2/rvk2_DrawSemantic.cpp
  Graphics/RealityVK2/rvk2_Executor.cpp
  Graphics/RealityVK2/rvk2_RasterPipeline.cpp
  Graphics/RealityVK2/rvk2_RenderPlan.cpp
  Graphics/RealityVK2/rvk2_RDPState.cpp
  Graphics/RealityVK2/rvk2_RSPFrontend.cpp
  Graphics/RealityVK2/rvk2_Runtime.cpp
  Graphics/RealityVK2/rvk2_RuntimeSwitch.cpp
  Graphics/RealityVK2/rvk2_SubmissionPlan.cpp
  Graphics/RealityVK2/rvk2_TMEMModel.cpp
  Graphics/RealityVK2/rvk2_TraceOutput.cpp
  Graphics/RealityVK2/rvk2_Validation.cpp
  Graphics/RealityVK2/rvk2_VIRenderer.cpp
  Graphics/ObjectHandle.cpp
  Graphics/Parameters.cpp
  Graphics/VulkanContext/vulkan_ContextImpl.cpp
  Graphics/VulkanContext/vulkan_ContextImpl_InstanceDevice.cpp
  Graphics/VulkanContext/vulkan_ContextImpl_Swapchain.cpp
  Graphics/VulkanContext/vulkan_CombinerDecode.cpp
  Graphics/VulkanContext/vulkan_CombinerClassify.cpp
  Graphics/VulkanContext/vulkan_CombinerApply.cpp
  Graphics/VulkanContext/vulkan_BlendMux.cpp
  Graphics/VulkanContext/vulkan_CombinerHeuristics.cpp
  Graphics/VulkanContext/vulkan_ReadbackAdapters.cpp
  Graphics/VulkanContext/vulkan_PacketBuilder.cpp
  Graphics/VulkanContext/vulkan_PacketNormalize.cpp
  Graphics/VulkanContext/vulkan_SpecialPrograms.cpp
  uCodes/F3D.cpp
  uCodes/F3DBETA.cpp
  uCodes/F3DDKR.cpp
  uCodes/F3DEX.cpp
  uCodes/F3DEX095.cpp
  uCodes/F3DAM.cpp
  uCodes/F3DEX2.cpp
  uCodes/F3DEX3.cpp
  uCodes/F3DEX2ACCLAIM.cpp
  uCodes/F3DEX2CBFD.cpp
  uCodes/F3DZEX2.cpp
  uCodes/F3DFLX2.cpp
  uCodes/F3DGOLDEN.cpp
  uCodes/F3DTEXA.cpp
  uCodes/F3DPD.cpp
  uCodes/F3DSETA.cpp
  uCodes/F5Indi_Naboo.cpp
  uCodes/F5Rogue.cpp
  uCodes/L3D.cpp
  uCodes/L3DEX2.cpp
  uCodes/L3DEX.cpp
  uCodes/S2DEX2.cpp
  uCodes/S2DEX.cpp
  uCodes/T3DUX.cpp
  uCodes/Turbo3D.cpp
  uCodes/ZSort.cpp
  uCodes/ZSortBOSS.cpp
)

set(GLideNUI_SOURCES
  GLideNUI/AboutDialog.cpp
  GLideNUI/AboutDialog.ui
  GLideNUI/ConfigDialog.cpp
  GLideNUI/Config_GLideNUI.cpp
  GLideNUI/GLideNUI.cpp
  GLideNUI/Settings.cpp
  GLideNUI/QtKeyToHID.cpp
  GLideNUI/HIDKeyToName.cpp
  GLideNUI/configDialog.ui
  GLideNUI/icon.qrc
)

message(STATUS "Legacy renderer sources: disabled (Vulkan-only branch)")

if(MUPENPLUSAPI_GLIDENUI OR NOT MUPENPLUSAPI)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)
  set(CMAKE_AUTOUIC ON)

  if (GLIDENUI_QT6)
    find_package(Qt6 COMPONENTS Core Widgets Gui REQUIRED)
    set(GLIDENUI_LIBRARIES
        Qt6::Widgets Qt6::Core Qt6::Gui
    )
  else(GLIDENUI_QT6)
    find_package(Qt5 COMPONENTS Core Widgets Gui REQUIRED)
    set(GLIDENUI_LIBRARIES
        Qt5::Widgets Qt5::Core Qt5::Gui
    )
  endif(GLIDENUI_QT6)
  if (MUPENPLUSAPI)
    list(APPEND GLideNUI_SOURCES
      GLideNUI/fullscreenresolutions_mupen64plus.cpp
    )
  else()
    list(APPEND GLideNUI_SOURCES
      GLideNUI/fullscreenresolutions_windows.cpp
    )
  endif()
endif()

# Build type

if( NOT CMAKE_BUILD_TYPE)
        set( CMAKE_BUILD_TYPE Release)
endif( NOT CMAKE_BUILD_TYPE)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" OR
   CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(REALITYVK_BUILD_TYPE Release)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(REALITYVK_BUILD_TYPE Debug)
endif()

if( REALITYVK_BUILD_TYPE STREQUAL "Debug")
	set( DEBUG_BUILD TRUE)
endif( REALITYVK_BUILD_TYPE STREQUAL "Debug")

if (DEBUG_DUMP)
  add_definitions(
    -DDEBUG_DUMP
  )
endif(DEBUG_DUMP)

#check if we're running on Raspberry Pi
if(EXISTS "${CMAKE_FIND_ROOT_PATH}/opt/vc/include/bcm_host.h" AND NOT MESA)
  message("bcm_host.h found")
  set(BCMHOST ON)
  set(EGL ON)
  set(EGL_LIB -lbrcmEGL -lbrcmGLESv2)
  add_definitions(
    -DVC
  )
  include_directories(
    "${CMAKE_FIND_ROOT_PATH}/opt/vc/include"
    "${CMAKE_FIND_ROOT_PATH}/opt/vc/include/interface/vcos"
    "${CMAKE_FIND_ROOT_PATH}/opt/vc/include/interface/vmcs_host/linux"
    "${CMAKE_FIND_ROOT_PATH}/opt/vc/include/interface/vcos/pthreads"
  )
  link_directories(
    "${CMAKE_FIND_ROOT_PATH}/opt/vc/lib"
    "${CMAKE_FIND_ROOT_PATH}/opt/vc/lib/GL"
  )
else(EXISTS "${CMAKE_FIND_ROOT_PATH}/opt/vc/include/bcm_host.h" AND NOT MESA)
  set(EGL_LIB -lEGL)
endif(EXISTS "${CMAKE_FIND_ROOT_PATH}/opt/vc/include/bcm_host.h" AND NOT MESA)

if(MUPENPLUSAPI)
  add_definitions(
	-DMUPENPLUSAPI
		-DTXFILTER_LIB
  )
  include_directories( . inc )
  set(RealityVK_SOURCES_UNIX
	MupenPlusPluginAPI.cpp
	mupenplus/CommonAPIImpl_mupenplus.cpp
	mupenplus/MemoryStatus_mupenplus.cpp
	mupenplus/MupenPlusAPIImpl.cpp
	Graphics/Host/mupen64plus/mupen64plus_DisplayWindow.cpp
  )

  if(MUPENPLUSAPI_GLIDENUI)
      add_definitions(-DM64P_GLIDENUI)
      list(APPEND RealityVK_SOURCES_UNIX
        ${GLideNUI_SOURCES}
      )
  else()
      list(APPEND RealityVK_SOURCES_UNIX
        mupenplus/Config_mupenplus.cpp
      )
  endif()

  set(RealityVK_SOURCES_WIN ${RealityVK_SOURCES_UNIX}
  )
  if (ANDROID)
      set(RealityVK_DLL_NAME libmupen64plus-video-RealityVK)
  else()
      set(RealityVK_DLL_NAME mupen64plus-video-RealityVK)
  endif()
else(MUPENPLUSAPI)
  message(FATAL_ERROR "Vulkan-only branch currently supports only MUPENPLUSAPI builds.")
  include_directories( . inc )
	set(RealityVK_SOURCES_WIN
		ZilmarPluginAPI.cpp
		GLideNUI/Config_GLideNUI.cpp
		windows/CommonAPIImpl_windows.cpp
		windows/RealityVK_windows.cpp
		windows/MemoryStatus_windows.cpp
		windows/ZilmarAPIImpl_windows.cpp
	  windows/ScreenShot.cpp
	  ${GLideNUI_SOURCES}
	  )
	set(RealityVK_DLL_NAME RealityVK)
endif(MUPENPLUSAPI)

add_subdirectory( osal )
include_directories( osal )

if (NOHQ)
  list(APPEND RealityVK_SOURCES
    TxFilterStub.cpp
  )
else (NOHQ)
  add_subdirectory( GLideNHQ )
  include_directories( GLideNHQ )
endif(NOHQ)

if(UNIX)
  list(APPEND RealityVK_SOURCES ${RealityVK_SOURCES_UNIX})
  if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	add_definitions(
	  -DOS_MAC_OS_X
	)
	if (NOT CMAKE_VERSION VERSION_LESS 2.8.12)
	  set(CMAKE_MACOSX_RPATH 0)
	endif()
  elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	add_definitions(
	  -DOS_LINUX
	)
  endif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
endif(UNIX)

if(MINGW)
  add_definitions(
    -DMINGW
  )
  if (NOT MUPENPLUSAPI)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static")
  endif()
endif(MINGW)

if(WIN32)
  list(APPEND RealityVK_SOURCES ${RealityVK_SOURCES_WIN})
  add_definitions(
    -DOS_WINDOWS
    -D__WIN32__
    -DWIN32
    -D_WIN32_ASM
    -D_CRT_SECURE_NO_WARNINGS
    -D__MSC__
  )
endif(WIN32)

if(SDL)
  include(FindPkgConfig)
  pkg_check_modules(SDL REQUIRED sdl)
  include_directories(${SDL_INCLUDE_DIRS})
  add_definitions(
	-DUSE_SDL
  )
endif(SDL)

if(PANDORA)
  #Pandora as a SGX530, but it should share the bugs and limitations as SGX540
  add_definitions(
  -DPANDORA
  -DPowerVR_SGX_540
  -DEGL
   )
endif(PANDORA)

if(ODROID)
  add_definitions(
  -DODROID
   )
endif(ODROID)

if(VERO4K)
  add_definitions(
  -DVERO4K
   )
endif(VERO4K)

if(ANDROID)
   message(FATAL_ERROR "Vulkan-only branch: Android path is not yet ported.")
endif(ANDROID)

if(NOT NO_OSD)
  if(ANDROID)
    set( FREETYPE_INCLUDE_DIRS "${ANDROID_LIB_PATH}/freetype/include" )
  elseif(UNIX OR BCMHOST)
    set( FREETYPE_INCLUDE_DIRS "/usr/include/freetype2/" )
  else(UNIX OR BCMHOST)
    set( ENV{FREETYPE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../freetype )
  endif(ANDROID)

  if(ANDROID)
      add_library( ANDROID_FREETYPE
              SHARED
              IMPORTED )

      if( REALITYVK_BUILD_TYPE STREQUAL "Debug")
          set( BUILD_VARIANT "debug" )
      else()
          set( BUILD_VARIANT "release" )
      endif()

      set_target_properties( # Specifies the target library.
              ANDROID_FREETYPE

              # Specifies the parameter you want to define.
              PROPERTIES IMPORTED_LOCATION

              # Provides the path to the library you want to import.
              ${ANDROID_LIB_PATH}/libs/${BUILD_VARIANT}/${ANDROID_ABI}/libfreetype.so)
      set(FREETYPE_LIBRARIES ANDROID_FREETYPE)
  else(ANDROID)
      FIND_PACKAGE( Freetype REQUIRED )
      # presume a qt build linked to freetype
      # will be used with mingw
      if (NOT MUPENPLUSAPI AND MINGW)
        set(FREETYPE_LIBRARIES "")
      endif()
  endif(ANDROID)

  include_directories( ${FREETYPE_INCLUDE_DIRS} )
else()
  list(REMOVE_ITEM RealityVK_SOURCES
    TextDrawer.cpp
  )
  list(APPEND RealityVK_SOURCES
    TextDrawerStub.cpp
  )
endif(NOT NO_OSD)

if(VEC4_OPT)
  add_definitions(
    -D__VEC4_OPT
  )
endif(VEC4_OPT)

if(CRC_ARMV8)
  list(REMOVE_ITEM RealityVK_SOURCES
	CRC32.cpp
  )
  list(APPEND RealityVK_SOURCES
	CRC32_ARMV8.cpp
  )
elseif(CRC_OPT)
  list(REMOVE_ITEM RealityVK_SOURCES
    CRC32.cpp
  )
  list(APPEND RealityVK_SOURCES
    CRC_OPT.cpp
  )
elseif(CRC_NEON)
  list(REMOVE_ITEM RealityVK_SOURCES
    CRC32.cpp
  )
  list(APPEND RealityVK_SOURCES
    Neon/CRC_OPT_NEON.cpp
  )
endif(CRC_ARMV8)

if(NEON_OPT)
  add_definitions(
    -D__NEON_OPT
  )
  list(APPEND RealityVK_SOURCES
    Neon/3DMathNeon.cpp
    Neon/gSPNeon.cpp
    Neon/RSP_LoadMatrixNeon.cpp
  )
  list(REMOVE_ITEM RealityVK_SOURCES
    3DMath.cpp
    RSP_LoadMatrix.cpp
  )
endif(NEON_OPT)

if(X86_OPT)
  list(APPEND RealityVK_SOURCES
    RSP_LoadMatrixX86.cpp
  )
  list(REMOVE_ITEM RealityVK_SOURCES
    RSP_LoadMatrix.cpp
  )
endif(X86_OPT)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
  #check for compiler version
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpversion OUTPUT_VARIABLE G++_VERSION)
  if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND G++_VERSION VERSION_LESS 4.8)
    message("Found compiler version " ${G++_VERSION})
    message(SEND_ERROR "You need at least G++ 4.8 to compile RealityVK!")
  elseif(G++_VERSION VERSION_LESS 3.3)
    message("Found compiler version " ${G++_VERSION})
    message(SEND_ERROR "You need at least Clang 3.3 to compile RealityVK!")
  endif()
  set(GCC_CPP11_COMPILE_FLAGS "-std=c++11 -pthread")
  set( CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} ${GCC_CPP11_COMPILE_FLAGS}" )
  if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(GCC_STATIC_LINK_FLAGS "-static-libgcc")
  endif()
  set( CMAKE_SHARED_LINKER_FLAGS  "${CMAKE_SHARED_LINKER_FLAGS} ${GCC_STATIC_LINK_FLAGS}" )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

add_library( ${RealityVK_DLL_NAME} SHARED ${RealityVK_SOURCES} ${PATH_REVISION})

set_target_properties(
	${RealityVK_DLL_NAME}
	PROPERTIES
	LINKER_LANGUAGE CXX # Or else we get an error message, because cmake can't figure out from the ".o"-suffix that it is a C-linker we need.
	PREFIX ""
)

get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(is_multi_config)
	SET_TARGET_PROPERTIES(
		${RealityVK_DLL_NAME}
		PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/plugin
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/plugin
	)
elseif(REALITYVK_BUILD_TYPE)
	SET_TARGET_PROPERTIES(
		${RealityVK_DLL_NAME}
		PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/plugin/${REALITYVK_BUILD_TYPE}
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/plugin/${REALITYVK_BUILD_TYPE}
	)
endif(is_multi_config)

if(USE_IPO AND REALITYVK_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT result)
  if(result)
    message("Interprocedural optimizations enabled")
    set_property(TARGET ${RealityVK_DLL_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -O3")
  endif()
endif(USE_IPO AND REALITYVK_BUILD_TYPE STREQUAL "Release")

if(REALITYVK_BUILD_RVK2_TESTS)
	add_executable(rvk2_unit_tests
		Graphics/RealityVK2/rvk2_UnitTests.cpp
		Graphics/RealityVK2/rvk2_CommandStream.cpp
		Graphics/RealityVK2/rvk2_DrawSemantic.cpp
		Graphics/RealityVK2/rvk2_Executor.cpp
		Graphics/RealityVK2/rvk2_RasterPipeline.cpp
		Graphics/RealityVK2/rvk2_RenderPlan.cpp
		Graphics/RealityVK2/rvk2_RDPState.cpp
		Graphics/RealityVK2/rvk2_RSPFrontend.cpp
		Graphics/RealityVK2/rvk2_Runtime.cpp
		Graphics/RealityVK2/rvk2_SubmissionPlan.cpp
		Graphics/RealityVK2/rvk2_TMEMModel.cpp
		Graphics/RealityVK2/rvk2_VIRenderer.cpp
	)
	set_target_properties(rvk2_unit_tests PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	)
endif(REALITYVK_BUILD_RVK2_TESTS)

if(REALITYVK_BUILD_TYPE STREQUAL "Debug")
  if(SDL)
	if (NOHQ)
	  target_link_libraries(${RealityVK_DLL_NAME} PRIVATE ${SDL_LIBRARIES} ${FREETYPE_LIBRARIES} ${LOG_LIB} ${GLIDENUI_LIBRARIES} osald ${REALITYVK_VULKAN_LIBRARIES} )
	else (NOHQ)
	  target_link_libraries(${RealityVK_DLL_NAME} PRIVATE ${SDL_LIBRARIES} ${FREETYPE_LIBRARIES} ${LOG_LIB} ${GLIDENUI_LIBRARIES} osald GLideNHQd ${REALITYVK_VULKAN_LIBRARIES} )
	endif (NOHQ)
  else(SDL)
	if (NOHQ)
	  target_link_libraries(${RealityVK_DLL_NAME} PRIVATE ${FREETYPE_LIBRARIES} ${LOG_LIB} ${GLIDENUI_LIBRARIES} osald ${REALITYVK_VULKAN_LIBRARIES} )
	else (NOHQ)
	  target_link_libraries(${RealityVK_DLL_NAME} PRIVATE ${FREETYPE_LIBRARIES} ${LOG_LIB} ${GLIDENUI_LIBRARIES} osald GLideNHQd ${REALITYVK_VULKAN_LIBRARIES} )
	endif (NOHQ)
  endif(SDL)
else(REALITYVK_BUILD_TYPE STREQUAL "Debug")
  if(SDL)
        if (NOHQ)
          target_link_libraries(${RealityVK_DLL_NAME} ${SDL_LIBRARIES} ${FREETYPE_LIBRARIES} ${LOG_LIB} ${GLIDENUI_LIBRARIES} osal ${REALITYVK_VULKAN_LIBRARIES} )
        else (NOHQ)
          target_link_libraries(${RealityVK_DLL_NAME} ${SDL_LIBRARIES} ${FREETYPE_LIBRARIES} ${LOG_LIB} ${GLIDENUI_LIBRARIES} osal GLideNHQ ${REALITYVK_VULKAN_LIBRARIES} )
        endif (NOHQ)
  else(SDL)
        if (NOHQ)
          target_link_libraries(${RealityVK_DLL_NAME} ${FREETYPE_LIBRARIES} ${LOG_LIB} ${GLIDENUI_LIBRARIES} osal ${REALITYVK_VULKAN_LIBRARIES} )
        else (NOHQ)
          target_link_libraries(${RealityVK_DLL_NAME} ${FREETYPE_LIBRARIES} ${LOG_LIB} ${GLIDENUI_LIBRARIES} osal GLideNHQ ${REALITYVK_VULKAN_LIBRARIES} )
        endif (NOHQ)
  endif(SDL)
endif(REALITYVK_BUILD_TYPE STREQUAL "Debug")

if(UNIX AND NOT APPLE AND NOT ANDROID)
	install(TARGETS ${RealityVK_DLL_NAME}
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/mupen64plus"
	)
	install(FILES ../ini/${PROJECT_NAME}.custom.ini
		DESTINATION "${CMAKE_INSTALL_DATADIR}/mupen64plus"
	)
endif(UNIX AND NOT APPLE AND NOT ANDROID)

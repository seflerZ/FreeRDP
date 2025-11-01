# FreeRDP: A Remote Desktop Protocol Implementation
# Android Client
#
# Copyright 2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
# Copyright 2013 Bernhard Miklautz <bernhard.miklautz@thincast.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(MODULE_NAME "freerdp-android")
set(MODULE_PREFIX "FREERDP_CLIENT_ANDROID")

include_directories(.)

if(CMAKE_C_COMPILER_ID MATCHES "GNU")  # GCC-specific flags
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-pointer-sign")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")  # Clang-specific flags
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-pointer-sign -mllvm -page-size=16384")
endif()

set(${MODULE_PREFIX}_SRCS
	android_event.c
	android_event.h
	android_freerdp.c
	android_freerdp.h
	android_jni_utils.c
	android_jni_utils.h
	android_jni_callback.c
	android_jni_callback.h)

if(WITH_CLIENT_CHANNELS)
	set(${MODULE_PREFIX}_SRCS ${${MODULE_PREFIX}_SRCS}
		android_cliprdr.c
		android_cliprdr.h)
endif()

add_library(${MODULE_NAME} SHARED ${${MODULE_PREFIX}_SRCS})

set(${MODULE_PREFIX}_LIBS ${${MODULE_PREFIX}_LIBS} winpr freerdp freerdp-client)

set(${MODULE_PREFIX}_LIBS ${${MODULE_PREFIX}_LIBS} dl)
set(${MODULE_PREFIX}_LIBS ${${MODULE_PREFIX}_LIBS} jnigraphics)

#set_target_properties(${MODULE_NAME} PROPERTIES OUTPUT_NAME ${MODULE_NAME}${FREERDP_API_VERSION})
# 链接阶段配置（保持并确认以下内容）
target_link_libraries(${MODULE_NAME} ${${MODULE_PREFIX}_LIBS})
# 目标特定链接器标志
target_link_options(${MODULE_NAME} PRIVATE "-Wl,-z,max-page-size=16384")
# 全局共享库链接器标志（确保所有共享库继承）
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

target_link_libraries(${MODULE_NAME} ${${MODULE_PREFIX}_LIBS})
install(TARGETS ${MODULE_NAME} DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT libraries EXPORT AndroidTargets)

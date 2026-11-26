cmake_minimum_required(VERSION 3.16)
project(weapon_framework_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep this test target independent from the full 32-bit game DLL build.
get_filename_component(HLSDK_ROOT "\${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_executable(weapon_framework_tests
	"\${HLSDK_ROOT}/game_shared/weapon_framework/weapon_definition.cpp"
	"\${HLSDK_ROOT}/game_shared/weapon_framework/weapon_registry.cpp"
	"\${HLSDK_ROOT}/game_shared/weapon_framework/weapon_json.cpp"
	"\${CMAKE_CURRENT_SOURCE_DIR}/weapon_framework_tests.cpp"
)

target_include_directories(weapon_framework_tests PRIVATE
	"\${HLSDK_ROOT}/dlls"
	"\${HLSDK_ROOT}/dlls/wpn_shared"
	"\${HLSDK_ROOT}/common"
	"\${HLSDK_ROOT}/engine"
	"\${HLSDK_ROOT}/pm_shared"
	"\${HLSDK_ROOT}/game_shared"
	"\${HLSDK_ROOT}/public"
)

enable_testing()
add_test(NAME weapon_framework_tests COMMAND weapon_framework_tests)

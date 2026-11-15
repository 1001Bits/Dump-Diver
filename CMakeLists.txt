cmake_minimum_required(VERSION 3.24)
project(DumpDiver VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(DD_DEFINES
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    DD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DD_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DD_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    DD_VERSION_STRING="${PROJECT_VERSION}")
set(DD_OPTIONS /W4 /WX /permissive- /utf-8 /EHsc /Zi)

# --- game plugin: one source, built for SKSE and for F4SE ---------------------------------

set(DD_PLUGIN_SOURCES
    src/plugin/Exports.cpp
    src/plugin/LoadingScreen.cpp
    src/plugin/LockWatch.cpp
    src/plugin/Log.cpp
    src/plugin/Menu.cpp
    src/plugin/Plugin.cpp
    src/plugin/TaskTracking.cpp
    src/plugin/version.rc)

function(dd_add_plugin target game_define folder)
    add_library(${target} SHARED ${DD_PLUGIN_SOURCES})
    target_include_directories(${target} PRIVATE src)
    target_compile_definitions(${target} PRIVATE ${DD_DEFINES} ${game_define})
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${DD_OPTIONS}>)
    target_link_libraries(${target} PRIVATE shell32 ole32)
    set(out ${CMAKE_BINARY_DIR}/${folder}/$<CONFIG>)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME DumpDiver
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${out}
        LIBRARY_OUTPUT_DIRECTORY ${out}
        ARCHIVE_OUTPUT_DIRECTORY ${out}
        PDB_OUTPUT_DIRECTORY ${out})
    # the archive ships a public-symbols PDB, enough for crash loggers and Dump Diver to name functions
    set(public_pdb ${CMAKE_BINARY_DIR}/${folder}/public-pdb/DumpDiver.pdb)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/${folder}/public-pdb)
    target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF "/PDBSTRIPPED:${public_pdb}")
endfunction()

dd_add_plugin(DumpDiverSkyrim DD_GAME_SKYRIM skyrim)
dd_add_plugin(DumpDiverFallout4 DD_GAME_FALLOUT4 fallout4)

# --- helper -------------------------------------------------------------------------------

set(DD_HELPER_CORE
    src/helper/Analyze.cpp
    src/helper/Process.cpp
    src/helper/Records.cpp
    src/helper/Report.cpp
    src/helper/SnapshotFile.cpp
    src/helper/Util.cpp)
set(DD_HELPER_LIBS dbghelp psapi version user32 advapi32 shell32 winmm)

add_executable(DumpDiverHelper WIN32
    ${DD_HELPER_CORE}
    src/helper/Capture.cpp
    src/helper/Main.cpp
    src/helper/Prompt.cpp
    src/helper/Stack.cpp
    src/helper/version.rc
    src/helper/DumpDiverHelper.manifest)
target_include_directories(DumpDiverHelper PRIVATE src)
target_compile_definitions(DumpDiverHelper PRIVATE ${DD_DEFINES})
target_compile_options(DumpDiverHelper PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${DD_OPTIONS}>)
target_link_libraries(DumpDiverHelper PRIVATE ${DD_HELPER_LIBS} gdiplus)
target_link_options(DumpDiverHelper PRIVATE /DEBUG /OPT:REF /OPT:ICF)

# --- tests (opt-in: nothing here runs during the build) -----------------------------------

# Unit tests of the analysis on hand-made snapshots.
add_executable(dd_analysis_tests test/analysis_tests.cpp ${DD_HELPER_CORE} src/helper/Capture.cpp src/helper/Stack.cpp)
target_include_directories(dd_analysis_tests PRIVATE src)
target_compile_definitions(dd_analysis_tests PRIVATE ${DD_DEFINES})
target_compile_options(dd_analysis_tests PRIVATE ${DD_OPTIONS})
target_link_libraries(dd_analysis_tests PRIVATE ${DD_HELPER_LIBS})

# Real captures (.ddsnap) kept as tests: the analysis must still name what each one expects.
add_executable(dd_replay_tests test/replay_tests.cpp ${DD_HELPER_CORE})
target_include_directories(dd_replay_tests PRIVATE src)
target_compile_definitions(dd_replay_tests PRIVATE ${DD_DEFINES})
target_compile_options(dd_replay_tests PRIVATE ${DD_OPTIONS})
target_link_libraries(dd_replay_tests PRIVATE ${DD_HELPER_LIBS})

# The freeze prompt, on a desktop of its own: it closes when the game is back or gone, and no key
# quits the game.
add_executable(dd_prompt_tests test/prompt_tests.cpp src/helper/Prompt.cpp src/helper/Util.cpp)
target_include_directories(dd_prompt_tests PRIVATE src)
target_compile_definitions(dd_prompt_tests PRIVATE ${DD_DEFINES})
target_compile_options(dd_prompt_tests PRIVATE ${DD_OPTIONS})
target_link_libraries(dd_prompt_tests PRIVATE ${DD_HELPER_LIBS} gdiplus)

# A stand-in game (neutral name on purpose) that loads the Skyrim build of the plugin like SKSE
# does and freezes on request, with a DLL that causes each freeze and a decoy that must not be
# blamed.
add_library(DDTestCulprit SHARED test/culprit.cpp)
target_compile_options(DDTestCulprit PRIVATE ${DD_OPTIONS})
# a full PDB, so the helper sees its inline frames the way it does for mods that ship one
target_link_options(DDTestCulprit PRIVATE /DEBUG)
add_library(DDTestDecoy SHARED test/decoy.cpp)
target_compile_options(DDTestDecoy PRIVATE ${DD_OPTIONS})
add_executable(DumpDiverTestHost test/host.cpp)
target_include_directories(DumpDiverTestHost PRIVATE src)
target_compile_definitions(DumpDiverTestHost PRIVATE ${DD_DEFINES})
target_compile_options(DumpDiverTestHost PRIVATE ${DD_OPTIONS})
target_link_libraries(DumpDiverTestHost PRIVATE user32)
# No console window, whatever starts it (Windows Terminal would show one on the user's desktop).
set_target_properties(DumpDiverTestHost PROPERTIES ENABLE_EXPORTS ON WIN32_EXECUTABLE ON)
target_link_options(DumpDiverTestHost PRIVATE /ENTRY:wmainCRTStartup)

# In-game freeze trigger for the lab runs (never packaged).
function(dd_add_freeze_test target game_define folder)
    add_library(${target} SHARED test/freezetest.cpp)
    target_include_directories(${target} PRIVATE src)
    target_compile_definitions(${target} PRIVATE ${DD_DEFINES} ${game_define})
    target_compile_options(${target} PRIVATE ${DD_OPTIONS})
    target_link_libraries(${target} PRIVATE shell32 ole32 user32)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME DumpDiverFreezeTest PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${folder}-test/$<CONFIG>
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${folder}-test/$<CONFIG>
        PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${folder}-test/$<CONFIG>)
endfunction()
dd_add_freeze_test(DumpDiverFreezeTestSkyrim DD_GAME_SKYRIM skyrim)
dd_add_freeze_test(DumpDiverFreezeTestFallout4 DD_GAME_FALLOUT4 fallout4)

add_executable(dd_scenario_tests test/scenario_tests.cpp)
target_include_directories(dd_scenario_tests PRIVATE src)
target_compile_definitions(dd_scenario_tests PRIVATE ${DD_DEFINES})
target_compile_options(dd_scenario_tests PRIVATE ${DD_OPTIONS})
add_dependencies(dd_scenario_tests DumpDiverTestHost DDTestCulprit DDTestDecoy DumpDiverSkyrim DumpDiverHelper)

enable_testing()
add_test(NAME analysis COMMAND dd_analysis_tests)
add_test(NAME replay COMMAND dd_replay_tests ${CMAKE_SOURCE_DIR}/test/corpus)
add_test(NAME prompt COMMAND dd_prompt_tests)
add_test(NAME scenarios
    COMMAND dd_scenario_tests $<TARGET_FILE:DumpDiverTestHost> $<TARGET_FILE:DumpDiverSkyrim>
            $<TARGET_FILE:DumpDiverHelper> $<TARGET_FILE:DDTestCulprit> $<TARGET_FILE:DDTestDecoy>)

# --- release archives ---------------------------------------------------------------------

function(dd_add_package name plugin loader folder)
    set(stage ${CMAKE_BINARY_DIR}/package-${folder})
    set(archive ${CMAKE_BINARY_DIR}/${name}-${PROJECT_VERSION}.zip)
    add_custom_target(package_${folder}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage} ${archive}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${stage}/${loader}/Plugins/DumpDiver
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${plugin}> ${CMAKE_BINARY_DIR}/${folder}/public-pdb/DumpDiver.pdb
                ${CMAKE_SOURCE_DIR}/dist/DumpDiver.ini ${stage}/${loader}/Plugins
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:DumpDiverHelper> ${stage}/${loader}/Plugins/DumpDiver
        COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar cf ${archive} --format=zip ${loader}
        DEPENDS ${plugin} DumpDiverHelper
        COMMENT "Packaging ${archive}")
endfunction()

dd_add_package(DumpDiver-Skyrim DumpDiverSkyrim SKSE skyrim)
dd_add_package(DumpDiver-Fallout4 DumpDiverFallout4 F4SE fallout4)

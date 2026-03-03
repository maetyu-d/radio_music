cmake_minimum_required(VERSION 3.22)

project(RandomRadioFX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" JUCE)
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/JUCE" JUCE)
else()
    find_package(JUCE CONFIG REQUIRED)
endif()

juce_add_plugin(RandomRadioFX
    COMPANY_NAME "Codex"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Cdx1
    PLUGIN_CODE Rrfx
    FORMATS AU VST3 Standalone
    PLIST_TO_MERGE "${CMAKE_CURRENT_SOURCE_DIR}/Build/NetworkPermissions.plist"
    PRODUCT_NAME "Radio Music")

target_sources(RandomRadioFX
    PRIVATE
        Source/PluginProcessor.h
        Source/PluginProcessor.cpp
        Source/PluginEditor.h
        Source/PluginEditor.cpp)

target_include_directories(RandomRadioFX
    PRIVATE
        /Applications/VLC.app/Contents/MacOS/include)

target_link_options(RandomRadioFX
    PRIVATE
        "-Wl,-rpath,@executable_path/../Frameworks")

target_compile_definitions(RandomRadioFX
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(RandomRadioFX
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        /Applications/VLC.app/Contents/MacOS/lib/libvlc.dylib
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if (TARGET RandomRadioFX_Standalone)
    add_custom_command(TARGET RandomRadioFX_Standalone POST_BUILD
        COMMAND install_name_tool -delete_rpath /Applications/VLC.app/Contents/MacOS/lib
                "$<TARGET_FILE:RandomRadioFX_Standalone>"
        COMMAND install_name_tool -add_rpath @executable_path/../Frameworks
                "$<TARGET_FILE:RandomRadioFX_Standalone>"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:RandomRadioFX_Standalone>/../Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "/Applications/VLC.app/Contents/MacOS/lib/libvlc.dylib"
                "$<TARGET_FILE_DIR:RandomRadioFX_Standalone>/../Frameworks/libvlc.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "/Applications/VLC.app/Contents/MacOS/lib/libvlccore.dylib"
                "$<TARGET_FILE_DIR:RandomRadioFX_Standalone>/../Frameworks/libvlccore.dylib"
        COMMAND codesign --force --deep --sign -
                "$<TARGET_FILE_DIR:RandomRadioFX_Standalone>/../..")
endif()

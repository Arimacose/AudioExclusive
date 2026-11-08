target_link_options(am_exclusive_hook PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/fixed-hook.map")
target_compile_options(am_exclusive_broker PRIVATE /utf-8)
add_executable(runtime_readonly "${CMAKE_CURRENT_LIST_DIR}/tests/RuntimeReadOnly.cpp")
target_include_directories(runtime_readonly PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rigtorp-SPSCQueue/include")
target_compile_definitions(runtime_readonly PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_executable(playback_recovery_tests "${CMAKE_CURRENT_LIST_DIR}/tests/PlaybackRecoveryTests.cpp"
  src/audio_v2/AudioCoreRuntime.cpp src/audio_v2/AudioCoreCoordinator.cpp src/audio_v2/WasapiExclusiveSink.cpp)
target_include_directories(playback_recovery_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rigtorp-SPSCQueue/include")
target_compile_definitions(playback_recovery_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(playback_recovery_tests PRIVATE ole32 uuid avrt)
add_executable(apple_local_pipeline_tests "${CMAKE_CURRENT_LIST_DIR}/tests/AppleLocalPipelineTests.cpp"
  src/IpcTransport.cpp src/LosslessQualityLock.cpp src/audio_v2/AudioCoreRuntime.cpp
  src/audio_v2/AudioCoreCoordinator.cpp src/audio_v2/WasapiExclusiveSink.cpp src/audio_v2/NativeRenderGateProxy.cpp)
target_include_directories(apple_local_pipeline_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rigtorp-SPSCQueue/include")
target_compile_definitions(apple_local_pipeline_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(apple_local_pipeline_tests PRIVATE /utf-8)
target_link_libraries(apple_local_pipeline_tests PRIVATE minhook ole32 uuid advapi32 avrt)
add_executable(native_terminal_tests "${CMAKE_CURRENT_LIST_DIR}/tests/NativeTerminalTests.cpp"
  src/audio_v2/NativeRenderGateProxy.cpp)
target_include_directories(native_terminal_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(native_terminal_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(native_terminal_tests PRIVATE ole32 uuid avrt)

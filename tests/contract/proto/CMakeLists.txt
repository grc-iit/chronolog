add_executable(proto_contract_check proto_contract_check.cpp)
target_link_libraries(proto_contract_check PRIVATE protobuf::libprotobuf chronolog_warnings)
set(descriptor "${CMAKE_CURRENT_BINARY_DIR}/chronolog.pb")
file(GLOB_RECURSE contract_proto_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/proto/chronolog/*.proto")
add_custom_command(OUTPUT "${descriptor}"
    COMMAND protobuf::protoc --proto_path=${PROJECT_SOURCE_DIR}/proto
        --include_imports --descriptor_set_out=${descriptor} ${contract_proto_files}
    DEPENDS ${contract_proto_files} protobuf::protoc VERBATIM)
add_custom_target(proto_contract_descriptor ALL DEPENDS "${descriptor}")
add_dependencies(proto_contract_check proto_contract_descriptor)
add_test(NAME ProtoContract.AdditiveCompatibility
    COMMAND proto_contract_check compat "${PROJECT_SOURCE_DIR}/proto/baseline/chronolog.v1.m1.pb" "${descriptor}")
add_test(NAME ProtoContract.NamingLint COMMAND proto_contract_check lint "${descriptor}")
add_test(NAME ProtoContract.InternalAdditiveCompatibility
    COMMAND proto_contract_check compat "${CMAKE_CURRENT_SOURCE_DIR}/chronolog.internal.v1.baseline.pb"
        "${descriptor}" chronolog.internal.v1)
# After an approved internal breaking-change RFC, explicitly regenerate and commit this baseline:
# cmake --build --preset dev --target proto_contract_update_internal_baseline
add_custom_target(proto_contract_update_internal_baseline
    COMMAND protobuf::protoc --proto_path=${PROJECT_SOURCE_DIR}/proto --include_imports
        --descriptor_set_out=${CMAKE_CURRENT_SOURCE_DIR}/chronolog.internal.v1.baseline.pb
        ${PROJECT_SOURCE_DIR}/proto/chronolog/internal/v1/internal.proto
    DEPENDS protobuf::protoc VERBATIM)

file(GLOB_RECURSE fixture_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/*.proto")
foreach(source IN LISTS fixture_sources)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}/fixtures" "${source}")
    string(REGEX REPLACE "/.*" "" fixture "${relative}")
    list(APPEND fixtures "${fixture}")
endforeach()
list(REMOVE_DUPLICATES fixtures)
foreach(fixture IN LISTS fixtures)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${fixture}/*.proto")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${fixture}.pb")
    add_custom_command(OUTPUT "${output}"
        COMMAND protobuf::protoc --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${fixture}
            --descriptor_set_out=${output} ${sources}
        DEPENDS ${sources} protobuf::protoc VERBATIM)
    list(APPEND fixture_descriptors "${output}")
endforeach()
add_custom_target(proto_contract_fixtures ALL DEPENDS ${fixture_descriptors})
add_dependencies(proto_contract_check proto_contract_fixtures)

function(proto_fixture fixture mode expected diagnostic)
    if(mode STREQUAL "compat")
        set(inputs "${CMAKE_CURRENT_BINARY_DIR}/baseline.pb;${CMAKE_CURRENT_BINARY_DIR}/${fixture}.pb")
    else()
        set(inputs "${CMAKE_CURRENT_BINARY_DIR}/${fixture}.pb")
    endif()
    add_test(NAME ProtoContract.Fixture.${fixture}
        COMMAND "${CMAKE_COMMAND}" "-Dchecker=$<TARGET_FILE:proto_contract_check>"
            "-Demulator=${CMAKE_CROSSCOMPILING_EMULATOR}" "-Dmode=${mode}" "-Dinputs=${inputs}"
            "-Dexpected=${expected}" "-Ddiagnostic=${diagnostic}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_fixture.cmake")
    set_tests_properties(ProtoContract.Fixture.${fixture} PROPERTIES TIMEOUT 30)
endfunction()
proto_fixture(additive compat 0 "")
proto_fixture(field_removal compat 1 "removed field")
proto_fixture(field_renumber compat 1 "renumbered field")
proto_fixture(field_type compat 1 "changed field type")
proto_fixture(field_cardinality compat 1 "changed field label or presence")
proto_fixture(message_rename compat 1 "removed message")
proto_fixture(enum_removal compat 1 "removed enum")
proto_fixture(enum_value_removal compat 1 "removed or renumbered enum value")
proto_fixture(service_rename compat 1 "removed service")
proto_fixture(method_rename compat 1 "removed RPC")
proto_fixture(package_valid lint 0 "")
proto_fixture(package_defined lint 1 "PACKAGE_DEFINED")
proto_fixture(package_same_directory lint 1 "PACKAGE_SAME_DIRECTORY")
proto_fixture(package_version lint 1 "PACKAGE_VERSION_SUFFIX")
proto_fixture(package_directory lint 1 "PACKAGE_DIRECTORY_MATCH")
proto_fixture(package_snake lint 1 "PACKAGE_LOWER_SNAKE_CASE")
add_test(NAME ContractHeaders.NoGeneratedIncludes
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/contract/include_guard_test.sh" "${PROJECT_SOURCE_DIR}")
add_test(NAME SdkBuild.NoInternalProto
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/contract/sdk_internal_guard_test.sh" "${PROJECT_SOURCE_DIR}")
add_test(NAME ContractLogs.NoPayloadInLogs
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/contract/log_payload_guard_test.sh" "${PROJECT_SOURCE_DIR}")
add_test(NAME ProtoContract.WireTimesAreIntegerNanoseconds
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/wire_time_guard_test.sh" "${PROJECT_SOURCE_DIR}")
set_tests_properties(ProtoContract.AdditiveCompatibility ProtoContract.InternalAdditiveCompatibility ProtoContract.NamingLint
    ContractHeaders.NoGeneratedIncludes SdkBuild.NoInternalProto ContractLogs.NoPayloadInLogs
    ProtoContract.WireTimesAreIntegerNanoseconds PROPERTIES TIMEOUT 30)

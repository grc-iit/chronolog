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
add_test(NAME ContractHeaders.NoGeneratedIncludes
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/contract/include_guard_test.sh" "${PROJECT_SOURCE_DIR}")
add_test(NAME SdkBuild.NoInternalProto
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/contract/sdk_internal_guard_test.sh" "${PROJECT_SOURCE_DIR}")
add_test(NAME ContractLogs.NoPayloadInLogs
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/contract/log_payload_guard_test.sh" "${PROJECT_SOURCE_DIR}")
set_tests_properties(ProtoContract.AdditiveCompatibility ProtoContract.NamingLint
    ContractHeaders.NoGeneratedIncludes SdkBuild.NoInternalProto ContractLogs.NoPayloadInLogs PROPERTIES TIMEOUT 30)

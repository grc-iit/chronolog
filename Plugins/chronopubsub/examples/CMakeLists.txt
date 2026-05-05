# ChronoPubSub runnable examples
add_executable(chronopubsub_publisher_example chronopubsub_publisher_example.cpp)
add_executable(chronopubsub_subscriber_example chronopubsub_subscriber_example.cpp)

target_link_libraries(chronopubsub_publisher_example PRIVATE chronopubsub)
target_link_libraries(chronopubsub_subscriber_example PRIVATE chronopubsub)

# cmd_arg_parse.h lives in chronolog_client's public include dir.
target_include_directories(chronopubsub_publisher_example PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)
target_include_directories(chronopubsub_subscriber_example PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)

set_target_properties(chronopubsub_publisher_example PROPERTIES
    OUTPUT_NAME chrono-chronopubsub-example-publisher
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)
set_target_properties(chronopubsub_subscriber_example PROPERTIES
    OUTPUT_NAME chrono-chronopubsub-example-subscriber
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

chronolog_install_target(chronopubsub_publisher_example DESTINATION examples)
chronolog_install_target(chronopubsub_subscriber_example DESTINATION examples)

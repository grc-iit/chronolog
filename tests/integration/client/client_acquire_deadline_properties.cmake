if(client_acquire_deadline_test_TESTS)
    set_tests_properties(${client_acquire_deadline_test_TESTS} PROPERTIES
        ENVIRONMENT "FI_SOCKETS_IFACE=lo;FI_TCP_IFACE=lo")
endif()

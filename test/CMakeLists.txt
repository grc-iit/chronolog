cmake_minimum_required(VERSION 3.25)

add_subdirectory(unit)
# Integration and communication tests require the old Thallium-based architecture
# and will be rewritten as ChiMod tests. Disabled for now.
# add_subdirectory(integration)
# add_subdirectory(communication)

set(THALLIUM_FOUND TRUE)

if (NOT TARGET thallium::thallium)
    # Include directories
    find_path(THALLIUM_INCLUDE_DIRS mercury.h PATH_SUFFIXES include/ NO_CACHE)
    if (NOT IS_DIRECTORY "${THALLIUM_INCLUDE_DIRS}")
        set(THALLIUM_FOUND FALSE)
    else ()
        #message(${THALLIUM_INCLUDE_DIRS})
        find_path(THALLIUM_LIBRARY_PATH libmercury.so PATH_SUFFIXES lib/)
        find_path(THALLIUM_LIBRARY_MERCURY_PATH libmercury.so PATH_SUFFIXES lib/)
        set(THALLIUM_LIBRARY_LD_PRELOAD ${THALLIUM_LIBRARY_MERCURY_PATH}/libmercury.so)
        #message(${THALLIUM_LIBRARY_LD_PRELOAD})
        #message(${THALLIUM_LIBRARY_PATH})
        set(THALLIUM_LIBRARIES -lmercury -lmercury_util -lmargo -labt)
        set(THALLIUM_DEFINITIONS "")
        add_library(thallium INTERFACE)
        add_library(thallium::thallium ALIAS thallium)
        target_include_directories(thallium INTERFACE ${THALLIUM_INCLUDE_DIRS})
        target_link_libraries(thallium INTERFACE -L${THALLIUM_LIBRARY_PATH} ${THALLIUM_LIBRARIES})
        target_compile_options(thallium INTERFACE ${THALLIUM_DEFINITIONS})
    endif ()
    include(FindPackageHandleStandardArgs)
    # handle the QUIETLY and REQUIRED arguments and set ortools to TRUE
    # if all listed variables are TRUE
    find_package_handle_standard_args(thallium
            REQUIRED_VARS THALLIUM_FOUND THALLIUM_LIBRARIES THALLIUM_INCLUDE_DIRS)
endif ()
#!/bin/bash

# Define your project directories
PROJECT_DIR="~/Documents/Repositories/ChronoLog"
BUILD_DIR="/home/eneko/chronolog"
INSTALL_DIR="/home/eneko/chronolog_install"
VISOR_BIN="/home/eneko/chronolog_install/bin/chronovisor_server"
KEEPER_BIN="/home/eneko/chronolog_install/bin/chrono_keeper"
CLIENT_BIN="/home/eneko/chronolog_install/bin/client_lib_multi_storytellers"
LIB_DIR="/home/eneko/chronolog_install/lib"

#!/bin/bash

# Function to check all .csv files in a given directory
check_csv_files() {
    local DIRECTORY_PATH="$1"

    # Check if directory exists
    if [ ! -d "$DIRECTORY_PATH" ]; then
        echo "Directory does not exist."
        return 1
    fi

    # Function to check a single file
    check_file() {
        local file_path="$1"
        local filename=$(basename -- "$file_path")
        
        # Extract StoryID from the filename, considering IP might contain dots.
        # Using awk to get the second last field when split by dot, which should be StoryID
        local file_story_id=$(echo "$filename" | awk -F. '{print $(NF-2)}')

        local line_numbers=""
        local mismatch_found=0
        local line_count=0

	echo "Analyzing the file: $file_path"

        # Read the file line by line
        while IFS= read -r line; do
            # Increment line count
            ((line_count++))
            
            # Extract StoryID from the line
            local line_story_id=$(echo "$line" | grep -oP 'event : \K[0-9]+')
            
            # Check if StoryIDs match
            if [ "$file_story_id" != "$line_story_id" ]; then
                #echo -e "\e[41mFail: $file_path contains a line with a mismatched StoryID.\e[0m"
		#echo -e "\e[41mTest 1 - Check if StoryId matches: Failed.\e[0m"
                mismatch_found=1
                break
	    #else 
	    	#echo -e "\e[42mTest 1 - Check if StoryId matches: Success.\e[0m"	    
            fi

            # Extract and store line numbers for sequential check
            local current_line_number=$(echo "$line" | grep -oP 'line \K[0-9]+')
            line_numbers="$line_numbers$current_line_number "
        done < "$file_path"

        # Exit if a mismatch was found
        if [ $mismatch_found -eq 1 ]; then
	    echo -e "\t\e[41mTest 1 - Check if StoryId matches: Failed.\e[0m"	
            #return 1
	else
	    echo -e "\t\e[42mTest 1 - Check if StoryId matches: Success.\e[0m"	
	fi

        # Check for exactly 100 lines
        if [ $line_count -ne 100 ]; then
            echo -e "\t\e[41mTest 2 - Check if file contains the expected number of events: Failed.\e[0m"
            #return 1
	else
	    echo -e "\t\e[42mTest 2 - Check if file contains the expected number of events: Success.\e[0m"
        fi

        # Check if lines are sequential from 0 to 99
        local expected_lines=$(seq 0 99 | tr '\n' ' ')
        if [ "$line_numbers" == "$expected_lines" ]; then
	    echo -e "\t\e[42mTest 3 - Lines in sequential order: Success.\e[0m"
            #echo "Success: $file_path contains 100 lines in correct sequential order with matching StoryID."
        else
            echo -e "\t\e[41mTest 3 - Lines in sequential order: Failed.\e[0m"
            return 1
        fi
    }

    # Loop over all CSV files in the directory and check them
    for file in "$DIRECTORY_PATH"/*.csv; do
        check_file "$file"
    done
}

# Methods
extract_shared_libraries() {
    local executable="$1"
    ldd_output=$(ldd ${executable} 2>/dev/null | grep '=>' | awk '{print $3}' | grep -v 'not' | grep -v '^/lib')
    echo "${ldd_output}"
}

copy_shared_libs_recursive() {
    local lib_path="$1"
    local dest_path="$2"
    local linked_to_lib_path="$(readlink -f ${lib_path})"

    # Copy the library and maintain symbolic links recursively
    final_dest_lib_copies=false
    echo -e "${DEBUG}Copying ${lib_path} recursively ...${NC}"
    while [ "$final_dest_lib_copies" != true ]
    do
        cp -P "$lib_path" "$dest_path/"
        if [ "$lib_path" == "$linked_to_lib_path" ]
        then
            final_dest_lib_copies=true
        fi
        lib_path="$linked_to_lib_path"
        linked_to_lib_path="$(readlink -f ${lib_path})"
    done
}

copy_shared_libs() {
    libs_visor=$(extract_shared_libraries ${VISOR_BIN})
    libs_keeper=$(extract_shared_libraries ${KEEPER_BIN})
    libs_client=$(extract_shared_libraries ${CLIENT_BIN})

    # Combine shared libraries from all executables and remove duplicates
    all_shared_libs=$(echo -e "${libs_visor}\n${libs_keeper}\n${libs_client}" | sort | uniq)

    # Copy shared libraries to the lib directory
    echo -e "${DEBUG}Copying shared library ...${NC}"
    mkdir -p ${LIB_DIR}
    for lib in ${all_shared_libs}; do
        if [[ ! -z ${lib} ]]
        then
            copy_shared_libs_recursive ${lib} ${LIB_DIR}
        fi
    done
}

# Function to perform Message Generation Consistency Test
test_message_generation_consistency() {
    echo "[ $(date +'%Y-%m-%d %H:%M:%S') ] Starting Test 1: Message Generation Consistency Test"

    TEST_DIR="/tmp/"  # Make sure to adjust this path
    all_tests_passed=true

    while read file; do
        if [ $(tail -n +2 "$file" | awk -F, '{print $1}' | grep -P '^line \d+$' | sort | uniq | wc -l) -eq 100 ] &&
           [ $(tail -n +2 "$file" | awk -F, '{print $1}' | sort | uniq -c | grep -v ' 1 ' | wc -l) -eq 0 ]; then
            echo "[ $(date +'%Y-%m-%d %H:%M:%S') ] $file: Passed"
        else
            echo "[ $(date +'%Y-%m-%d %H:%M:%S') ] $file: Failed"
            all_tests_passed=false
        fi
    done < <(find "$TEST_DIR" -type f -name '*.csv')

    if [ "$all_tests_passed" = true ]; then
        echo "[ $(date +'%Y-%m-%d %H:%M:%S') ] All Message Generation Consistency Tests Passed."
    else
        echo "[ $(date +'%Y-%m-%d %H:%M:%S') ] Some Message Generation Consistency Tests Failed."
    fi
}


echo -e "\033[34m[ $(date +'%Y-%m-%d %H:%M:%S') ] Script Execution Started\033[0m"

# Create the build & install directories if they don't already exist
mkdir -p "${BUILD_DIR}"
mkdir -p "${INSTALL_DIR}"

# Change to the build directory
cd "${BUILD_DIR}"
echo -e "\033[34m[ $(date +'%Y-%m-%d %H:%M:%S') ] Changed directory to BUILD_DIR\033[0m"

# Run CMake to configure the project for debugging
# Adjust the CMake command according to your project's requirements
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
      "${PROJECT_DIR}"

# Build the project automatically after configuring it
make

# Install the project
make install

# Copy shared libraries
copy_shared_libs
echo -e "\033[34m[ $(date +'%Y-%m-%d %H:%M:%S') ] Shared libraries copied\033[0m"
sleep 3

# Navigate to installation directory
cd "${INSTALL_DIR}"

# Define LD_LIBRARY_PATH
export LD_LIBRARY_PATH=${LIB_DIR}

# Run ChronoVisor
bin/chronovisor_server -c conf/default_conf.json &
CHRONOVISOR_PID=$!
sleep 2

# Run ChronoKeeper
bin/chrono_keeper -c conf/default_conf.json &
CHRONOKEEPER_PID=$!
sleep 2

# Run Client Test
echo -e "\033[32mRunning the client...\033[0m"
bin/client_lib_multi_storytellers -c conf/default_conf.json
echo -e "\033[32mClient finished.\033[0m"

# Once the client test ends, send a SIGTERM to ChronoKeeper
kill ${CHRONOKEEPER_PID}
wait ${CHRONOKEEPER_PID}
echo -e "\033[34m[ $(date +'%Y-%m-%d %H:%M:%S') ] ChronoKeeper terminated\033[0m"

# Once the client and keeper are finished, send a SIGTERM to ChronoVisor
kill ${CHRONOVISOR_PID}
wait ${CHRONOVISOR_PID}
echo -e "\033[34m[ $(date +'%Y-%m-%d %H:%M:%S') ] ChronoVisor terminated\033[0m"

# Test 1
# Run the Message Generation Consistency Test
check_csv_files "/tmp"

# Final completion message
echo -e "\033[32m[ $(date +'%Y-%m-%d %H:%M:%S') ] Test completed successfully.\033[0m"

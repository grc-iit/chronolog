#!/bin/bash

# Define project directories
PROJECT_DIR="$HOME/Documents/Repositories/ChronoLog"
BUILD_DIR="$HOME/chronolog"
INSTALL_DIR="$HOME/chronolog_install"
VISOR_BIN="$INSTALL_DIR/bin/chronovisor_server"
KEEPER_BIN="$INSTALL_DIR/bin/chrono_keeper"
CLIENT_BIN="$INSTALL_DIR/bin/client_lib_multi_storytellers"
LIB_DIR="$INSTALL_DIR/lib"
CSV_FILES_DIR="/tmp"

# Check .csv files in a directory
check_csv_files() {
    local dir_path="$1"

    if [ ! -d "$dir_path" ]; then
        echo "Directory does not exist."
        return 1
    fi

    for file in "$dir_path"/*.csv; do
        check_file "$file"
    done
}

# Check a single .csv file for specific conditions
check_file() {
    local file_path="$1"
    local filename=$(basename -- "$file_path")
    local file_story_id=$(echo "$filename" | awk -F. '{print $(NF-2)}')
    local line_numbers="" mismatch_found=0 line_count=0

    echo "Analyzing the file: $file_path"

    while IFS= read -r line; do
        ((line_count++))
        local line_story_id=$(echo "$line" | grep -oP 'event : \K[0-9]+')

        if [ "$file_story_id" != "$line_story_id" ]; then
            echo -e "\tTest 1 - StoryID match: \e[41mFailed.\e[0m"
            mismatch_found=1
            break
        fi

        local current_line_number=$(echo "$line" | grep -oP 'line \K[0-9]+')
        line_numbers="$line_numbers$current_line_number "
    done < "$file_path"

    if [ $mismatch_found -eq 0 ]; then
        echo -e "\tTest 1 - StoryID match: \e[42mSuccess.\e[0m"
    fi

    if [ $line_count -ne 100 ]; then
        echo -e "\tTest 2 - Expected events: \e[41mFailed.\e[0m"
    else
        echo -e "\tTest 2 - Expected events: \e[42mSuccess.\e[0m"
    fi

    local expected_lines=$(seq 0 99 | tr '\n' ' ')
    if [ "$line_numbers" == "$expected_lines" ]; then
        echo -e "\tTest 3 - Sequential order: \e[42mSuccess.\e[0m"
    else
        echo -e "\tTest 3 - Sequential order: \e[41mFailed.\e[0m"
    fi
}

# Extract and copy shared libraries recursively
extract_shared_libraries() { ldd "$1" 2>/dev/null | awk '/=>/ {print $3}' | grep -vE '^/(lib|not)'; }
copy_shared_libs_recursive() {
    local lib_path="$1" dest_path="$2" linked_to_lib_path
    linked_to_lib_path="$(readlink -f "$lib_path")"

    while [ "$lib_path" != "$linked_to_lib_path" ]; do
        cp -P "$lib_path" "$dest_path/"
        lib_path="$linked_to_lib_path"
        linked_to_lib_path="$(readlink -f "$lib_path")"
    done
    cp -P "$lib_path" "$dest_path/"
}
copy_shared_libs() {
    mkdir -p "$LIB_DIR"
    local libs lib
    for lib in $VISOR_BIN $KEEPER_BIN $CLIENT_BIN; do
        libs+=$(extract_shared_libraries "$lib")$'\n'
    done
    libs=$(echo "$libs" | sort -u)
    for lib in $libs; do
        [ -n "$lib" ] && copy_shared_libs_recursive "$lib" "$LIB_DIR"
    done
}

# Build and install the project
build_and_install() {
    mkdir -p "$BUILD_DIR" "$INSTALL_DIR"
    cd "$BUILD_DIR"
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" "$PROJECT_DIR"
    make && make install
}

# Run the system and execute tests
run_system_and_tests() {
    export LD_LIBRARY_PATH="$LIB_DIR"
    "$VISOR_BIN" -c conf/default_conf.json &
    CHRONOVISOR_PID=$!
    sleep 1
    "$KEEPER_BIN" -c conf/default_conf.json &
    CHRONOKEEPER_PID=$!
    sleep 1

    echo "Running the client..."
    "$CLIENT_BIN" -c conf/default_conf.json
    echo "Client finished."

    kill "$CHRONOKEEPER_PID" && wait "$CHRONOKEEPER_PID"
    kill "$CHRONOVISOR_PID" && wait "$CHRONOVISOR_PID"
    check_csv_files "${CSV_FILES_DIR}"
}

# Main script execution
echo -e "\033[34m[ $(date +'%Y-%m-%d %H:%M:%S') ] Script Execution Started\033[0m"
build_and_install
copy_shared_libs
run_system_and_tests
echo -e "\033[32m[ $(date +'%Y-%m-%d %H:%M:%S') ] Test completed successfully.\033[0m"


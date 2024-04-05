#!/bin/bash

# Default paths
default_project_dir="$HOME/Documents/Repositories/ChronoLog"
default_build_dir="$HOME/chronolog"
default_install_dir="$HOME/chronolog_install"
default_csv_files_dir="/tmp"

# Assign arguments with default fallbacks
PROJECT_DIR="${1:-$default_project_dir}"
BUILD_DIR="${2:-$default_build_dir}"
INSTALL_DIR="${3:-$default_install_dir}"
CSV_FILES_DIR="${4:-$default_csv_files_dir}"

VISOR_BIN="$INSTALL_DIR/bin/chronovisor_server"
KEEPER_BIN="$INSTALL_DIR/bin/chrono_keeper"
CLIENT_BIN="$INSTALL_DIR/bin/client_lib_multi_storytellers"
LIB_DIR="$INSTALL_DIR/lib"
CONFIG_FILE="$INSTALL_DIR/conf/default_conf.json"

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
            echo -e "\t\e[41mTest 1 - StoryID match: Failed.\e[0m"
            mismatch_found=1
            break
        fi

        local current_line_number=$(echo "$line" | grep -oP 'line \K[0-9]+')
        line_numbers="$line_numbers$current_line_number "
    done < "$file_path"

    if [ $mismatch_found -eq 0 ]; then
        echo -e "\t\e[42mTest 1 - StoryID match: Success.\e[0m"
    fi

    if [ $line_count -ne 100 ]; then
        echo -e "\t\e[41mTest 2 - Expected events: Failed.\e[0m"
    else
        echo -e "\t\e[42mTest 2 - Expected events: Success.\e[0m"
    fi

    local expected_lines=$(seq 0 99 | tr '\n' ' ')
    if [ "$line_numbers" == "$expected_lines" ]; then
        echo -e "\t\e[42mTest 3 - Sequential order: Success.\e[0m"
    else
        echo -e "\t\e[41mTest 3 - Sequential order: Failed.\e[0m"
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
    "$VISOR_BIN" -c "${CONFIG_FILE}" &
    CHRONOVISOR_PID=$!
    sleep 1
    "$KEEPER_BIN" -c "${CONFIG_FILE}" &
    CHRONOKEEPER_PID=$!
    sleep 1

    echo "Running the client..."
    "$CLIENT_BIN" -c "${CONFIG_FILE}"
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


#!/bin/bash

RED='\e[41m'
GREEN='\e[42m'
NC='\033[0m' # No Color
BOLD='\033[1m'
UNDERLINE='\033[4m'

print_header() {
    echo
    echo "**************************************************"
    echo "* Starting Fidelity Test 03 with path $directory_path"
    echo "**************************************************"
    echo "Test 04: Checks if the events have been written in sequential order."
    echo
}

# Check if the correct number of arguments is given
if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <original_file_path> <after_system_file_path>"
    exit 1
fi

function check_line_order {
    # Assign input parameters to variables
    local original_file="$1"
    local after_system_file="$2"

    # Extract lines from the after system file
    local lines_from_after=$(grep -o 'line [0-9]*' "$after_system_file")

    # Initialize the last found line index
    local last_index=-1
    local error_occurred=0

    # Set Internal Field Separator to newline to handle each line separately
    local IFS=$'\n'

    # Read each extracted line
    while read -r line; do
        # Get the line number from the line
        local line_num=$(echo "$line" | grep -o '[0-9]*')

        # Check if the line exists in the original file and get its line number
        local current_index=$(grep -n "^$line$" "$original_file" | cut -d: -f1)

        # Ensure only the first index is considered if multiple are found
        current_index=$(echo "$current_index" | head -n1)

        # If line is found and the line index is greater than the last found line index
        if [[ -n $current_index && $current_index -gt $last_index ]]; then
            last_index=$current_index
        else
            error_occurred=1
            break
        fi
    done <<< "$lines_from_after"

    local filename=$(basename -- "$after_system_file")
    local mod_datetime=$(stat -c %y "$after_system_file" | cut -d '.' -f 1)

    echo -e "${BOLD}\tFile:${NC} $filename"
    echo -e "${BOLD}\tDate:${NC} $mod_datetime"

    # Final check if there were no errors
    if [[ $error_occurred -eq 0 ]]; then
        echo -e "\t${BOLD}Result:${NC} ${GREEN}Success.${NC}\n"
    else
        echo -e "\t${BOLD}Result:${NC} ${RED}Failed.${NC}\n"
    fi
}

check_csv_files() {
    local dir_path="$1"

    if [ ! -d "$dir_path" ]; then
        echo -e "${RED}Directory does not exist.${NC}"
        return 1
    fi

    echo -e "${UNDERLINE}Checking CSV files in directory: $dir_path${NC}\n"

    for file in "$dir_path"/*.csv; do
        check_line_order "$original_file" "$file"
    done
}

# Execution ____________________________________________________________________________________________________________
original_file="$1"
output_path="$2"
print_header
check_csv_files "$output_path"

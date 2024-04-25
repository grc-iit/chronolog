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
    echo "Test 03: Checks if the events have been written in sequential order."
    echo
}

# Check if the correct number of arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <input_file> <directory>"
    exit 1
fi

verify_order() {
    local file1="$1"
    local file2="$2"

    # Check if both files are specified
    if [[ -z "$file1" || -z "$file2" ]]; then
        echo "Error: Two file paths must be provided"
        return 1
    fi

    # Read file1 line by line
    local current_line_number=0
    local found
    local last_line_number=0


    while IFS= read -r line_from_file1; do
        found=0
        # Increment to move to the next line to be checked after a successful match
        ((current_line_number++))
        local line_num=$(echo "$line_from_file1" | cut -d ':' -f6)

        # Search for the line in file2 starting from the last matched line
        while IFS= read -r line_from_file2; do
            if [[ "$line_num" == "$line_from_file2" ]]; then
                found=1
                # Update last_line_number to the line number of the matched line in file2
                last_line_number="$current_line_number"
                break
            fi
            ((current_line_number++))
        done < <(tail -n +"$current_line_number" "$file2")

        # If no match is found, the test fails
        if [[ $found -eq 0 ]]; then
            echo -e "\t${BOLD}Result:${NC} ${RED}Failed.${NC}\n"
            return 1
        fi
    done < "$file1"

    echo -e "\t${BOLD}Result:${NC} ${GREEN}Success.${NC}\n"
    return 0
}

check_csv_files() {
    local dir_path="$1"

    if [ ! -d "$dir_path" ]; then
        echo -e "${RED}Directory does not exist.${NC}"
        return 1
    fi

    echo -e "${UNDERLINE}Checking CSV files in directory: $dir_path${NC}\n"

    for file in "$dir_path"/*.csv; do
        local filename=$(basename -- "$file")
        local mod_datetime=$(stat -c %y "$file" | cut -d '.' -f 1)

        echo -e "${BOLD}\tFile:${NC} $filename"
        echo -e "${BOLD}\tDate:${NC} $mod_datetime"

        verify_order "$file" "$original_file"
    done
}

# Execution ____________________________________________________________________________________________________________
original_file="$1"
output_path="$2"
print_header
check_csv_files "$output_path"

#!/bin/bash

# Use ANSI color codes for better visibility
RED='\e[41m'
GREEN='\e[42m'
NC='\033[0m' # No Color
BOLD='\033[1m'
UNDERLINE='\033[4m'

# Function to check all .csv files in a directory
check_csv_files() {
    local dir_path="$1"

    if [ ! -d "$dir_path" ]; then
        echo -e "${RED}Directory does not exist.${NC}"
        return 1
    fi

    echo -e "${UNDERLINE}Checking CSV files in directory: $dir_path${NC}\n"

    for file in "$dir_path"/*.csv; do
        check_sequential_order "$file"
    done
}

# Function to verify that event numbers are sequential
check_sequential_order() {
    local file_path="$1"
    local line_numbers=""
    local filename=$(basename -- "$file_path")
    local mod_datetime=$(stat -c %y "$file_path" | cut -d '.' -f 1)

    echo -e "${BOLD}\tFile:${NC} $filename"
    echo -e "${BOLD}\tDate:${NC} $mod_datetime"

    while IFS= read -r line; do
        local current_line_number=$(echo "$line" | grep -oP 'line \K[0-9]+')
        line_numbers="$line_numbers$current_line_number "
    done < "$file_path"

    local expected_lines=$(seq 0 99 | tr '\n' ' ')
    if [ "$line_numbers" == "$expected_lines" ]; then
        echo -e "\t${BOLD}Result:${NC} ${GREEN}Success.${NC}\n"
    else
        echo -e "\t${BOLD}Result:${NC} ${RED}Failed.${NC}\n"
    fi
}

# Main script execution
if [ $# -eq 0 ]; then
    echo -e "${RED}Usage: $0 <directory_path>${NC}"
    exit 1
fi

directory_path="$1"
check_csv_files "$directory_path"

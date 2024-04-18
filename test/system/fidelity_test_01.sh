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
        check_story_id "$file"
    done
}

# Function to check the StoryID matching for a single file
check_story_id() {
    local file_path="$1"
    local filename=$(basename -- "$file_path")
    local file_story_id=$(echo "$filename" | awk -F. '{print $(NF-2)}')

    # Retrieve the last modification date of the file
    local mod_datetime=$(stat -c %y "$file_path" | cut -d '.' -f 1)

    echo -e "${BOLD}\tFile:${NC} $file_path"
    echo -e "${BOLD}\tDate:${NC} $mod_datetime"

    while IFS= read -r line; do
        local line_story_id=$(echo "$line" | grep -oP 'event : \K[0-9]+')

        if [ "$file_story_id" != "$line_story_id" ]; then
            echo -e "\t${BOLD}Result:${NC} ${RED}Failed.${NC}\n"
            return
        fi
    done < "$file_path"

    echo -e "\t${BOLD}Result:${NC} ${GREEN}Success.${NC}\n"
}

# Main script execution
if [ $# -eq 0 ]; then
    echo -e "${RED}Usage: $0 <directory_path>${NC}"
    exit 1
fi

directory_path="$1"
check_csv_files "$directory_path"

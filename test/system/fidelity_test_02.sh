#!/bin/bash

# Use ANSI color codes for better visibility
RED='\e[41m'
GREEN='\e[42m'
NC='\033[0m' # No Color
BOLD='\033[1m'
UNDERLINE='\033[4m'

print_header() {
    echo
    echo "**************************************************"
    echo "* Starting Fidelity Test 02 with path $directory_path"
    echo "**************************************************"
    echo "Test 02: Checks if the number of recorded events is the expected one."
    echo
}

# Check if the correct number of arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <directory> <input_file>"
    exit 1
fi


verify_num_event () {
    # Assigning the directory and the input file from arguments
    local file="$1"
    local dir="$2"

    # Validate that the input file and dir exist
    if [ ! -d "$dir" ]; then
        echo "Directory does not exist."
        exit 1
    fi

    if [ ! -f "$file" ]; then
        echo "Input file does not exist."
        exit 1
    fi

    # Get the number of lines in the input file
    file_lines=$(wc -l < "$file")

    # Initialize associative array to store total line counts per storyId
    declare -A line_counts

    # Loop through all csv files in the directory
    for file in "$dir"/*.csv; do
        # Extract storyId from filename
        filename=$(basename -- "$file")
        storyId=$(echo "$filename" | cut -d '.' -f1)

        # Count the number of lines in the current file
        current_lines=$(wc -l < "$file")

        # Add the current line count to the total line count for the storyId
        line_counts[$storyId]=$((line_counts[$storyId] + current_lines))
    done

    # Compare and print the results for each storyId
    for storyId in "${!line_counts[@]}"; do
        local mod_datetime=$(stat -c %y "$file" | cut -d '.' -f 1)

        #echo -e "${BOLD}\tDate:${NC} $mod_datetime"
        echo -e "${BOLD} \tStory ID:${NC}  $storyId"
        echo -e "${BOLD} \tNumber of expected lines:${NC} ${file_liness}"
        echo -e "${BOLD} \tNumber of lines:${NC} ${line_counts[$storyId]}"

        if [ "${line_counts[$storyId]}" -eq "$file_lines" ]; then
            #echo "Story ID $storyId Result: Success"
            echo -e "\t${BOLD}Result:${NC} ${GREEN}Success.${NC}\n"
        else
            echo -e "\t${BOLD}Result:${NC} ${RED}Failed.${NC}\n"
            #echo "Story ID $storyId Result: Failed"
        fi
    done
}


# Execution____________________________________________________
directory="$2"
input_file="$1"
print_header
verify_num_event "$input_file" "$directory"

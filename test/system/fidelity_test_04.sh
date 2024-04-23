#!/bin/bash

# Check if the correct number of arguments is given
if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <original_file_path> <after_system_file_path>"
    exit 1
fi

# Assign input parameters to variables
original_file="$1"
after_system_file="$2"

# Extract lines from the after system file
lines_from_after=$(grep -o 'line [0-9]*' "$after_system_file")

# Initialize the last found line index
last_index=-1
error_occurred=0

# Set Internal Field Separator to newline to handle each line separately
IFS=$'\n'

# Read each extracted line
while read -r line; do
    # Get the line number from the line
    line_num=$(echo "$line" | grep -o '[0-9]*')

    # Check if the line exists in the original file and get its line number
    current_index=$(grep -n "^$line$" "$original_file" | cut -d: -f1)

    # Ensure only the first index is considered if multiple are found
    current_index=$(echo "$current_index" | head -n1)

    # If line is found and the line index is greater than the last found line index
    if [[ -n $current_index && $current_index -gt $last_index ]]; then
        last_index=$current_index
    else
        echo "Error: Line '$line' does not appear in order or is missing in the original file."
        error_occurred=1
        break
    fi
done <<< "$lines_from_after"

# Final check if there were no errors
if [[ $error_occurred -eq 0 ]]; then
    echo "Success: All events are in order and match the original file."
fi

import os
import sys
from collections import defaultdict

# ANSI color codes for better visibility
RED = '\033[41m'
GREEN = '\033[42m'
NC = '\033[0m'  # No Color
BOLD = '\033[1m'
UNDERLINE = '\033[4m'

def print_header(input_file, directory):
    print()
    print("**************************************************")
    print(f"* Starting Fidelity Test 02 with path {directory}")
    print("**************************************************")
    print("Test 02: Checks if the number of recorded events is the expected one.")
    print()
    print(f"{UNDERLINE}Checking CSV files in directory: {directory}{NC}\n")
    print(f"{UNDERLINE}Using as reference input file: {input_file}{NC}\n")
    print()

def verify_num_event(input_file, directory):
    # Validate that the input file and dir exist
    if not os.path.isdir(directory):
        print(f"{RED}Directory does not exist.{NC}")
        sys.exit(1)

    if not os.path.isfile(input_file):
        print(f"{RED}Input file does not exist.{NC}")
        sys.exit(1)

    # Get the number of lines in the input file
    with open(input_file, 'r') as file:
        file_lines = sum(1 for line in file)

    # Initialize dictionary to store total line counts per storyId
    line_counts = defaultdict(int)

    # Loop through all csv files in the directory
    for file_name in os.listdir(directory):
        if file_name.endswith(".csv"):
            file_path = os.path.join(directory, file_name)
            story_id = file_name.split('.')[0]  # Extract storyId from filename

            with open(file_path, 'r') as file:
                current_lines = sum(1 for line in file)

            # Add the current line count to the total line count for the storyId
            line_counts[story_id] += current_lines

    # Compare and print the results for each storyId
    for story_id, count in line_counts.items():
        print(f"{BOLD}\tStory ID:{NC} {story_id}")
        print(f"{BOLD}\tNumber of expected lines:{NC} {file_lines}")
        print(f"{BOLD}\tNumber of lines:{NC} {count}")

        if count == file_lines:
            print(f"\t{BOLD}Result:{NC} {GREEN}Success.{NC}\n")
        else:
            print(f"\t{BOLD}Result:{NC} {RED}Failed.{NC}\n")

def main():
    if len(sys.argv) != 3:
        print(f"{RED}Error: Missing Arguments{NC}")
        print("Usage: python fidelity_test_02.py <input_file> <directory>")
        print("  <input_file>: The file that has been processed by the system.")
        print("  <directory>: The destination directory for the generated files.")
        print()
        sys.exit(1)

    input_file = sys.argv[1]
    directory = sys.argv[2]
    print_header(input_file, directory)
    verify_num_event(input_file, directory)

if __name__ == "__main__":
    main()

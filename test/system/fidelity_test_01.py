import os
import sys
import re
import datetime

# ANSI color codes for better visibility
RED = '\033[41m'
GREEN = '\033[42m'
NC = '\033[0m'  # No Color
BOLD = '\033[1m'
UNDERLINE = '\033[4m'

def print_header(directory):
    print()
    print("**************************************************")
    print(f"* Starting Fidelity Test 01 with path {directory}")
    print("**************************************************")
    print("Test 01: Checks if the StoryID of the file and the one in the events written inside match.")
    print()
    print(f"{UNDERLINE}Checking CSV files in directory: {directory}{NC}\n")
    print()


def check_csv_files(directory):
    if not os.path.isdir(directory):
        print(f"{RED}Directory does not exist.{NC}")
        return False

    for file in os.listdir(directory):
        if file.endswith(".csv"):
            check_story_id(os.path.join(directory, file))
    return True

def check_story_id(file_path):
    filename = os.path.basename(file_path)
    file_story_id = filename.split('.')[0]  # Get the element before the first dot

    # Retrieve the last modification date of the file
    mod_datetime = datetime.datetime.fromtimestamp(os.path.getmtime(file_path)).strftime('%Y-%m-%d %H:%M:%S')

    print(f"{BOLD}\tFile:{NC} {file_path}")
    print(f"{BOLD}\tDate:{NC} {mod_datetime}")

    with open(file_path, 'r') as file:
        for line in file:
            match = re.search(r'event : (\d+)', line)
            if match:
                line_story_id = match.group(1)
                if file_story_id != line_story_id:
                    print(f"\t{BOLD}Result:{NC} {RED}Failed.{NC}\n")
                    return
    print(f"\t{BOLD}Result:{NC} {GREEN}Success.{NC}\n")

def main():
    if len(sys.argv) != 2:
        print(f"{RED}Fidelity Test 01: Missing Arguments{NC}")
        print("Usage: python fidelity_test_01.py <directory>")
        print("  <directory>: The destination directory for the generated files.")
        print()
        sys.exit(1)
    directory = sys.argv[1]
    print_header(directory)
    check_csv_files(directory)

if __name__ == "__main__":
    main()

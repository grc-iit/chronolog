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

def print_header(directory_path):
    print()
    print("**************************************************")
    print(f"* Starting Fidelity Test 01 with path {directory_path}")
    print("**************************************************")
    print("Test 01: Checks if the StoryID of the file and the one in the events written inside match.")
    print()

def check_csv_files(directory_path):
    if not os.path.isdir(directory_path):
        print(f"{RED}Directory does not exist.{NC}")
        return False

    print(f"{UNDERLINE}Checking CSV files in directory: {directory_path}{NC}\n")

    for file in os.listdir(directory_path):
        if file.endswith(".csv"):
            check_story_id(os.path.join(directory_path, file))
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
        print(f"{RED}Usage: python script.py <directory_path>{NC}")
        sys.exit(1)
    directory_path = sys.argv[1]
    print_header(directory_path)
    check_csv_files(directory_path)

if __name__ == "__main__":
    main()

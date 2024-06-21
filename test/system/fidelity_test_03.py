import os
import sys

# ANSI color codes for better visibility
RED = '\033[41m'
GREEN = '\033[42m'
NC = '\033[0m'  # No Color
BOLD = '\033[1m'
UNDERLINE = '\033[4m'

def print_header(directory_path):
    print()
    print("**************************************************")
    print(f"* Starting Fidelity Test 03 with path {directory_path}")
    print("**************************************************")
    print("Test 03: Checks if the events have been written in sequential order.")
    print()

def verify_order(file1, file2):
    # Check if both files are specified
    if not file1 or not file2:
        print("Error: Two file paths must be provided")
        return False

    # Read file1 line by line
    current_line_number = 0

    with open(file1, 'r') as f1:
        lines_file1 = f1.readlines()

    with open(file2, 'r') as f2:
        lines_file2 = f2.readlines()

    for line_from_file1 in lines_file1:
        current_line_number += 1
        line_num = line_from_file1.split(':')[5].strip()
        found = False

        for line_from_file2 in lines_file2[current_line_number-1:]:
            if line_num == line_from_file2.strip():
                found = True
                break
            current_line_number += 1

        if not found:
            print(f"\t{BOLD}Result:{NC} {RED}Failed.{NC}\n")
            return False

    print(f"\t{BOLD}Result:{NC} {GREEN}Success.{NC}\n")
    return True

def check_csv_files(directory_path, original_file):
    if not os.path.isdir(directory_path):
        print(f"{RED}Directory does not exist.{NC}")
        return False

    print(f"{UNDERLINE}Checking CSV files in directory: {directory_path}{NC}\n")

    for file_name in os.listdir(directory_path):
        if file_name.endswith(".csv"):
            file_path = os.path.join(directory_path, file_name)
            mod_datetime = os.path.getmtime(file_path)

            print(f"{BOLD}\tFile:{NC} {file_name}")
            print(f"{BOLD}\tDate:{NC} {mod_datetime}")

            verify_order(file_path, original_file)

def main():
    if len(sys.argv) != 3:
        print(f"{RED}Usage: python script.py <input_file> <directory>{NC}")
        sys.exit(1)

    original_file = sys.argv[1]
    directory_path = sys.argv[2]
    print_header(directory_path)
    check_csv_files(directory_path, original_file)

if __name__ == "__main__":
    main()

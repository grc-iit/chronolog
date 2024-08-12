import os
import sys

# ANSI color codes for better visibility
RED = '\033[41m'
GREEN = '\033[42m'
NC = '\033[0m'  # No Color
BOLD = '\033[1m'
UNDERLINE = '\033[4m'

def print_header(input_file, directory):
    print()
    print("**************************************************")
    print(f"* Starting Fidelity Test 03 with path {directory}")
    print("**************************************************")
    print("Test 03: Checks if the events have been written in sequential order.")
    print()
    print(f"{UNDERLINE}Checking CSV files in directory: {directory}{NC}\n")
    print(f"{UNDERLINE}Using as reference input file: {input_file}{NC}\n")
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

def check_csv_files(directory, input_file):
    if not os.path.isdir(directory):
        print(f"{RED}Directory does not exist.{NC}")
        return False

    for file_name in os.listdir(directory):
        if file_name.endswith(".csv"):
            file_path = os.path.join(directory, file_name)
            mod_datetime = os.path.getmtime(file_path)

            print(f"{BOLD}\tFile:{NC} {file_name}")
            print(f"{BOLD}\tDate:{NC} {mod_datetime}")

            verify_order(file_path, input_file)

def main():
    if len(sys.argv) != 3:
        print(f"{RED}Error: Missing Arguments{NC}")
        print("Usage: python fidelity_test_03.py <input_file> <directory>")
        print("  <input_file>: The file that has been processed by the system.")
        print("  <directory>: The destination directory for the generated files.")
        print()
        sys.exit(1)

    input_file = sys.argv[1]
    directory = sys.argv[2]
    print_header(input_file, directory)
    check_csv_files(directory, input_file)

if __name__ == "__main__":
    main()

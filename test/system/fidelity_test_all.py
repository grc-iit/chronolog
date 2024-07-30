import subprocess
import sys
import os

RED = '\033[41m'
NC = '\033[0m'  # No Color

def check_files_exist(test_files, test_files_directory):
    for file in test_files:
        if not os.path.isfile(os.path.join(test_files_directory, file)):
            return False
    return True

def usage():
    print("Usage: python fidelity_test_all.py <input_file> <directory> [<test_files_directory>]")
    print("  <input_file>: The file that has been processed by the system.")
    print("  <directory>: The destination directory for the generated files.")
    print("  <test_files_directory> (optional): The directory where the fidelity test files are located.")

# Check if the correct number of arguments are provided
if len(sys.argv) < 3 or len(sys.argv) > 4:
    print(f"{RED}Error: Missing Arguments{NC}")
    usage()
    sys.exit(1)

# Store the path parameters
original_file = sys.argv[1]
directory = sys.argv[2]
test_files_directory = sys.argv[3] if len(sys.argv) == 4 else "."

test_files = ["fidelity_test_01.py", "fidelity_test_02.py", "fidelity_test_03.py"]

if not check_files_exist(test_files, test_files_directory):
    print(f"{RED}Error: One or more fidelity test files are missing in the directory: {test_files_directory}{NC}")
    print("Please specify the correct directory containing the fidelity test files.")
    usage()
    sys.exit(1)

subprocess.run(["python3", os.path.join(test_files_directory, "fidelity_test_01.py"), directory])
subprocess.run(["python3", os.path.join(test_files_directory, "fidelity_test_02.py"), original_file, directory])
subprocess.run(["python3", os.path.join(test_files_directory, "fidelity_test_03.py"), original_file, directory])

print("Execution completed.")

import subprocess
import sys

RED = '\033[41m'
NC = '\033[0m'  # No Color

# Check if the correct number of arguments are provided
if len(sys.argv) != 3:
    print(f"{RED}Fidelity Test All: Missing Arguments{NC}")
    print("Usage: python fidelity_test_all.py <input_file> <directory>")
    print("  <input_file>: The file that has been processed by the system.")
    print("  <directory>: The destination directory for the generated files.")
    sys.exit(1)

# Store the path parameters
original_file = sys.argv[1]
directory = sys.argv[2]

subprocess.run(["python3", "fidelity_test_01.py", directory])
subprocess.run(["python3", "fidelity_test_02.py", original_file, directory])
subprocess.run(["python3", "fidelity_test_03.py", original_file, directory])

print("Execution completed.")

import subprocess
import glob
import os

# Base paths
REPO_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../"))
DEPLOY_PATH = os.path.join(REPO_PATH, "deploy")
INSTALL_PATH = os.path.expanduser("~/chronolog/Release")
BIN_PATH = os.path.join(INSTALL_PATH, "bin")
OUTPUT_PATH = os.path.join(INSTALL_PATH, "output")
CONF_PATH = os.path.join(INSTALL_PATH, "conf")

# ANSI color codes
GREEN_HIGHLIGHT = "\033[42m"  # Green background for Success
RED_HIGHLIGHT = "\033[41m"    # Red background for Failure
RESET = "\033[0m"

# Utility Functions
def run_command(command, shell=False):
    """
    Executes a shell command and prints output.
    """
    try:
        result = subprocess.run(command, shell=shell, check=True, executable="/bin/bash" if shell else None, text=True)
        print(result.stdout)
    except subprocess.CalledProcessError as e:
        print(f"Command failed with error: {e.stderr}")


def generate_input_file():
    """
    Creates a file with chronicle identifiers for testing.
    """
    temp_input_path = os.path.join(BIN_PATH, "temp_input")
    os.makedirs(os.path.dirname(temp_input_path), exist_ok=True)
    with open(temp_input_path, "w") as f:
        f.writelines([f"chronicle_0_0.story_0_0.{i}\n" for i in range(1, 101)])
    print("Input file generated")


# Deployment and System Control Functions
def deploy_system():
    """
    Deploys the system.
    """
    deploy_command = [
        os.path.join(DEPLOY_PATH, "build_install_deploy.sh"), "-type", "Release", "-n", "5", "-j", "2", "-d"
    ]
    run_command(deploy_command)


def run_client():
    """
    Runs the client admin process in a Spack environment.
    """
    generate_input_file()
    client_admin_command = f"""
        spack env activate -p {REPO_PATH} &&
        export LD_LIBRARY_PATH={os.path.join(INSTALL_PATH, "lib")}:$LD_LIBRARY_PATH &&
        {os.path.join(BIN_PATH, "client_admin")} --config {os.path.join(CONF_PATH, "client_conf.json")} -f {os.path.join(BIN_PATH, "temp_input")}
    """
    run_command(client_admin_command, shell=True)


def stop_execution():
    """
    Stops the execution of the system.
    """
    single_user_command = [os.path.join(DEPLOY_PATH, "local_single_user_deploy.sh"), "-s", "-w", INSTALL_PATH]
    run_command(single_user_command)


# Test Functions
def report_result(test_name, description, result, is_success):
    highlight = GREEN_HIGHLIGHT if is_success else RED_HIGHLIGHT
    status = "Success" if is_success else "Failure"
    print(f"{test_name}:\n***{description}***\n{highlight}{status}{RESET}\n")


def test_message_storage_validation():
    """
    Checks that each file entry contains the specified pattern.
    """
    file_list = glob.glob(os.path.join(OUTPUT_PATH, "chronicle_0_0.story_0_0.*.csv"))
    if not file_list:
        return "Test Failure: No matching files found.", False

    for file in file_list:
        with open(file, "r") as f:
            if any("chronicle_0_0.story_0_0" not in line for line in f):
                return f"Test Failure: Entry without 'chronicle_0_0.story_0_0' found in {file}.", False

    return "Test Success: All entries contain 'chronicle_0_0.story_0_0'.", True

def test_timestamp_range_check():
    """
    Ensures all timestamps fall within the expected start and end range for each file.
    """
    file_list = glob.glob(os.path.join(OUTPUT_PATH, "chronicle_0_0.story_0_0.*.csv"))
    if not file_list:
        return "Test Failure: No matching files found.", False

    for file in file_list:
        filename_parts = os.path.basename(file).split('.')
        try:
            story_chunk_start = int(filename_parts[2]) * 1_000_000_000
        except (IndexError, ValueError):
            print(f"Skipping {file} due to start time extraction issue.")
            continue

        with open(file, "r") as f:
            lines = f.readlines()
            first_timestamp = int(lines[0].strip().split(":")[2]) if lines else None
            last_timestamp = int(lines[-1].strip().split(":")[2]) if lines else None

            if not (first_timestamp and last_timestamp):
                return f"Test Failure: Could not determine start or end time in {file}.", False

            for line in lines:
                try:
                    event_timestamp = int(line.strip().split(":")[2])
                    if not (first_timestamp <= event_timestamp <= last_timestamp):
                        return f"Test Failure: Timestamp {event_timestamp} in {file} is out of range.", False
                except (IndexError, ValueError):
                    return f"Test Failure: Invalid line format in {file}.", False

    return "Test Success: All timestamps are within the specified range.", True


def test_message_sorting_by_timestamp():
    """
    Validates messages in each file are sorted by timestamp in ascending order.
    """
    file_list = glob.glob(os.path.join(OUTPUT_PATH, "chronicle_0_0.story_0_0.*.csv"))
    if not file_list:
        return "Test Failure: No matching files found.", False

    for file in file_list:
        with open(file, "r") as f:
            timestamps = [int(line.strip().split(":")[2]) for line in f if line.strip()]
            if timestamps != sorted(timestamps):
                return f"Test Failure: Timestamps out of order in {file}.", False

    return "Test Success: All messages are sorted by timestamp.", True


# Main Execution
if __name__ == "__main__":
    #deploy_system()
    #run_client()
    #stop_execution()

    print("Functional Correctness Test:\n")

    result, success = test_message_storage_validation()
    report_result("Test 1: Message Storage Validation", "Validates chronicle entry pattern", result, success)

    result, success = test_timestamp_range_check()
    report_result("Test 2: Timestamp Range Check", "Checks if all timestamps are within the expected range", result, success)

    result, success = test_message_sorting_by_timestamp()
    report_result("Test 3: Message Sorting by Timestamp", "Validates messages are sorted by timestamp", result, success)

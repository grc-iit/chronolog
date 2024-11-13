from collections import namedtuple
import subprocess
import glob
import time
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
RED_HIGHLIGHT = "\033[41m"  # Red background for Failure
RESET = "\033[0m"

# Data structure for storing file data
FileData = namedtuple("FileData", ["path", "lines", "timestamps", "story_chunk_start"])

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
    time.sleep(30)
    run_command(client_admin_command, shell=True)


def stop_execution():
    """
    Stops the execution of the system.
    """
    single_user_command = [os.path.join(DEPLOY_PATH, "local_single_user_deploy.sh"), "-s", "-w", INSTALL_PATH]
    run_command(single_user_command)


# Test Functions
def report_result(test_name, description, is_success):
    highlight = GREEN_HIGHLIGHT if is_success else RED_HIGHLIGHT
    status = "Success" if is_success else "Failure"
    print(f"\t{test_name}: {description}: {highlight}{status}{RESET}")


def collect_file_data(file_path):
    """
    Reads a file and returns collected data for testing.
    """
    filename_parts = os.path.basename(file_path).split('.')
    try:
        story_chunk_start = int(filename_parts[2]) * 1_000_000_000
    except (IndexError, ValueError):
        print(f"Skipping {file_path} due to start time extraction issue.")
        return None

    with open(file_path, "r") as f:
        lines = f.readlines()

    timestamps = []
    for line in lines:
        parts = line.strip().split(":")
        try:
            event_timestamp = int(parts[2])
            timestamps.append(event_timestamp)
        except (IndexError, ValueError):
            pass  # Invalid lines are ignored in data collection; handled in tests

    return FileData(path=file_path, lines=lines, timestamps=timestamps, story_chunk_start=story_chunk_start)


def check_storage_validation(data):
    """
    Test 1: Checks if each line in the file contains the required chronicle entry pattern.
    """
    for line in data.lines:
        if "chronicle_0_0.story_0_0" not in line:
            report_result("Test 1", "Message Storage Validation", False)
            return False
    report_result("Test 1", "Message Storage Validation", True)
    return True


def check_timestamp_range(data):
    """
    Test 2: Verifies timestamps fall within the range defined by story_chunk_start and the last timestamp.
    """
    if not data.timestamps:
        report_result("Test 2", "Timestamp Range Check", False)
        return False

    # Use story_chunk_start from filename and the last timestamp from the file
    start_timestamp = data.story_chunk_start
    end_timestamp = data.timestamps[-1]
    is_within_range = all(start_timestamp <= ts <= end_timestamp for ts in data.timestamps)
    report_result("Test 2", "Timestamp Range Check", is_within_range)
    return is_within_range


def check_timestamp_sorting(data):
    """Test 3: Checks if the timestamps are sorted in ascending order."""
    is_sorted = data.timestamps == sorted(data.timestamps)
    report_result("Test 3", "Message Sorting by Timestamp", is_sorted)
    return is_sorted


def test_files():
    """
    Main function to test each file by reading data once and applying all tests.
    """
    file_list = glob.glob(os.path.join(OUTPUT_PATH, "chronicle_0_0.story_0_0.*.csv"))
    if not file_list:
        print("No matching files found.")
        return

    for file_path in file_list:
        print(f"\nTesting file: {file_path}")

        # Collect data once per file
        data = collect_file_data(file_path)
        if data is None:
            continue  # Skip if data collection failed due to filename format issue

        # Run tests using collected data
        check_storage_validation(data)
        check_timestamp_range(data)
        check_timestamp_sorting(data)


# Main Execution
if __name__ == "__main__":
    deploy_system()
    run_client()
    stop_execution()

    print("Functional Correctness Test:")
    test_files()

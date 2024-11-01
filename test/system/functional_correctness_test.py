import subprocess
import glob
import os


# Methods
def test_message_storage_validation():
    """
    Validates if all entries in the files contain the specified pattern.
    """
    file_path = "/home/eneko/chronolog/Release/output/chronicle_0_0.story_0_0.*.csv"
    file_list = glob.glob(file_path)

    if not file_list:
        return "Test Failure: No matching files found."

    for file in file_list:
        with open(file, "r") as f:
            lines = f.readlines()
            if any("chronicle_0_0.story_0_0" not in line for line in lines):
                return f"Test Failure: Entry without 'chronicle_0_0.story_0_0' found in file {file}."

    return "Test Success: All entries contain 'chronicle_0_0.story_0_0'."


def test_timestamp_range_check():
    """
    Ensures timestamps fall within the expected start and end range for each file.
    """
    file_path = "/home/eneko/chronolog/Release/output/chronicle_0_0.story_0_0.*.csv"
    file_list = glob.glob(file_path)

    if not file_list:
        return "Test Failure: No matching files found."

    for file in file_list:
        filename_parts = os.path.basename(file).split('.')
        try:
            story_chunk_start = int(filename_parts[2]) * 1_000_000_000
        except (IndexError, ValueError):
            print(f"Skipping file {file} due to start time extraction issue.")
            continue

        with open(file, "r") as f:
            lines = f.readlines()
            first_event_timestamp = int(lines[0].strip().split(":")[2]) if lines else None
            last_event_timestamp = int(lines[-1].strip().split(":")[2]) if lines else None

            if not (first_event_timestamp and last_event_timestamp):
                return f"Test Failure: Could not determine start or end time in file {file}."

            for line in lines:
                try:
                    event_timestamp = int(line.strip().split(":")[2])
                except (IndexError, ValueError):
                    return f"Test Failure: Invalid line format in file {file}."

                if not (first_event_timestamp <= event_timestamp <= last_event_timestamp):
                    return f"Test Failure: Timestamp {event_timestamp} in file {file} is out of range."

    return "Test Success: All timestamps are within the specified range."


def test_message_sorting_by_timestamp():
    """
    Validates messages in each file are sorted by timestamp in ascending order.
    """
    file_path = "/home/eneko/chronolog/Release/output/chronicle_0_0.story_0_0.*.csv"
    file_list = glob.glob(file_path)

    if not file_list:
        return "Test Failure: No matching files found."

    for file in file_list:
        with open(file, "r") as f:
            lines = f.readlines()
            timestamps = [int(line.strip().split(":")[2]) for line in lines if line.strip()]

            if timestamps != sorted(timestamps):
                return f"Test Failure: Timestamps are out of order in file {file}."

    return "Test Success: All messages are sorted by timestamp."


# Deployment and Execution Commands
def run_command(command, shell=False):
    """
    Executes a shell command and captures output.
    """
    try:
        result = subprocess.run(command, shell=shell, check=True, executable="/bin/bash" if shell else None, text=True)
        print(f"Command output: {result.stdout}")
        print(f"Command error: {result.stderr}")
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {e.stderr}")


def generate_input_file():
    """
    Creates an input file with chronicle identifiers for testing.
    """
    temp_input_path = "/home/eneko/chronolog/Release/bin/temp_input"
    os.makedirs(os.path.dirname(temp_input_path), exist_ok=True)
    with open(temp_input_path, "w") as f:
        f.writelines([f"chronicle_0_0.story_0_0.{i}\n" for i in range(1, 101)])


if __name__ == "__main__":
    # Step 1: Deploy the system
    deploy_command = [
        "/home/eneko/Documents/Repositories/ChronoLog/deploy/build_install_deploy.sh", "-type", "Release", "-n", "5",
        "-j", "2", "-d"
    ]
    run_command(deploy_command)

    # Step 2: Run client admin with Spack environment
    spack_setup_script = "/home/eneko/Spack/spack/share/spack/setup-env.sh"
    spack_env_path = "/home/eneko/Documents/Repositories/ChronoLog/"

    # Generate input file for testing
    generate_input_file()

    client_admin_command = f"""
        . {spack_setup_script} &&
        spack env activate -p {spack_env_path} &&
        export LD_LIBRARY_PATH=/home/eneko/chronolog/Release/lib:$LD_LIBRARY_PATH &&
        /home/eneko/chronolog/Release/bin/client_admin --config /home/eneko/chronolog/Release/conf/client_conf.json -f ~/chronolog/Release/bin/temp_input
    """
    run_command(client_admin_command, shell=True)

    # Step 3: Local single-user deployment
    single_user_command = [
        "/home/eneko/Documents/Repositories/ChronoLog/deploy/local_single_user_deploy.sh", "-s",
        "-w", "/home/eneko/chronolog/Release"
    ]
    run_command(single_user_command)

    # Run Tests
    print(test_message_storage_validation())
    print(test_timestamp_range_check())
    print(test_message_sorting_by_timestamp())

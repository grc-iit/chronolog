import subprocess
import glob
import os
import time


#Methods:
def test_1_message_storage_validation():
    # Define the file path with wildcard
    file_path = "/home/eneko/chronolog/Release/output/chronicle_0_0.story_0_0.*.csv"

    # Get the list of files that match the pattern
    file_list = glob.glob(file_path)

    # Check if there are any matching files
    if not file_list:
        return "Test 1 Failure: No matching files found."

    # Check each file for the required pattern in each line
    for file in file_list:
        with open(file, "r") as f:
            lines = f.readlines()
            for line in lines:
                if "chronicle_0_0.story_0_0" not in line:
                    return f"Test 1 Failure: Entry without 'chronicle_0_0.story_0_0' found in file {file}."

    return "Test 1 Success: All entries contain 'chronicle_0_0.story_0_0'."

def test_2_timestamp_range_check():
    # Define the file path with wildcard
    file_path = "/home/eneko/chronolog/Release/output/chronicle_0_0.story_0_0.*.csv"

    # Get the list of files that match the pattern
    file_list = glob.glob(file_path)

    if not file_list:
        return "Timestamp Range Check Failure: No matching files found."

    # Check each file for timestamp range consistency
    for file in file_list:
        # Extract story_chunk_start from the filename, if available
        filename_parts = os.path.basename(file).split('.')
        try:
            story_chunk_start = int(filename_parts[2]) * 1000000000  # Convert back to original timestamp scale
        except (IndexError, ValueError):
            print(f"Could not extract start time from filename {file}; skipping file.")
            continue

        with open(file, "r") as f:
            lines = f.readlines()

            # Set story_chunk_start to the first event's timestamp if not retrieved from filename
            first_event_timestamp = int(lines[0].strip().split(":")[2]) if lines else None
            story_chunk_start = story_chunk_start or first_event_timestamp

            # Retrieve the story_chunk_end as the last event's timestamp
            last_event_timestamp = int(lines[-1].strip().split(":")[2]) if lines else None
            story_chunk_end = last_event_timestamp

            if not (story_chunk_start and story_chunk_end):
                return f"Timestamp Range Check Failure: Could not determine start or end time in file {file}."

            # Check if all events fall within the [story_chunk_start, story_chunk_end] range
            for line in lines:
                try:
                    event_timestamp = int(line.strip().split(":")[2])
                except (IndexError, ValueError):
                    return f"Timestamp Range Check Failure: Invalid line format in file {file}."

                if not (story_chunk_start <= event_timestamp <= story_chunk_end):
                    return f"Timestamp Range Check Failure: Timestamp {event_timestamp} in file {file} is out of range."

    return "Timestamp Range Check Success: All timestamps are within the specified range."

def test_3_message_sorting_by_timestamp():
    # Define the file path with wildcard
    file_path = "/home/eneko/chronolog/Release/output/chronicle_0_0.story_0_0.*.csv"

    # Get the list of files that match the pattern
    file_list = glob.glob(file_path)

    if not file_list:
        return "Message Sorting by Timestamp Failure: No matching files found."

    # Check each file to ensure messages are sorted by timestamp
    for file in file_list:
        with open(file, "r") as f:
            lines = f.readlines()
            previous_timestamp = None

            for line in lines:
                try:
                    # Extract the timestamp (assuming it's the third element in the format)
                    timestamp = int(line.strip().split(":")[2])
                except (IndexError, ValueError):
                    return f"Message Sorting by Timestamp Failure: Invalid line format in file {file}."

                # Check if current timestamp is sorted with respect to the previous one
                if previous_timestamp is not None and timestamp < previous_timestamp:
                    return f"Message Sorting by Timestamp Failure: Timestamps out of order in file {file}."

                previous_timestamp = timestamp  # Update for the next iteration

    return "Message Sorting by Timestamp Success: All messages are sorted by timestamp."


# Step 1: Run deploy the system
command = ["/home/eneko/Documents/Repositories/ChronoLog/deploy/build_install_deploy.sh", "-type", "Release", "-n", "5",
           "-j", "2", "-d"]
subprocess.run(command, check=True, text=True)

# Step 2: Run the test
spack_setup_script = "/home/eneko/Spack/spack/share/spack/setup-env.sh"
spack_env_path = "/home/eneko/Documents/Repositories/ChronoLog/"

# Generate input file
temp_input_path = os.path.expanduser("~/chronolog/Release/bin/temp_input")
os.makedirs(os.path.dirname(temp_input_path), exist_ok=True)
with open(temp_input_path, "w") as f:
    for i in range(1, 101):
        f.write(f"chronicle_0_0.story_0_0.{i}\n")

# Run the test
command = f"""
    . {spack_setup_script} &&
    spack env activate -p {spack_env_path} &&
    export LD_LIBRARY_PATH=/home/eneko/chronolog/Release/lib:$LD_LIBRARY_PATH &&
    /home/eneko/chronolog/Release/bin/client_admin --config /home/eneko/chronolog/Release/conf/client_conf.json -f ~/chronolog/Release/bin/temp_input
"""

try:
    result = subprocess.run(command, shell=True, check=True, executable="/bin/bash", text=True)
    print("Command output:", result.stdout)
    print("Command error:", result.stderr)
except subprocess.CalledProcessError as e:
    print("Error running command chain:", e.stderr)

command = ["/home/eneko/Documents/Repositories/ChronoLog/deploy/local_single_user_deploy.sh", "-s", "-w", "/home/eneko/chronolog/Release"]
subprocess.run(command, check=True, text=True)

# Run Test 1 - Message Storage Validation
result = test_1_message_storage_validation()
print(result)

result = test_2_timestamp_range_check()
print(result)

result = test_3_message_sorting_by_timestamp()
print(result)


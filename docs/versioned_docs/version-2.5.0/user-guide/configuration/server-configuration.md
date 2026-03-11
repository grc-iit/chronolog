---
sidebar_position: 2
title: "Server Configuration"
---

# Default Configuration File

ChronoLog executables require a configuration file to initialize and define various parameters and behaviors. The default configuration file template, `default_conf.json.in`, contains all necessary settings. Modify this file to suit your specific deployment and operational preferences.

The configuration file is divided into sections for different components of ChronoLog. Each section contains common settings, such as **RPC configuration** and **logging options**, as well as component-specific configurations. Below, we explain the common fields first, followed by details specific to each component.

---

### Common Fields in All Components

1. **RPC Configuration (`rpc`)**:
    - **`rpc_implementation`**: Specifies the RPC backend to use. For example, `Thallium_sockets`.
    - **`protocol_conf`**: Defines the protocol configuration, e.g., `ofi+sockets`.
    - **`service_ip`**: IP address of the service.
    - **`service_base_port`**: Base port number for communication.
    - **`service_provider_id`**: A unique identifier for the RPC service.

2. **Monitoring Configuration (`log`)**:
    - **`type`**: Type of logging, such as `file`.
    - **`file`**: Name of the log file.
    - **`level`**: Logging verbosity level, e.g., `debug`, `info`, or `warning`.
    - **`filesize`**: Maximum size of log files in bytes before rotation.
    - **`filenum`**: Number of log backups to retain.
    - **`flushlevel`**: Minimum log level that triggers a flush to disk.

---

### Component-Specific Sections

#### **Clock Configuration (`clock`)**
- **`clocksource_type`**: The type of clock source to use. For example, `CPP_STYLE` for C++-style clocks.
- **`drift_cal_sleep_sec`**: Time in seconds for clock drift calibration.
- **`drift_cal_sleep_nsec`**: Additional nanoseconds for drift calibration.

#### **Authentication (`authentication`)**
- **`auth_type`**: Defines the authentication mechanism. Example: `RBAC` for Role-Based Access Control.
- **`module_location`**: File path to the authentication module.

#### **ChronoVisor Configuration (`chrono_visor`)**
- **`VisorClientPortalService`**: Specifies the RPC configuration used for the Visor Client Portal Service. This service is responsible for facilitating communication between the ChronoVisor and its clients.
- **`VisorKeeperRegistryService`**: Specifies the RPC configuration used for the Visor Keeper Registry Service. This service is responsible for registering and managing Keepers in the ChronoVisor's ecosystem.
- **`Monitoring`**: Monitoring options specific to the ChronoClient. These options control how logging and monitoring are configured for the ChronoVisor.
- **`delayed_data_admin_exit_in_secs`**: Specifies the delay (in seconds) before the data admin service exits gracefully. This allows ongoing processes to complete before shutdown.

#### **ChronoKeeper Configuration (`chrono_keeper`)**
- **`RecordingGroup`**: Identifier for the recording group used by the Keeper. It defines a logical grouping of data for storage and retrieval.
- **`KeeperRecordingService`**: Specifies the RPC configuration used for the Keeper Recording Service, which handles the recording of data into the storage system.
- **`KeeperDataStoreAdminService`**: Specifies the RPC configuration used for the Keeper Data Store Administration Service, which manages administrative tasks related to the Keeper's data store.
- **`VisorKeeperRegistryService`**: Specifies the RPC configuration used for the Visor Keeper Registry Service, enabling communication between the ChronoKeeper and the ChronoVisor.
- **`KeeperGrapherDrainService`**: Specifies the RPC configuration used for the Keeper Grapher Drain Service, which handles data transfers between the Keeper and the ChronoGrapher.
- **`Monitoring`**: Monitoring options specific to the ChronoKeeper. These options control how logging and monitoring are configured for the Keeper.
- **`story_files_dir`**: Directory path for storing story-related files. This is where the Keeper stores serialized story data.

#### **ChronoGrapher Configuration (`chrono_grapher`)**
- **`RecordingGroup`**: Identifier for the recording group used by the Grapher. It defines a logical grouping of data for storage and retrieval.
- **`KeeperGrapherDrainService`**: Specifies the RPC configuration used for the Keeper Grapher Drain Service, which transfers data from the Keeper to the Grapher.
- **`DataStoreAdminService`**: Specifies the RPC configuration used for the Data Store Administration Service, which manages administrative tasks related to the Grapher's data store.
- **`VisorRegistryService`**: Specifies the RPC configuration used for the Visor Registry Service, which facilitates communication between the Grapher and the Visor.
- **`Monitoring`**: Monitoring options specific to the ChronoGrapher. These options control how logging and monitoring are configured for the Grapher.
- **`DataStoreInternals`**: Internal settings for the Grapher's data store. For example:
    - **`max_story_chunk_size`**: Maximum size (in bytes) of individual story chunks to optimize performance and storage efficiency.
- **`Extractors`**:
    - **`story_files_dir`**: Directory for storing extracted story files used during data retrieval.

#### **ChronoPlayer Configuration (`chrono_player`)**
- **`RecordingGroup`**: Identifier for the recording group used by the ChronoPlayer. It defines a logical grouping of data for playback.
- **`PlayerStoreAdminService`**: Specifies the RPC configuration used for the Player Store Administration Service, which manages playback-related administrative tasks.
- **`PlaybackQueryService`**: Specifies the RPC configuration used for the Playback Query Service, which processes data queries for playback.
- **`VisorRegistryService`**: Specifies the RPC configuration used for the Visor Registry Service, which enables communication between the Player and the Visor.
- **`Monitoring`**: Monitoring options specific to the ChronoPlayer. These options control how logging and monitoring are configured for the Player.

#### **ChronoClient Configuration (`chrono_client`)**
- **`VisorClientPortalService`**: Specifies the RPC configuration used for the Visor Client Portal Service. This service manages communication between the ChronoClient and the Visor.
- **`Monitoring`**: Monitoring options specific to the ChronoClient. These options control how logging and monitoring are configured for the Client.

---

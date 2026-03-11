---
sidebar_position: 2
title: "Multi-Node Deployment"
---

# Distributed Deployment

ChronoLog is designed to work in a distributed environment. We prepare a script at `deploy/single_user_deploy.sh` to help deploying it in a distributed environment.

[mpssh](https://github.com/ndenev/mpssh) and [jq](https://jqlang.github.io/jq/) are needed to run the script. Passwordless SSH needs to be configured for the script to work.

By default, the script reads hosts files, `hosts_visor`, `hosts_keeper`, `hosts_grapher`, and `hosts_client` to be specific, under `~/chronolog/conf` directory for the hosts to run the ChronoLog server daemons.

If the cluster uses **Slurm** for job scheduling, you can deploy ChronoLog in your active job using the following command:

```bash
./deploy/single_user_deploy.sh -d -n NUM_RECORDING_GROUP -j JOB_ID
```

The script will fetch the node list from Slurm, assign the first node to run the ChronoVisor, all the nodes to run ChronoKeepers, and the last `NUM_RECORDING_GROUP` nodes to run ChronoGraphers. The `client_lib_multi_storytellers` use case will be launched at last to test if the deployment is successful.

```
Usage:
-d|--deploy Start ChronoLog deployment (default: false)
-s|--stop Stop ChronoLog deployment (default: false)
-k|--kill Terminate ChronoLog deployment (default: false)
-r|--reset Reset/cleanup ChronoLog deployment (default: false)
-w|--work_dir WORK_DIR (default: ~/chronolog)
-u|--output_dir OUTPUT_DIR (default: work_dir/output)
-v|--visor VISOR_BIN (default: work_dir/bin/chronovisor_server)
-g|--grapher GRAPHER_BIN (default: work_dir/bin/chrono_grapher)
-p|--keeper KEEPER_BIN (default: work_dir/bin/chrono_keeper)
-c|--client CLIENT_BIN (default: work_dir/bin/client_lib_multi_storytellers)
-i|--visor_hosts VISOR_HOSTS (default: work_dir/conf/hosts_visor)
-a|--grapher_hosts GRAPHER_HOSTS (default: work_dir/conf/hosts_grapher)
-o|--keeper_hosts KEEPER_HOSTS (default: work_dir/conf/hosts_keeper)
-t|--client_hosts CLIENT_HOSTS (default: work_dir/conf/hosts_client)
-f|--conf_file CONF_FILE (default: work_dir/conf/default_conf.json)
-j|--job_id JOB_ID (default: "", overwrites hosts files if set)
-n|--num_recording_group NUM_RECORDING_GROUP (default: #hosts in GRAPHER_HOSTS if exists, 1 otherwise, overwrites hosts files if set)
-e|--verbose Enable verbose output (default: false)
-h|--help Print this page
```

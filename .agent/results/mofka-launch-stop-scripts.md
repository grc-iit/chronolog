# Mofka Launch/Stop Scripts

Status: complete.

Added:

- `.agent/scripts/mofka_common.sh`
- `.agent/scripts/mofka_launch.sh`
- `.agent/scripts/mofka_stop.sh`

The launch wrapper follows Mofka's Bedrock deployment model with a master provider and one or more storage providers. It defaults to bare-metal mode with `ofi+tcp`, and local `na+sm` launches are explicitly treated as smoke validation only.

The stop wrapper terminates only pids recorded under the Phase 0 result directory.

Validation evidence:

- `.agent/results/20260511-235012/mofka/stdout.log`
- `.agent/results/20260511-235012/mofka/stderr.log`

Documentation consulted:

- https://mofka.readthedocs.io/en/latest/usage/installation.html
- https://mofka.readthedocs.io/en/latest/usage/quickstart.html
- https://mofka.readthedocs.io/en/latest/usage/deployment.html

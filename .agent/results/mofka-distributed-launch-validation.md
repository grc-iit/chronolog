# Mofka Distributed Launch Validation

Status: complete for bare-metal two-node launch/query.

Validation run:

```text
.agent/results/20260512-004325/
```

Launch command:

```text
.agent/scripts/mofka_launch.sh \
  --deployment-mode bare_metal \
  --node-count 2 \
  --protocol ofi+tcp \
  --slurm-partition debug \
  --slurm-time 00:05:00
```

Evidence:

- Selected nodes: `.agent/results/20260512-004325/config/mofka-slurm-nodes.txt`
- Master log: `.agent/results/20260512-004325/mofka/logs/mofka-master.stdout.log`
- Storage log: `.agent/results/20260512-004325/mofka/logs/mofka-storage-1.stdout.log`
- Bedrock query: `.agent/results/20260512-004325/mofka/bedrock-query-distributed.json`
- Config manifest: `.agent/results/20260512-004325/config/mofka-config-manifest.env`

Observed layout:

- master node: `ares-comp-03`
- storage node: `ares-comp-04`
- master address: `ofi+tcp://172.25.101.3:46601`
- storage address: `ofi+tcp://172.25.101.4:40647`
- master provider: Yokan tagged `mofka:master`
- storage providers: Yokan tagged `mofka:metadata`, Warabi tagged `mofka:data`

Notes:

- The Mofka launcher now selects/pins distinct SLURM nodes for bare-metal multi-node runs instead of launching independent one-node `srun` commands that may land on the same node.
- `srun` stderr contains expected cancellation messages from stopping long-running Bedrock daemons after the validation query.
- This validates distributed launch topology; the next step is a distributed Mofka append workflow using this topology.

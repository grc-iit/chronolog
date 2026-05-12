# ChronoLog Deployment Topology Used by Phase 0 / ProfileForge

This note describes how the current ChronoLog distributed harness places ChronoLog services and the benchmark application process on Ares.

The active harness is:

```text
.agent/scripts/chronolog_run_append_distributed.sh
```

The underlying ChronoLog launcher is:

```text
<chronolog-install>/tools/deploy/deploy_cluster.sh
```

## Current Deployment Model

The current deployment is bare-metal SLURM, not containerized.

The harness first obtains a SLURM allocation:

```bash
salloc --partition=<partition> --nodes=<node_count> --time=<time> [--nodelist=<nodes>]
```

Inside that allocation, it writes global host files:

```text
hosts_visor   = first allocated node
hosts_keeper  = all allocated nodes before record-group partitioning
hosts_grapher = last <record_groups> allocated nodes
hosts_player  = last <record_groups> allocated nodes
client_node   = first allocated node
```

`deploy_cluster.sh` then derives per-recording-group host files such as `hosts_keeper.1`, `hosts_keeper.2`, `hosts_grapher.1`, and `hosts_grapher.2`.

For the default `record_groups=1`, this means:

```text
ChronoVisor:   first node
ChronoKeepers: every allocated node
ChronoGrapher: last node
ChronoPlayer:  last node
Client/app:    first node
```

The client/application process is the benchmark process, currently `chrono-bench` for append throughput. It is launched from the harness on the first allocated node. For `profile-mode=perf`, the client is explicitly launched with `srun --nodelist=<client_node>` so it runs on the compute node rather than the login/master node.

Transport is currently:

```text
ofi+sockets
```

The harness resolves the client callback IP using the first node's `-40g` hostname when available.

## Recording Groups

A ChronoLog recording group is the service group used to record stories. In the current launcher, each group has:

```text
one or more ChronoKeepers
one ChronoGrapher
one ChronoPlayer
```

`record_groups` is tunable in the benchmark harness through:

```bash
--record-groups <N>
```

This setting changes the generated host files and therefore changes placement. It is not just a label in the metrics.

## Two-Node Layout

For:

```bash
--node-count 2 --record-groups 1 --nodelist 'ares-comp-[03-04]'
```

the layout is:

| Node | Services / processes |
|---|---|
| `ares-comp-03` | ChronoVisor, ChronoKeeper, client/application process |
| `ares-comp-04` | ChronoKeeper, ChronoGrapher, ChronoPlayer |

This is the layout used by the latest broad TAU semantic run:

```text
.agent/results/20260512-122315-chronolog-tau-full-semantics
```

## Four-Node Layout With One Recording Group

For:

```bash
--node-count 4 --record-groups 1 --nodelist 'ares-comp-[03-06]'
```

the layout is:

| Node | Services / processes |
|---|---|
| `ares-comp-03` | ChronoVisor, ChronoKeeper, client/application process |
| `ares-comp-04` | ChronoKeeper |
| `ares-comp-05` | ChronoKeeper |
| `ares-comp-06` | ChronoKeeper, ChronoGrapher, ChronoPlayer |

Evidence:

```text
.agent/results/20260512-020504/config/chronolog-slurm-nodes.txt
.agent/results/20260512-020504/config/chronolog-config-manifest.env
.agent/results/phase0-scaling-sweep.md
```

## Important Interpretation

When we say "4-node ChronoLog deployment" in the current Phase 0 harness, it does **not** mean one service per node. For the one-recording-group case above, it means:

It means:

```text
4 allocated SLURM nodes
4 ChronoKeepers
1 ChronoVisor
1 ChronoGrapher
1 ChronoPlayer
1 client/application process
```

with service co-location as described above. For `record_groups > 1`, the per-group host files change this service layout.

This is acceptable as a validated distributed harness, but it is not necessarily the final production or performance-optimal placement.

## Four-Node Layout With Two Recording Groups

For:

```bash
--node-count 4 --record-groups 2 --nodelist 'ares-comp-[03-06]'
```

the validated layout is:

| Node | Services / processes |
|---|---|
| `ares-comp-03` | ChronoVisor, RG1 ChronoKeeper, client/application process |
| `ares-comp-04` | RG1 ChronoKeeper |
| `ares-comp-05` | RG1 ChronoGrapher, RG1 ChronoPlayer, RG2 ChronoKeeper |
| `ares-comp-06` | RG2 ChronoGrapher, RG2 ChronoPlayer, RG2 ChronoKeeper |

Evidence:

```text
.agent/results/20260512-133500-chronolog-rg2-validation/config/chronolog-config-manifest.env
.agent/results/20260512-133500-chronolog-rg2-validation/config/hosts_keeper.1
.agent/results/20260512-133500-chronolog-rg2-validation/config/hosts_keeper.2
.agent/results/20260512-133500-chronolog-rg2-validation/config/hosts_grapher.1
.agent/results/20260512-133500-chronolog-rg2-validation/config/hosts_grapher.2
.agent/results/20260512-133500-chronolog-rg2-validation/chronolog/deploy-work/conf/hosts_player.1
.agent/results/20260512-133500-chronolog-rg2-validation/chronolog/deploy-work/conf/hosts_player.2
.agent/results/20260512-133500-chronolog-rg2-validation/chronolog/metrics.json
```

The global `hosts_keeper` file contains all four nodes, but the per-group files split them:

```text
RG1 Keepers: ares-comp-03, ares-comp-04
RG2 Keepers: ares-comp-05, ares-comp-06
RG1 Grapher/Player: ares-comp-05
RG2 Grapher/Player: ares-comp-06
```

This distinction matters for performance interpretation because a four-node deployment with `record_groups=2` is not the same service topology as a four-node deployment with `record_groups=1`.

## Engineer Review Questions

The ChronoLog engineers should confirm whether this topology is the intended comparison topology:

1. Should every allocated node run a ChronoKeeper, or should Keepers be isolated from Visor/Grapher/Player?
2. Should the client/application process share the Visor node, or should it run on a separate client node outside the service allocation?
3. For `record_groups=1`, should Grapher and Player share the last node, or should they be separated for profiling and performance runs?
4. For larger node counts, should `record_groups` scale with node count?
5. Should the next production loop use `ofi+sockets`, or should it switch to the intended RDMA/RoCE provider once configured?
6. Should process placement, CPU affinity, and NUMA binding become explicit benchmark parameters in the ProfileForge target manifest?
7. For `record_groups > 1`, should Grapher/Player nodes overlap with Keepers, or should ProfileForge reserve separate service/client roles for production comparison?

## ProfileForge Target-Manifest Implication

The ChronoLog target manifest should represent placement explicitly instead of hiding it in shell host-file rules.

A future manifest entry should look like:

```yaml
deployment:
  mode: bare_metal_slurm
  transport: ofi+sockets
  node_count: 4
  record_groups: 2
  placement:
    visor: first_node
    keepers: partitioned_across_record_groups
    grapher: one_per_recording_group
    player: one_per_recording_group
    client: first_node
```

If the engineers prefer a different placement, this document should be updated first, then the harness and manifest should follow.

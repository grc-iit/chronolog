# ChronoLog plugins

Each plugin is its own owned directory and is built only on the public client SDK or its bindings, never on `src/`
(ARCHITECTURE.md section 11). The plugins build with the default presets unless `-DCHRONOLOG_BUILD_PLUGINS=OFF` is
set. Tests that start a stack run the real services on loopback ports.

| Plugin | What it does | Tests |
| --- | --- | --- |
| `kvs` | A versioned key-value store on a story: put, get, get as of an HLC, and history. CLI `chronolog_kvs`. | `ctest --preset dev -R '^kvs\.'` |
| `pubsub` | Topics with at-least-once delivery and resumable consumers whose positions live in kvs. | `ctest --preset dev -R '^pubsub\.'` |
| `sql` | Append-only typed tables with a small SELECT that reports its Completion. CLI `chronolog_sql`. | `ctest --preset dev -R '^sql\.'` |
| `mcp` | `chronolog-mcp`, the MCP server that gives agents the Context API as twelve tools, plus local instance control ([mcp/README.md](mcp/README.md)). | `ctest --test-dir build/python -R mcp` after building the `python` preset |
| `stream` | Host metrics into ChronoLog and a resumable exporter to InfluxDB, with Grafana dashboards. | `ctest --preset dev -R '^stream\.'` |
| `viz` | A Grafana datasource for ChronoLog with completeness notices and live tail. | `ctest --preset dev -R viz` (needs Node.js and npm) |
| `ldms` | An LDMS store plugin that bridges sampler data into ChronoLog through a non-blocking queue. The ldmsd plugin itself builds only against an LDMS install; the tests use a fake ldmsd. | `ctest --preset dev -R '^ldms\.'` |

The container smoke (`tests/smoke/run.sh`) also runs the plugin suites against the compose stack.

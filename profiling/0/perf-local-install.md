# Repo-Local perf Install

The default Ares environment did not expose `perf` on `PATH`. For Phase 0, a no-sudo kernel-matched `perf` binary was installed under ignored workspace state:

```text
opt/perf/bin/perf
```

Validated on `ares-comp-03`:

```bash
srun -p debug -N1 -n1 -w ares-comp-03 --time=00:02:00 \
  /mnt/common/jcernudagarcia/chronolog-opt/chronolog/opt/perf/bin/perf stat true
```

Recreate the local install on Ares:

```bash
mkdir -p opt/perf/debs opt/perf/root opt/perf/bin
cd opt/perf/debs
apt-get download linux-tools-5.15.0-176=5.15.0-176.186
apt-get download linux-tools-5.15.0-176-generic=5.15.0-176.186
apt-get download linux-tools-common=5.15.0-176.186
cd ../../..
for deb in opt/perf/debs/*.deb; do
  dpkg-deb -x "$deb" opt/perf/root
done
ln -sf ../root/usr/lib/linux-tools/5.15.0-176-generic/perf opt/perf/bin/perf
opt/perf/bin/perf --version
```

Use it in the ChronoLog distributed harness:

```bash
.agent/scripts/chronolog_run_append_distributed.sh \
  --partition debug \
  --nodelist 'ares-comp-[03-04]' \
  --node-count 2 \
  --install-dir .agent/install-tau/chronolog \
  --profile-mode perf \
  --perf-bin opt/perf/bin/perf
```

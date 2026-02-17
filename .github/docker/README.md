# ChronoLog CI Docker images

This directory contains the Dockerfile used by the CI workflow. The workflow publishes images to GitHub Container Registry (ghcr.io).

## Image tags

- **Base image** (`chronolog:base-<hash>`): Ubuntu + Spack + Spack dependencies. No ChronoLog source; used as a build cache.
- **Final image** (`chronolog:chronolog-<branch>` e.g. `chronolog:chronolog-main`): Base image + ChronoLog built and installed, **plus the ChronoLog repo** at `/home/grc-iit/chronolog-repo`.

## Using the final image locally

The final image is configured to run as user **`grc-iit`** (UID 1001) by default, so you can use it without permission issues:

```bash
# Pull the image (use your branch tag, e.g. chronolog-main or latest)
docker pull ghcr.io/grc-iit/chronolog:chronolog-main

# Run (default user is grc-iit)
docker run -it ghcr.io/grc-iit/chronolog:chronolog-main bash
```

If you need to run as root (e.g. to install packages), use `--user root`. To match a specific host user, use `--user 1001:1001` or your own UID:GID.

## Paths in the final image

| Path | Contents |
|------|----------|
| `/home/grc-iit/chronolog-repo` | ChronoLog source (same commit as the image build) |
| `/home/grc-iit/chronolog-install/chronolog` | Installed ChronoLog binaries and config |
| `/home/grc-iit/spack` | Spack installation |

Spack is already set up; inside the container run `source /home/grc-iit/spack/share/spack/setup-env.sh` and `spack env activate -p .` in the repo directory to use the environment.

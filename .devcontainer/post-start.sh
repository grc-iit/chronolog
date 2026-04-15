#!/usr/bin/env bash
# Devcontainer post-start setup — runs every time the container starts.
# Opens the Docker socket so the dev user can use Docker regardless of host GID.

if [ -S /var/run/docker.sock ]; then
    sudo chmod 666 /var/run/docker.sock
fi

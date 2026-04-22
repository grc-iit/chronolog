#!/usr/bin/env bash
# FIXTURES_CLEANUP for end-to-end data-integrity tests.
#
# The fixture no longer owns the deployment (see the note in
# data_integrity_fixture_setup.sh) — the CI pipeline's Stop ChronoLog step
# and the developer's own workflow are responsible for teardown. This
# cleanup is a deliberate no-op so ctest's FIXTURES_CLEANUP plumbing still
# has a hook, without interfering with the shared stack.

exit 0

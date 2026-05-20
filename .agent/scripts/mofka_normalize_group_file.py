#!/usr/bin/env python3
"""Write a Mofka client group file with stable master-first member order."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def provider_has_tag(endpoint: dict, tag: str) -> bool:
    for provider in endpoint.get("providers", []):
        if tag in provider.get("tags", []):
            return True
    return False


def endpoint_supports_storage(endpoint: dict) -> bool:
    libraries = set(endpoint.get("libraries", []))
    if "libmofka-bedrock-module.so" in libraries:
        return True
    return provider_has_tag(endpoint, "mofka:data") or provider_has_tag(endpoint, "mofka:metadata")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group-file", required=True)
    parser.add_argument("--bedrock-query", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--expected-members", type=int, required=True)
    args = parser.parse_args()

    group_path = Path(args.group_file)
    group = json.loads(group_path.read_text())
    query = json.loads(Path(args.bedrock_query).read_text())

    member_provider_ids = {}
    for member in group.get("members", []):
        address = member.get("address")
        if address and address not in member_provider_ids:
            member_provider_ids[address] = member.get("provider_id", 1)

    primary_address = ""
    try:
        primary_address = json.loads(group.get("metadata", {}).get("__config__", "{}")).get("primary_address", "")
    except Exception:
        primary_address = ""

    master_addresses = [
        address for address, endpoint in query.items() if provider_has_tag(endpoint, "mofka:master")
    ]
    if primary_address:
        master_addresses.insert(0, primary_address)

    ordered_addresses: list[str] = []
    for address in master_addresses:
        if address in query and address not in ordered_addresses:
            ordered_addresses.append(address)

    for address, endpoint in query.items():
        if endpoint_supports_storage(endpoint) and address not in ordered_addresses:
            ordered_addresses.append(address)

    for address in member_provider_ids:
        if address not in ordered_addresses:
            ordered_addresses.append(address)

    if len(ordered_addresses) != args.expected_members:
        raise SystemExit(
            f"normalized Mofka group has {len(ordered_addresses)} unique members; "
            f"expected {args.expected_members}: {ordered_addresses}"
        )

    normalized = {
        "members": [
            {"address": address, "provider_id": member_provider_ids.get(address, 1)}
            for address in ordered_addresses
        ],
        "metadata": group.get("metadata", {}),
    }
    Path(args.output).write_text(json.dumps(normalized, separators=(",", ":")) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

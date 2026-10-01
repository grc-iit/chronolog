#!/usr/bin/env python3
"""Smoke test for the ChronoLog stub servers.

Generates Python stubs from proto/ into a temporary directory, then calls one RPC
on every chronolog.v1 service at every endpoint. Exits 0 only if every call
returns UNIMPLEMENTED, which is the contract of chronolog_stub_server until real
services exist. A service whose proto declares no methods is probed with a
synthetic method name, which an UNIMPLEMENTED server rejects the same way.
"""

import argparse
import importlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PROTO_ROOT = REPO_ROOT / "proto"
# Matches deploy/compose/compose.yaml.
DEFAULT_ENDPOINTS = {
    "chrono-visor": "127.0.0.1:50051",
    "chrono-keeper": "127.0.0.1:50052",
    "chrono-grapher": "127.0.0.1:50053",
    "chrono-player": "127.0.0.1:50054",
}


def generate_stubs(out_dir: Path) -> None:
    protos = sorted(str(p.relative_to(PROTO_ROOT)) for p in PROTO_ROOT.glob("chronolog/v1/*.proto"))
    if not protos:
        raise SystemExit("no .proto files under proto/chronolog/v1")
    cmd = [
        sys.executable, "-m", "grpc_tools.protoc",
        f"-I{PROTO_ROOT}", f"--python_out={out_dir}", f"--grpc_python_out={out_dir}", *protos,
    ]
    subprocess.run(cmd, check=True)
    for pkg in (out_dir / "chronolog", out_dir / "chronolog" / "v1"):
        (pkg / "__init__.py").touch()


def load_services(out_dir: Path):
    """Returns (service_descriptor, message_factory_module) for every service."""
    sys.path.insert(0, str(out_dir))
    from google.protobuf import message_factory

    services = []
    for proto in sorted((out_dir / "chronolog" / "v1").glob("*_pb2.py")):
        module = importlib.import_module(f"chronolog.v1.{proto.stem}")
        for service in module.DESCRIPTOR.services_by_name.values():
            services.append(service)
    return services, message_factory


def probe(channel, service, message_factory, timeout):
    """Calls one RPC of the service and returns the resulting grpc.StatusCode."""
    import grpc

    methods = list(service.methods)
    streaming = False
    if not methods:
        call = channel.unary_unary(f"/{service.full_name}/Unimplemented")
        label = f"{service.full_name}/Unimplemented"
        request = b""
    else:
        method = methods[0]
        path = f"/{service.full_name}/{method.name}"
        label = path[1:]
        in_cls = message_factory.GetMessageClass(method.input_type)
        out_cls = message_factory.GetMessageClass(method.output_type)
        kwargs = dict(request_serializer=in_cls.SerializeToString,
                      response_deserializer=out_cls.FromString)
        factory = {
            (False, False): channel.unary_unary,
            (False, True): channel.unary_stream,
            (True, False): channel.stream_unary,
            (True, True): channel.stream_stream,
        }[(method.client_streaming, method.server_streaming)]
        call = factory(path, **kwargs)
        streaming = method.server_streaming
        request = in_cls()
        if method.client_streaming:
            request = iter([request])
    try:
        result = call(request, timeout=timeout)
        if streaming:
            list(result)
        return label, grpc.StatusCode.OK
    except grpc.RpcError as err:
        return label, err.code()


def parse_endpoints(values):
    if not values:
        env = os.environ.get("CHRONOLOG_SMOKE_ENDPOINTS")
        values = env.split(",") if env else []
    if not values:
        return dict(DEFAULT_ENDPOINTS)
    endpoints = {}
    for item in values:
        name, sep, address = item.partition("=")
        if not sep:
            raise SystemExit(f"endpoint must be NAME=HOST:PORT, got {item!r}")
        endpoints[name] = address
    return endpoints


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--endpoint", action="append", metavar="NAME=HOST:PORT",
                        help="repeatable; defaults to the compose services, or CHRONOLOG_SMOKE_ENDPOINTS")
    parser.add_argument("--ready-timeout", type=float, default=30.0, help="seconds to wait per endpoint")
    parser.add_argument("--rpc-timeout", type=float, default=10.0)
    args = parser.parse_args()
    endpoints = parse_endpoints(args.endpoint)

    import grpc

    out_dir = Path(tempfile.mkdtemp(prefix="chronolog-smoke-"))
    failures = 0
    try:
        generate_stubs(out_dir)
        services, message_factory = load_services(out_dir)
        if not services:
            print("FAIL no services found in generated stubs")
            return 1
        for name, address in endpoints.items():
            with grpc.insecure_channel(address) as channel:
                try:
                    grpc.channel_ready_future(channel).result(timeout=args.ready_timeout)
                except grpc.FutureTimeoutError:
                    print(f"FAIL {name} {address} not reachable within {args.ready_timeout}s")
                    failures += 1
                    continue
                for service in services:
                    label, code = probe(channel, service, message_factory, args.rpc_timeout)
                    ok = code == grpc.StatusCode.UNIMPLEMENTED
                    failures += 0 if ok else 1
                    print(f"{'PASS' if ok else 'FAIL'} {name} {address} {label} -> {code.name}")
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)
    print("smoke: all calls UNIMPLEMENTED" if failures == 0 else f"smoke: {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

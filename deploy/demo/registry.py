import contextlib
import getpass
import hashlib
import io
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'local'))
from chronolog_local.cli import external_status, main as chronolog
from chronolog_local.registry import atomic, control, home, load


def main():
    action, engine, project = sys.argv[1:]
    os.umask(0o077)
    name = 'demo-' + engine + '-' + hashlib.sha256(project.encode()).hexdigest()[:16]
    root = home()
    path = root / 'external' / (name + '.json')
    with control(root / 'locks/registry.lock'):
        record = load(path) if path.exists() else None
        if (root / 'instances' / name).exists() or (record is not None and
                (record.get('kind') != 'external' or record.get('engine') != engine or
                 record.get('project') != project or record.get('owner', {}).get('uid') != os.getuid())):
            raise ValueError('registry name is owned by another instance: ' + name)
        if action == 'up':
            if record is None:
                sys.argv = ['chronolog', 'register', name, '--catalog', '127.0.0.1:50051',
                            '--player', '127.0.0.1:50054']
                with contextlib.redirect_stdout(io.StringIO()):
                    chronolog()
                record = load(path)
            record.update(engine=engine, project=project,
                          owner={'uid': os.getuid(), 'user': getpass.getuser(), 'created_by': 'chronolog-demo'})
            record = external_status(record)
            atomic(path, record)
        elif action == 'down' and record is not None:
            path.unlink()
            fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        elif action not in ('check', 'down'):
            raise ValueError('unknown registry action ' + action)
    print(json.dumps({'name': name}))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        print('chronolog-demo: ' + str(error), file=sys.stderr)
        sys.exit(1)

import base64
import csv
import hashlib
import io
import os
from pathlib import Path
import re
import sys
import tempfile
import tomllib
import zipfile


def main():
    source = Path(__file__).resolve().parent
    config = tomllib.loads((source / 'pyproject.toml').read_text())
    project = config['project']
    name = re.sub(r'[-_.]+', '_', project['name'])
    version = project['version']
    info = name + '-' + version + '.dist-info'
    files = {}
    for package in config['tool']['setuptools']['packages']:
        for path in sorted((source / package).rglob('*.py')):
            files[path.relative_to(source).as_posix()] = path.read_bytes()
    metadata = ['Metadata-Version: 2.4', 'Name: ' + project['name'], 'Version: ' + version,
                'Requires-Python: ' + project['requires-python']]
    metadata += ['Requires-Dist: ' + dependency for dependency in project.get('dependencies', [])]
    files[info + '/METADATA'] = ('\n'.join(metadata) + '\n\n').encode()
    files[info + '/WHEEL'] = b'Wheel-Version: 1.0\nGenerator: chronolog-local-stdlib\nRoot-Is-Purelib: true\nTag: py3-none-any\n'
    entries = ['[console_scripts]']
    entries += [name + ' = ' + target for name, target in sorted(project['scripts'].items())]
    files[info + '/entry_points.txt'] = ('\n'.join(entries) + '\n').encode()
    record = io.StringIO(newline='')
    writer = csv.writer(record, lineterminator='\n')
    for path, data in sorted(files.items()):
        digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b'=').decode()
        writer.writerow([path, 'sha256=' + digest, len(data)])
    writer.writerow([info + '/RECORD', '', ''])
    files[info + '/RECORD'] = record.getvalue().encode()
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    wheel = output / (name + '-' + version + '-py3-none-any.whl')
    fd, temporary = tempfile.mkstemp(dir=output, suffix='.whl')
    os.close(fd)
    try:
        with zipfile.ZipFile(temporary, 'w') as archive:
            for path, data in sorted(files.items()):
                item = zipfile.ZipInfo(path, date_time=(1980, 1, 1, 0, 0, 0))
                item.compress_type = zipfile.ZIP_DEFLATED
                item.external_attr = 0o100644 << 16
                archive.writestr(item, data, compresslevel=9)
        os.replace(temporary, wheel)
    finally:
        Path(temporary).unlink(missing_ok=True)
    print('Built ' + str(wheel))


if __name__ == '__main__':
    main()

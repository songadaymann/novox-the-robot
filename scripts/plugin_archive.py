#!/usr/bin/env python3
"""Archive source and plugin bundles before installation. Python 3, no dependencies.

prepare freezes source before a build; install refuses source changes since prepare.
All archives are immutable, uniquely dated, and locally retained. Old binaries without
an installation receipt are explicitly marked as having unverified source provenance.
"""
import argparse
import contextlib
import datetime as dt
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import uuid

EXCLUDED = {'.git', 'node_modules', '.venv', 'venv', '__pycache__', 'private-assets',
            'reference', 'out', 'output', 'outputs', 'logs', 'models', '.cache',
            'model-cache', 'data', 'dist', '.ssh', '.aws', '.gnupg', 'secrets'}
REQUIRED_DATA = {'data/cmudict.dict', 'data/cmudict.LICENSE'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()


def save(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp-' + uuid.uuid4().hex)
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    os.replace(temporary, path)


def read(path):
    return json.loads(Path(path).read_text())


@contextlib.contextmanager
def locked(root):
    root.mkdir(parents=True, exist_ok=True)
    with open(root / '.archive.lock', 'a+b') as lock:
        if os.name == 'nt':
            import msvcrt
            lock.write(b'0'); lock.flush(); lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == 'nt':
                lock.seek(0); msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def new_archive(root, kind, label):
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H-%M-%S.%fZ')
    safe_label = ''.join(c if c.isalnum() or c in '-_' else '-' for c in label)[:60]
    path = root / (stamp + '-' + kind + ('-' + safe_label if safe_label else '') + '-' + uuid.uuid4().hex[:8])
    path.mkdir()  # Never reuse or overwrite a previous archive.
    return path


def entry(path):
    st = path.lstat()
    result = {'mode': stat.S_IMODE(st.st_mode)}
    if stat.S_ISLNK(st.st_mode):
        result.update(type='symlink', target=os.readlink(path))
    elif stat.S_ISREG(st.st_mode):
        result.update(type='file', size=st.st_size, sha256=file_hash(path))
    elif stat.S_ISDIR(st.st_mode):
        result.update(type='directory')
    else:
        raise RuntimeError('Unsupported special file: ' + str(path))
    return result


def bundle_inventory(path):
    if not path.is_dir() or path.is_symlink():
        raise RuntimeError('Bundle must be a real directory: ' + str(path))
    result = {'.': entry(path)}
    for directory, dirs, files in os.walk(path, followlinks=False):
        for name in sorted(dirs + files):
            item = Path(directory) / name
            result[item.relative_to(path).as_posix()] = entry(item)
    return result


def copy_bundle(source, destination):
    if destination.exists() or destination.is_symlink():
        raise RuntimeError('Refusing to overwrite bundle copy: ' + str(destination))
    destination.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == 'darwin':
        subprocess.run(['/usr/bin/ditto', str(source), str(destination)], check=True)
    else:
        shutil.copytree(source, destination, symlinks=True, copy_function=shutil.copy2)
    if bundle_inventory(source) != bundle_inventory(destination):
        raise RuntimeError('Bundle copy verification failed: ' + str(destination))


def allowed(name):
    if name in REQUIRED_DATA:
        return True
    parts = Path(name).parts
    if any(p in EXCLUDED or p.startswith('build') for p in parts[:-1]):
        return False
    lower = parts[-1].lower()
    if lower == '.ds_store' or lower.startswith('.dev.vars'):
        return False
    if lower.startswith('.env') and lower != '.env.example':
        return False
    if lower in {'credentials.json', 'secrets.json', 'service-account.json', 'id_rsa', 'id_ed25519', 'id_ecdsa', '.npmrc', '.pypirc'}:
        return False
    return not lower.endswith(('.pem', '.key', '.p12', '.pfx', '.p8', '.keystore', '.pyc'))


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])



# Deliberately explicit: CMake caches can contain credentials and unrelated settings.
CACHE_KEYS = {
    'CMAKE_BUILD_TYPE', 'CMAKE_CONFIGURATION_TYPES', 'CMAKE_GENERATOR',
    'CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET', 'CMAKE_MAKE_PROGRAM',
    'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'CMAKE_OBJC_COMPILER', 'CMAKE_OBJCXX_COMPILER',
    'CMAKE_OSX_ARCHITECTURES', 'CMAKE_OSX_DEPLOYMENT_TARGET', 'CMAKE_OSX_SYSROOT',
    'CMAKE_SYSTEM_NAME', 'CMAKE_POSITION_INDEPENDENT_CODE', 'BUILD_SHARED_LIBS',
    'NOVOX_EMBED_DICT', 'NOVOX_BUILD_CLI', 'NOVOX_BUILD_TESTS', 'NOVOX_BUILD_PLUGIN',
    'NOVOX_AAX_SDK', 'NOVOX_PLUGIN_ARCHIVE_DIR',
    'WHISPER_BUILD_TESTS', 'WHISPER_BUILD_EXAMPLES', 'WHISPER_CURL', 'WHISPER_FFMPEG',
    'WHISPER_COREML', 'WHISPER_COREML_ALLOW_FALLBACK', 'WHISPER_OPENVINO',
    'FETCHCONTENT_SOURCE_DIR_JUCE', 'FETCHCONTENT_SOURCE_DIR_WHISPER',
}
FLAG_KEY = re.compile(r'^CMAKE_(?:(?:C|CXX|OBJC|OBJCXX|EXE_LINKER|MODULE_LINKER|SHARED_LINKER)_FLAGS(?:_[A-Z_]+)?|INTERPROCEDURAL_OPTIMIZATION(?:_[A-Z_]+)?)$')
SOURCE_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.m',
                   '.mm', '.cmake', '.in', '.metal', '.swift', '.s', '.asm', '.py', '.js'}


def build_environment(build_dir, archive=None):
    cache_path = build_dir / 'CMakeCache.txt'
    cache = {}
    if cache_path.exists():
        for line in cache_path.read_text().splitlines():
            match = re.match(r'^([^#/:][^:=]*):([^=]+)=(.*)$', line)
            if match:
                key, kind, value = match.groups()
                if key in CACHE_KEYS or FLAG_KEY.match(key) or (key.startswith('GGML_') and kind == 'BOOL'):
                    cache[key] = value
    result = {'schema': 1, 'cache_present': cache_path.exists(), 'cache': cache, 'dependencies': {}}
    if 'NOVOX_AAX_SDK' in cache and not cache['NOVOX_AAX_SDK']:
        result['aax_sdk_environment'] = os.environ.get('NOVOX_AAX_SDK', '')
    for name in ('juce', 'whisper'):
        override = cache.get('FETCHCONTENT_SOURCE_DIR_' + name.upper())
        dependency = Path(override).resolve() if override else build_dir / '_deps' / (name + '-src')
        if not dependency.exists():
            if override:
                raise RuntimeError('Configured dependency is missing: ' + str(dependency))
            continue
        dependency = dependency.resolve()
        actual_root = Path(git(dependency, 'rev-parse', '--show-toplevel').decode().strip()).resolve()
        if actual_root != dependency:
            raise RuntimeError('Dependency must have its own Git checkout: ' + str(dependency))
        untracked = sorted(os.fsdecode(x) for x in git(dependency, 'ls-files', '-z', '--others', '--exclude-standard').split(b'\0') if x)
        unsafe = [n for n in untracked if Path(n).suffix.lower() in SOURCE_SUFFIXES
                  or Path(n).name in {'CMakeLists.txt', 'Makefile', 'meson.build'}]
        if unsafe:
            raise RuntimeError('Untracked dependency source cannot be reproduced: ' + name + ': ' + ', '.join(unsafe))
        patch = git(dependency, 'diff', '--binary', '--full-index', '--no-ext-diff', '--no-textconv', '--no-color', 'HEAD', '--')
        result['dependencies'][name] = {
            'path': str(dependency), 'git_head': git(dependency, 'rev-parse', 'HEAD').decode().strip(),
            'tracked_patch_sha256': digest(patch), 'untracked_files': untracked,
        }
        if archive is not None:
            folder = archive / 'build-dependencies'
            folder.mkdir(exist_ok=True)
            (folder / (name + '.patch')).write_bytes(patch)
    if archive is not None:
        save(archive / 'build-environment.json', result)
    return result


def verify_build_environment(archive, source):
    expected = source.get('build_environment_sha256')
    if expected is None:
        return
    environment = read(archive / 'build-environment.json')
    if digest(canonical(environment)) != expected:
        raise RuntimeError('Build environment receipt failed verification')
    for name, dependency in environment['dependencies'].items():
        if name not in {'juce', 'whisper'} or file_hash(archive / 'build-dependencies' / (name + '.patch')) != dependency['tracked_patch_sha256']:
            raise RuntimeError('Dependency patch receipt failed verification')


def copy_build_environment(source_archive, destination, prefix=''):
    if (source_archive / 'build-environment.json').exists():
        shutil.copy2(source_archive / 'build-environment.json', destination / (prefix + 'build-environment.json'))
    if (source_archive / 'build-dependencies').exists():
        shutil.copytree(source_archive / 'build-dependencies', destination / (prefix + 'build-dependencies'))


def source_inventory(root, omitted=()):
    names = set(os.fsdecode(x) for x in git(root, 'ls-files', '-z', '--cached', '--others', '--exclude-standard').split(b'\0') if x)
    names.update(n for n in REQUIRED_DATA if (root / n).exists())
    result = {}
    for name in sorted(names):
        item = root / name
        if not allowed(name) or any(item == p or p in item.parents for p in omitted):
            continue
        if not item.exists() and not item.is_symlink():  # Tracked deletion is intentional.
            continue
        resolved = item.resolve()
        if resolved != root and root not in resolved.parents:
            raise RuntimeError('Source symlink leaves repository; vendor its required content: ' + name)
        info = entry(item)
        if info['type'] == 'directory':
            raise RuntimeError('Submodule/directory must be explicitly vendored before archiving: ' + name)
        if info.get('size', 0) > 50 * 1024 * 1024:
            raise RuntimeError('Unexpected source file over 50 MiB; move generated/private data outside source: ' + name)
        result[name] = info
    if not result:
        raise RuntimeError('No source files found')
    return result


def verify_source_tar(path, inventory):
    with tarfile.open(path, 'r:gz') as archive:
        members = archive.getmembers()
        if len(members) != len(inventory):
            raise RuntimeError('Source archive member count mismatch')
        seen = set()
        for member in members:
            name = member.name.removeprefix('source/')
            if name in seen or name not in inventory or member.name != 'source/' + name:
                raise RuntimeError('Unexpected source archive member')
            seen.add(name)
            expected = inventory[name]
            if stat.S_IMODE(member.mode) != expected['mode']:
                raise RuntimeError('Source archive permissions differ: ' + name)
            if expected['type'] == 'symlink':
                ok = member.issym() and member.linkname == expected['target']
            else:
                stream = archive.extractfile(member) if member.isfile() else None
                ok = stream is not None and digest(stream.read()) == expected['sha256']
            if not ok:
                raise RuntimeError('Source archive content differs: ' + name)


def capture_source(root, archive, omitted):
    inventory = source_inventory(root, omitted)
    with tarfile.open(archive / 'source.tar.gz', 'w:gz', dereference=False) as bundle:
        for name in inventory:
            bundle.add(root / name, arcname='source/' + name, recursive=False)
    verify_source_tar(archive / 'source.tar.gz', inventory)
    if source_inventory(root, omitted) != inventory:
        raise RuntimeError('Source changed during snapshot; retry when editing has stopped')
    save(archive / 'source-inventory.json', inventory)
    return {'inventory_sha256': digest(canonical(inventory)),
            'archive_sha256': file_hash(archive / 'source.tar.gz'),
            'git_head': git(root, 'rev-parse', 'HEAD').decode().strip(),
            'git_status': git(root, 'status', '--short').decode(),
            'file_count': len(inventory)}


def verified_source(receipt):
    archive = Path(receipt['archive'])
    manifest = read(archive / 'manifest.json')
    source = manifest['source']
    inventory = read(archive / 'source-inventory.json')
    if (manifest['status'] != 'complete' or source['inventory_sha256'] != receipt['inventory_sha256']
            or digest(canonical(inventory)) != source['inventory_sha256']
            or file_hash(archive / 'source.tar.gz') != source['archive_sha256']):
        raise RuntimeError('Source snapshot receipt failed verification')
    verify_source_tar(archive / 'source.tar.gz', inventory)
    verify_build_environment(archive, source)
    return archive, source, inventory


def receipt_path(root, destination):
    return root / 'receipts' / (digest(str(destination).encode()) + '.json')


def previous_source(root, destination, inventory, archive):
    receipt = receipt_path(root, destination)
    if not receipt.exists():
        return {'status': 'current-source-not-proven-matched'}
    old = read(receipt)
    if old['bundle_sha256'] != digest(canonical(inventory)):
        return {'status': 'installed-bundle-changed-since-receipt'}
    prior, source, _ = verified_source(old)
    shutil.copy2(prior / 'source.tar.gz', archive / 'previous-source.tar.gz')
    shutil.copy2(prior / 'source-inventory.json', archive / 'previous-source-inventory.json')
    copy_build_environment(prior, archive, 'previous-')
    return {'status': 'matched-prior-installation-receipt', 'archive': str(prior), **source}


def install_bundle(incoming, destination, expected):
    token = uuid.uuid4().hex
    staged = destination.with_name('.' + destination.name + '.install-' + token)
    previous = destination.with_name('.' + destination.name + '.previous-' + token)
    moved_old = installed_new = False
    try:
        copy_bundle(incoming, staged)
        if destination.exists() or destination.is_symlink():
            os.replace(destination, previous)
            moved_old = True
        os.replace(staged, destination)
        installed_new = True
        if bundle_inventory(destination) != expected:
            raise RuntimeError('Installed bundle verification failed')
    except BaseException:
        if installed_new:
            shutil.rmtree(destination)
        if moved_old:
            os.replace(previous, destination)
        raise
    finally:
        if staged.exists():
            shutil.rmtree(staged)
    if moved_old:
        shutil.rmtree(previous)


def run(args):
    root = args.source_root.resolve()
    archive_root = args.archive_root.resolve()
    omitted = [archive_root]
    if getattr(args, 'build_dir', None):
        args.build_dir = args.build_dir.resolve()
        omitted.append(args.build_dir)
    with locked(archive_root):
        archive = new_archive(archive_root, args.command, args.label)
        manifest = {'schema': 1, 'kind': args.command, 'status': 'incomplete', 'label': args.label}
        save(archive / 'manifest.json', manifest)
        try:
            if args.command in {'prepare', 'snapshot'}:
                environment = build_environment(args.build_dir, archive) if args.command == 'prepare' else None
                manifest['source'] = capture_source(root, archive, omitted)
                if environment is not None:
                    if build_environment(args.build_dir) != environment:
                        raise RuntimeError('Build settings or dependencies changed during preparation; retry')
                    manifest['source']['build_environment_sha256'] = digest(canonical(environment))
                if args.command == 'snapshot':
                    manifest['source_provenance'] = 'current-source-not-proven-matched'
                    manifest['bundles'] = []
                    for number, source in enumerate(args.bundle):
                        source = source.resolve()
                        copy_bundle(source, archive / 'bundles' / str(number) / source.name)
                        inventory = bundle_inventory(source)
                        manifest['bundles'].append({'path': str(source), 'inventory': inventory,
                            'source': previous_source(archive_root, source, inventory, archive / 'bundles' / str(number))})
                manifest['status'] = 'complete'
                save(archive / 'manifest.json', manifest)
                if args.command == 'prepare':
                    args.build_dir.mkdir(parents=True, exist_ok=True)
                    save(args.build_dir / 'novox-source-snapshot.json', {'archive': str(archive),
                         'inventory_sha256': manifest['source']['inventory_sha256']})
            else:
                source_archive, source, inventory = verified_source(read(args.build_dir / 'novox-source-snapshot.json'))
                if source_inventory(root, omitted) != inventory:
                    raise RuntimeError('Source changed since build preparation; rebuild before installing')
                if build_environment(args.build_dir) != read(source_archive / 'build-environment.json'):
                    raise RuntimeError('Build settings or dependencies changed since preparation; rebuild before installing')
                manifest['source'] = source
                manifest['source_provenance'] = 'matched-build-preparation'
                manifest['build_source_archive'] = str(source_archive)
                for name in ('source.tar.gz', 'source-inventory.json'):
                    shutil.copy2(source_archive / name, archive / name)
                copy_build_environment(source_archive, archive)
                incoming = args.bundle.resolve()
                destination = args.destination.resolve() / incoming.name
                if incoming == destination:
                    raise RuntimeError('Build bundle and installation destination are identical')
                destination.parent.mkdir(parents=True, exist_ok=True)
                expected = bundle_inventory(incoming)
                copy_bundle(incoming, archive / 'incoming' / incoming.name)
                manifest['incoming'] = {'path': str(incoming), 'inventory': expected}
                manifest['destination'] = str(destination)
                if destination.exists() or destination.is_symlink():
                    before = bundle_inventory(destination)
                    copy_bundle(destination, archive / 'previous' / destination.name)
                    manifest['previous'] = {'inventory': before,
                        'source': previous_source(archive_root, destination, before, archive)}
                manifest.update(status='complete', installation='pending')
                save(archive / 'manifest.json', manifest)  # Fully backed up BEFORE mutating destination.
                install_bundle(archive / 'incoming' / incoming.name, destination, expected)
                manifest['installation'] = 'verified'
                save(archive / 'manifest.json', manifest)
                receipt = receipt_path(archive_root, destination)
                receipt.parent.mkdir(parents=True, exist_ok=True)
                save(receipt, {'archive': str(archive), 'inventory_sha256': source['inventory_sha256'],
                     'bundle_sha256': digest(canonical(expected))})
            return archive
        except BaseException as error:
            manifest['error'] = str(error)
            save(archive / 'manifest.json', manifest)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('prepare', 'snapshot', 'install'):
        command = commands.add_parser(name)
        command.add_argument('--source-root', type=Path, required=True)
        command.add_argument('--archive-root', type=Path, required=True)
        command.add_argument('--label', default='')
        if name != 'snapshot':
            command.add_argument('--build-dir', type=Path, required=True)
        if name == 'snapshot':
            command.add_argument('--bundle', type=Path, action='append', required=True)
        if name == 'install':
            command.add_argument('--bundle', type=Path, required=True)
            command.add_argument('--destination', type=Path, required=True)
    try:
        print(run(parser.parse_args()))
    except Exception as error:
        parser.exit(1, 'Plugin archive failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()

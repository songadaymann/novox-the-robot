"""Safety checks for local plugin/source archives; never touches installed plugins."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('plugin_archive', Path(__file__).parents[1] / 'scripts' / 'plugin_archive.py')
archive = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(archive)


class PluginArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='novox archive ü ')
        self.base = Path(self.temp.name)
        self.source = self.base / 'source with spaces'
        self.source.mkdir()
        self.archives = self.base / 'dated archives'
        self.build = self.base / 'build tree'
        self.destination = self.base / 'Audio Plug-Ins'
        self.destination.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.source)], check=True)
        (self.source / 'main.cpp').write_text('original\n')
        (self.source / '.gitignore').write_text('data/\n.env*\nbuild*/\n')
        subprocess.run(['git', '-C', str(self.source), 'add', '.'], check=True)
        subprocess.run(['git', '-C', str(self.source), '-c', 'user.name=Archive Test',
                        '-c', 'user.email=archive@example.invalid', 'commit', '-qm', 'baseline'], check=True)
        self.incoming = self.bundle(self.base / 'compiled' / 'Növøx.component', 'new binary')
        self.installed = self.bundle(self.destination / 'Növøx.component', 'old binary')

    def tearDown(self):
        self.temp.cleanup()

    def bundle(self, path, content):
        contents = path / 'Contents'
        contents.mkdir(parents=True)
        binary = contents / 'binary'
        binary.write_text(content)
        binary.chmod(0o755)
        (contents / 'resource ü.txt').write_text('resource')
        (contents / 'alias').symlink_to('resource ü.txt')
        return path

    def args(self, command, **kwargs):
        values = dict(command=command, source_root=self.source, archive_root=self.archives,
                      build_dir=self.build, bundle=self.incoming, destination=self.destination, label='test')
        values.update(kwargs)
        return argparse.Namespace(**values)

    def prepare(self):
        return archive.run(self.args('prepare'))

    def test_dirty_untracked_source_and_required_ignored_dictionary(self):
        (self.source / 'main.cpp').write_text('dirty change\n')
        (self.source / 'new ü.cpp').write_text('untracked implementation')
        (self.source / 'data').mkdir()
        (self.source / 'data' / 'cmudict.dict').write_text('WORD W ER D')
        (self.source / 'data' / 'cmudict.LICENSE').write_text('dictionary license')
        (self.source / 'data' / 'private-recording.wav').write_bytes(b'private')
        (self.source / '.env').write_text('SECRET=not-for-the-archive')
        (self.source / '.env.example').write_text('SECRET=')
        subprocess.run(['git', '-C', str(self.source), 'add', '-f', '.env', '.env.example', 'data/private-recording.wav'], check=True)
        (self.source / 'private-assets').mkdir()
        (self.source / 'private-assets' / 'secret.wav').write_bytes(b'private')
        (self.source / 'credentials.json').write_text('secret')
        (self.source / 'main-link.cpp').symlink_to('main.cpp')
        result = self.prepare()
        inventory = archive.read(result / 'source-inventory.json')
        self.assertIn('new ü.cpp', inventory)
        self.assertIn('.env.example', inventory)
        self.assertIn('data/cmudict.dict', inventory)
        self.assertIn('data/cmudict.LICENSE', inventory)
        self.assertEqual(inventory['main-link.cpp']['target'], 'main.cpp')
        self.assertFalse(set(inventory) & {'.env', 'credentials.json', 'data/private-recording.wav', 'private-assets/secret.wav'})
        with tarfile.open(result / 'source.tar.gz') as tar:
            self.assertEqual(tar.extractfile('source/main.cpp').read(), b'dirty change\n')
        self.assertIn(' M main.cpp', archive.read(result / 'manifest.json')['source']['git_status'])

    def test_external_source_symlink_fails_closed(self):
        external = self.base / 'private-token.txt'
        external.write_text('private')
        (self.source / 'external.cpp').symlink_to(external)
        with self.assertRaisesRegex(RuntimeError, 'symlink leaves repository'):
            self.prepare()

    def test_unique_snapshots_never_overwrite_and_unproven_old_source_is_explicit(self):
        first = archive.run(self.args('snapshot', bundle=[self.installed]))
        original = (first / 'manifest.json').read_bytes()
        second = archive.run(self.args('snapshot', bundle=[self.installed]))
        self.assertNotEqual(first, second)
        self.assertEqual((first / 'manifest.json').read_bytes(), original)
        self.assertEqual(archive.read(first / 'manifest.json')['source_provenance'], 'current-source-not-proven-matched')
        with self.assertRaisesRegex(RuntimeError, 'overwrite'):
            archive.copy_bundle(self.incoming, first / 'bundles' / '0' / self.installed.name)

    def test_install_preserves_binary_resources_permissions_and_matching_source_for_next_version(self):
        self.prepare()
        old = archive.bundle_inventory(self.installed)
        first = archive.run(self.args('install'))
        self.assertEqual(archive.bundle_inventory(self.installed), archive.bundle_inventory(self.incoming))
        self.assertEqual(archive.bundle_inventory(first / 'previous' / self.installed.name), old)
        self.assertEqual(archive.read(first / 'manifest.json')['previous']['source']['status'], 'current-source-not-proven-matched')
        (self.incoming / 'Contents' / 'binary').write_text('next binary')
        second = archive.run(self.args('install'))
        manifest = archive.read(second / 'manifest.json')
        self.assertEqual(manifest['previous']['source']['status'], 'matched-prior-installation-receipt')
        self.assertTrue((second / 'previous-source.tar.gz').exists())
        self.assertEqual(archive.file_hash(second / 'previous-source.tar.gz'), archive.file_hash(first / 'source.tar.gz'))
        self.assertEqual(manifest['installation'], 'verified')

    def test_changed_source_and_tampered_snapshot_refuse_install(self):
        prepared = self.prepare()
        before = archive.bundle_inventory(self.installed)
        (self.source / 'main.cpp').write_text('changed during compilation')
        with self.assertRaisesRegex(RuntimeError, 'Source changed'):
            archive.run(self.args('install'))
        self.assertEqual(archive.bundle_inventory(self.installed), before)
        (self.source / 'main.cpp').write_text('original\n')
        (prepared / 'source.tar.gz').write_bytes(b'tampered')
        with self.assertRaisesRegex(RuntimeError, 'receipt failed'):
            archive.run(self.args('install'))
        self.assertEqual(archive.bundle_inventory(self.installed), before)

    def test_build_settings_and_dependency_changes_refuse_install(self):
        dependency = self.base / 'cached whisper'
        dependency.mkdir()
        subprocess.run(['git', 'init', '-q', str(dependency)], check=True)
        (dependency / 'whisper.cpp').write_text('upstream implementation')
        subprocess.run(['git', '-C', str(dependency), 'add', '.'], check=True)
        subprocess.run(['git', '-C', str(dependency), '-c', 'user.name=Archive Test',
                        '-c', 'user.email=archive@example.invalid', 'commit', '-qm', 'dependency baseline'], check=True)
        (dependency / 'whisper.cpp').write_text('local required fix')
        self.build.mkdir()
        cache = self.build / 'CMakeCache.txt'
        original_cache = ('CMAKE_BUILD_TYPE:STRING=Release\n'
                          'CMAKE_CXX_FLAGS:STRING=-O2\n'
                          'NOVOX_BUILD_PLUGIN:BOOL=ON\n'
                          'PRIVATE_API_TOKEN:STRING=must-not-be-saved\n'
                          'NOVOX_SECRET:STRING=must-not-be-saved-either\n'
                          'FETCHCONTENT_SOURCE_DIR_WHISPER:PATH=' + str(dependency) + '\n')
        cache.write_text(original_cache)
        prepared = self.prepare()
        environment = archive.read(prepared / 'build-environment.json')
        self.assertEqual(environment['cache']['CMAKE_BUILD_TYPE'], 'Release')
        self.assertNotIn('PRIVATE_API_TOKEN', environment['cache'])
        self.assertNotIn('NOVOX_SECRET', environment['cache'])
        self.assertIn('+local required fix', (prepared / 'build-dependencies' / 'whisper.patch').read_text())
        before = archive.bundle_inventory(self.installed)
        cache.write_text(original_cache.replace('-O2', '-O0'))
        with self.assertRaisesRegex(RuntimeError, 'settings or dependencies changed'):
            archive.run(self.args('install'))
        cache.write_text(original_cache)
        (dependency / 'whisper.cpp').write_text('changed while compiling')
        with self.assertRaisesRegex(RuntimeError, 'settings or dependencies changed'):
            archive.run(self.args('install'))
        self.assertEqual(archive.bundle_inventory(self.installed), before)
        (dependency / 'whisper.cpp').write_text('local required fix')
        installed_archive = archive.run(self.args('install'))
        self.assertEqual(archive.read(installed_archive / 'build-environment.json'), environment)
        (dependency / 'extra.cpp').write_text('untracked compiled input')
        with self.assertRaisesRegex(RuntimeError, 'Untracked dependency source'):
            self.prepare()

    def test_failed_backup_leaves_installed_bundle_untouched(self):
        self.prepare()
        before = archive.bundle_inventory(self.installed)
        real_copy = archive.copy_bundle
        def fail_backup(source, destination):
            if 'previous' in destination.parts:
                raise OSError('simulated backup disk failure')
            return real_copy(source, destination)
        with mock.patch.object(archive, 'copy_bundle', side_effect=fail_backup):
            with self.assertRaisesRegex(OSError, 'backup disk failure'):
                archive.run(self.args('install'))
        self.assertEqual(archive.bundle_inventory(self.installed), before)

    def test_failed_swap_rolls_back_existing_bundle(self):
        before = archive.bundle_inventory(self.installed)
        real_replace = archive.os.replace
        def fail_new_rename(source, destination):
            if '.install-' in Path(source).name:
                raise OSError('simulated swap failure')
            return real_replace(source, destination)
        with mock.patch.object(archive.os, 'replace', side_effect=fail_new_rename):
            with self.assertRaisesRegex(OSError, 'swap failure'):
                archive.install_bundle(self.incoming, self.installed, archive.bundle_inventory(self.incoming))
        self.assertEqual(archive.bundle_inventory(self.installed), before)
        self.assertEqual(list(self.destination.iterdir()), [self.installed])

    def test_failed_final_verification_rolls_back(self):
        before = archive.bundle_inventory(self.installed)
        with self.assertRaisesRegex(RuntimeError, 'verification failed'):
            archive.install_bundle(self.incoming, self.installed, {'invalid': {}})
        self.assertEqual(archive.bundle_inventory(self.installed), before)


if __name__ == '__main__':
    unittest.main()

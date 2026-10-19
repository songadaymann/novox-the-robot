# Plugin backups and recovery

Normal macOS CMake builds automatically archive every newly linked plugin before
installing it. This covers the Novox AU and VST3, the compatibility AU used by older
Logic projects, and optional AAX builds. The standalone app remains in the build
folder. Existing builds pick up these hooks when CMake regenerates their project.

```sh
cmake -S . -B build-plugin -DCMAKE_BUILD_TYPE=Release -DNOVOX_BUILD_PLUGIN=ON
cmake --build build-plugin --target NovoxTheRobot_All -j 6
```

Python 3.9+ and Git must be available. The default archive is
`out/plugin-versions/`, outside the build folder. A different retained location can
be configured with `-DNOVOX_PLUGIN_ARCHIVE_DIR=/path/to/plugin-backups`.
This directory is private local build history: it is not committed or published.
Do not delete it when cleaning build output. Include it in your normal backups.
The build never deletes old versions or imposes a retention limit.

## What is saved

Folders use UTC timestamps plus unique suffixes so repeated builds cannot overwrite
an earlier version. A build preparation folder contains:

- `source.tar.gz` and `source-inventory.json`: the actual source files, including
  uncommitted edits and untracked project files, with hashes and permissions.
- `build-environment.json`: selected build settings and JUCE/Whisper commit IDs.
- `build-dependencies/`: exact tracked dependency patches, including the Whisper
  alignment fix when present.
- `manifest.json`: source checksums, Git revision and working-tree status.

Private recordings, models, credentials, dependency checkout folders, and generated
build output are excluded. The required pronunciation dictionary and its license
are included. Dependencies are identified by revision and patches rather than
duplicated wholesale; rebuilding requires access to those upstream revisions and
the platform toolchain. An external AAX SDK, when used, must be retained separately.

Each installation folder additionally contains the full `incoming/` plugin bundle,
the full `previous/` installed bundle (if one existed), source and build provenance,
and final bundle hashes. When the prior version has a matching installation receipt,
its source is also copied into `previous-source.tar.gz`, with its inventory and build
settings. For an older binary that predates this system, the manifest explicitly
marks its source provenance as unverified; a source tree captured today is not
automatically considered a match for that binary.

Source files, selected settings, and dependency revisions/patches must stay unchanged
from preparation through installation. If they change, the installer stops; run the
build again after editing is finished. A failed backup prevents replacement. The
installer stages and checks a copy before replacing the destination and restores
the old bundle if replacement or its verification fails. Installations are serialized
to keep concurrent format builds from racing. A complete archive with
`installation: verified` confirms the copied bundle passed verification.

## Recovering a saved plugin

1. Save your DAW project and quit the DAW before restoring a plugin.
2. Preserve the currently installed version first. From the repository, run the
   following command (use your configured archive root if different):

   ```sh
   python3 scripts/plugin_archive.py snapshot --source-root . \
     --archive-root out/plugin-versions --label before-restore \
     --bundle "$HOME/Library/Audio/Plug-Ins/Components/Novox The Robot.component"
   ```

3. Choose an archive with a complete manifest. Copy the desired whole bundle from
   `incoming/`, `previous/`, or a manual snapshot's `bundles/` subfolder back into
   the matching `~/Library/Audio/Plug-Ins/Components` or `VST3` folder, replacing
   that bundle. Preserve the bundle name; the compatibility AU is
   `New Virtual Singer.component` and uses a different identity for old projects.
4. Reopen the DAW. If Logic lists a failed validation, rescan that plugin in its
   Plug-in Manager. Do not delete unrelated plugin caches.

You can recover source by extracting `source.tar.gz` into a new folder; the tree
is under `source/`. For the replaced version use `previous-source.tar.gz`, when
present. Keep the build settings and dependency patches beside the recovered tree.
Never extract a historical version over a working checkout with unsaved edits.

## Checks

```sh
python3 -m unittest discover -s tests -p test_plugin_archive.py
```

These checks exercise backup failures, rollback, archive verification, dirty source,
required data, secret exclusions, and refusal after source/configuration/dependency
changes. The installer also verifies actual bundle copies on each normal build.

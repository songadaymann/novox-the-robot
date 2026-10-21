# CLI and language-package releases

`.github/workflows/release.yml` runs on a pushed `v*` tag. It uploads CLI archives
to GitHub and publishes Python and npm packages. Running or rerunning it is a
publication action, not a way to test local repairs. The Mac app/plugin installer
has its own [packaging guide](../packaging/macos/README.md).

## npm authentication

The workflow uses the repository Actions secret **NPM_TOKEN**, mapped to
`NODE_AUTH_TOKEN` by the workflow and consumed by the registry configuration from
`actions/setup-node`. A local `npm login` does not authenticate a GitHub runner.

The October 19, 2026 release failed with `ENEEDAUTH`: the publish step's
`NODE_AUTH_TOKEN` was empty. The workflow now checks for the token and runs
`npm whoami` before installing Emscripten or building. A missing token or failed
identity request produces an actionable error without printing the token or
registry response. A successful identity check does not prove package write
permissions, available versions, or compliance with the package's 2FA policy.

**Maintainer action still required:** using an npm account allowed to publish
`novox`, provide a valid granular access token with **Read and write (publish and
stage)** permission for the package. For unattended token publication when 2FA is
required, select **Bypass two-factor authentication** on that token. Store the token
directly in GitHub → `songadaymann/novox-the-robot` → Settings → Secrets and
variables → Actions → New repository secret, named `NPM_TOKEN` (or update it there).
Do not paste it into a conversation or commit it. No credentials or account
settings were inspected or changed during the local repair.

See npm's [token creation instructions](https://docs.npmjs.com/creating-and-viewing-access-tokens/)
and [CI authentication guidance](https://docs.npmjs.com/using-private-packages-in-a-ci-cd-workflow/).
Switching to trusted publishing would require a separate npm configuration and
workflow change; the current workflow continues to use its existing token model.

## Local checks

Run the engine suite in a separate build directory:

```sh
cmake -S . -B out/ci-repair/native -DCMAKE_BUILD_TYPE=Release -DNOVOX_BUILD_PLUGIN=OFF
cmake --build out/ci-repair/native --config Release -j 4
ctest --test-dir out/ci-repair/native -C Release --output-on-failure
```

The release shell checks require Python and PyYAML (already in `tools/.venv`):

```sh
tools/.venv/bin/python -m unittest discover -s tests -p '*_workflow_tests.py' -v
```

They execute the workflow's actual configure and auth scripts against local
stubs: the Mac architecture list must reach CMake as one argument, and missing,
rejected, and accepted tokens must follow the intended paths without leaking
output. They neither authenticate nor publish.

On a Mac with the command-line developer tools, test both CLI architectures:

```sh
cmake -S . -B out/ci-repair/universal -DCMAKE_BUILD_TYPE=Release -DNOVOX_BUILD_TESTS=OFF \
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build out/ci-repair/universal --config Release -j 4 --target novox
lipo -archs out/ci-repair/universal/novox
```

These local checks cannot confirm MSVC/Windows builds, GitHub runner availability,
or npm/PyPI publication. Both language packages currently declare version `0.1.0`,
independently of the app's `1.0 beta`; those versions were not changed by this repair.
Before a later approved publication, review the intended package versions and
registry setup. Rerunning the original beta workflow uses the old tagged source,
not these local fixes. Preserve the existing `v1.0.0-beta.1` installer and checksum.

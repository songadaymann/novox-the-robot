"""Offline release-workflow checks: python -m unittest discover -s tests -p '*_workflow_tests.py'.

Requires PyYAML. The shell commands come from the workflow itself; cmake and npm
are replaced with local stubs. No credentials, network, builds or publishing.
"""

from pathlib import Path
import subprocess
import tempfile
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[1]


class ReleaseWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / ".github/workflows/release.yml").read_text(), Loader=yaml.BaseLoader
        )

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="novox-workflow-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        # Deliberately do not inherit npm configuration or credentials.
        self.env = {"PATH": f"{self.directory}:/usr/bin:/bin"}

    def stub(self, name, body):
        path = self.directory / name
        path.write_text("#!/bin/bash\n" + body)
        path.chmod(0o755)

    def run_step(self, script):
        return subprocess.run(
            ["/bin/bash", "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", script],
            cwd=self.directory, env=self.env, text=True, capture_output=True,
        )

    def auth_step(self):
        return next(
            step for step in self.workflow["jobs"]["npm"]["steps"]
            if step.get("name") == "Check npm publishing credentials"
        )

    def test_macos_architectures_reach_cmake_as_one_argument(self):
        cli = self.workflow["jobs"]["cli"]
        matrix = next(row for row in cli["strategy"]["matrix"]["include"]
                      if row["name"] == "macos-universal")
        configure = next(step["run"] for step in cli["steps"]
                         if step.get("run", "").startswith("cmake -S"))
        self.stub("cmake", 'printf "%s\\n" "$@" >> cmake-args\n')
        result = self.run_step(configure.replace("${{ matrix.flags }}", matrix["flags"]))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.directory / "cmake-args").read_text().splitlines(), [
            "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release",
            "-DNOVOX_BUILD_TESTS=OFF", "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0",
        ])

    def test_missing_or_empty_token_stops_before_npm(self):
        self.stub("npm", 'touch npm-called\nexit 0\n')
        for token in (None, ""):
            with self.subTest(token=token):
                if token is not None:
                    self.env["NODE_AUTH_TOKEN"] = token
                result = self.run_step(self.auth_step()["run"])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("NPM_TOKEN is missing", result.stdout)
                self.assertFalse((self.directory / "npm-called").exists())

    def test_rejected_token_stops_without_echoing_token_or_npm_output(self):
        self.env["NODE_AUTH_TOKEN"] = "dummy-test-token-not-a-credential"
        self.stub("npm", 'echo "$NODE_AUTH_TOKEN"\necho "private-stub-error" >&2\nexit 1\n')
        result = self.run_step(self.auth_step()["run"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("npm authentication failed", result.stdout)
        self.assertNotIn(self.env["NODE_AUTH_TOKEN"], result.stdout + result.stderr)
        self.assertNotIn("private-stub-error", result.stdout + result.stderr)

    def test_accepted_token_checks_identity_only(self):
        self.env["NODE_AUTH_TOKEN"] = "dummy-test-token-not-a-credential"
        self.stub("npm", 'printf "%s\\n" "$@" > npm-args\necho "stub-user"\n')
        result = self.run_step(self.auth_step()["run"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.directory / "npm-args").read_text().splitlines(),
                         ["whoami", "--registry", "https://registry.npmjs.org"])
        self.assertEqual(result.stdout + result.stderr, "")

    def test_auth_precedes_emscripten_build_and_uses_publish_token(self):
        steps = self.workflow["jobs"]["npm"]["steps"]
        auth = self.auth_step()
        setup = next(step for step in steps if step.get("uses", "").startswith("actions/setup-node@"))
        emsdk = next(step for step in steps if step.get("uses", "").startswith("mymindstorm/setup-emsdk@"))
        build = next(step for step in steps if step.get("run") == "./scripts/build-wasm.sh")
        publish = next(step for step in steps if step.get("run", "").startswith("npm publish"))
        self.assertLess(steps.index(setup), steps.index(auth))
        self.assertLess(steps.index(auth), steps.index(emsdk))
        self.assertLess(steps.index(emsdk), steps.index(build))
        self.assertLess(steps.index(build), steps.index(publish))
        self.assertEqual(auth["env"], publish["env"])
        self.assertEqual(auth["env"]["NODE_AUTH_TOKEN"], "${{ secrets.NPM_TOKEN }}")


if __name__ == "__main__":
    unittest.main()

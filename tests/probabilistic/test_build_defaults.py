"""Default-on Bloom must not auto-load the conflicting external module."""
from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@contextmanager
def fixture():
    # Exercise the real scripts against disposable artifacts/configs only.
    with tempfile.TemporaryDirectory(prefix="bloom-config-") as directory:
        root = Path(directory)
        (root / "src").mkdir()
        for binary in ("redis-server", "redis-cli", "redis-benchmark"):
            path = root / "src" / binary
            path.write_text('#!' + shutil.which('sh') + '\nprintf "%s\\n" "$@"\n')
            path.chmod(0o755)
        entries = ["modules:"]
        for name in ("redisbloom", "other"):
            module = root / "modules" / name
            (module / "src").mkdir(parents=True)
            (module / "src" / ".prepared").touch()
            (module / (name + ".so")).touch()
            (module / "src" / "module.conf").write_text(
                'cf-initial-size 1024\n' if name == 'redisbloom' else '# other config\n')
            entries += [f"  - name: {name}", f"    target_module: {name}.so",
                        f"    loadmodule: ./modules/{name}/{name}.so"]
        (root / "modules" / "modules.yaml").write_text('\n'.join(entries) + '\n')
        (root / "redis.conf").write_text('port 0\n')
        env = dict(os.environ, REPO_ROOT=str(root), PREFIX='', PROG_SUFFIX='',
                   MODULES_MANIFEST_FILE=str(root / 'modules/modules.yaml'),
                   REDIS_CONF='redis.conf', REDIS_GEN_CONF='redis-full.conf',
                   MODULES='all', ASSUME_BUILT='1', DESTDIR='', SKIP_BUILD='1', ARGS='--version')
        env.pop('BUILD_BLOOM', None)
        yield root, env


def script(name, env, *args):
    return subprocess.run([str(ROOT / 'scripts' / name), *args], env=env,
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


class testBuildDefaults(unittest.TestCase):
    def test_generated_config_default_and_opt_out(self):
        with fixture() as (root, env):
            for mode in (None, 'no', 'yes'):
                if mode is not None:
                    env['BUILD_BLOOM'] = mode
                result = script('sync-redis-conf.sh', env)
                self.assertEqual(0, result.returncode, result.stdout)
                config = (root / 'redis-full.conf').read_text()
                self.assertIn('loadmodule ./modules/other/other.so', config)
                self.assertNotIn('\nloadmodule ./modules/redisbloom/', config)
                self.assertNotIn('\ncf-initial-size', config)
                env['REDIS_CONF'] = 'redis-full.conf'

    def test_run_skips_module_and_rejects_explicit_conflict(self):
        with fixture() as (_, env):
            result = script('run.sh', env)
            self.assertEqual(0, result.returncode, result.stdout)
            self.assertIn('Skipping redisbloom', result.stdout)
            self.assertIn('Loading other', result.stdout)
            self.assertNotIn('Loading redisbloom', result.stdout)
            result = script('run.sh', env, 'redisbloom')
            self.assertNotEqual(0, result.returncode)
            self.assertIn('no longer a bundled module', result.stdout)
            env['BUILD_BLOOM'] = 'no'
            result = script('run.sh', env, 'redisbloom')
            self.assertNotEqual(0, result.returncode)
            self.assertNotIn('Loading redisbloom', result.stdout)

    def test_deploy_does_not_reintroduce_module(self):
        with fixture() as (root, env):
            env['BUILD_BLOOM'] = 'no'
            result = script('sync-redis-conf.sh', env)
            self.assertEqual(0, result.returncode, result.stdout)
            env['PREFIX'] = str(root / 'installed')
            env['BUILD_BLOOM'] = 'yes'
            result = script('deploy.sh', env)
            self.assertEqual(0, result.returncode, result.stdout)
            config = (root / 'redis-full.conf').read_text()
            self.assertIn('loadmodule ' + env['PREFIX'] + '/lib/redis/modules/other.so', config)
            self.assertNotIn('/redisbloom.so', config)
            self.assertNotIn('cf-initial-size', config)

    def test_core_only_deploy_removes_stale_bloom_load(self):
        with fixture() as (root, env):
            env['BUILD_BLOOM'] = 'no'
            result = script('sync-redis-conf.sh', env)
            self.assertEqual(0, result.returncode, result.stdout)
            config_path = root / 'redis-full.conf'
            with config_path.open('a') as legacy:
                legacy.write('\n# >>> BEGIN module: redisbloom <<<\ncf-initial-size 1024\n# <<< END module: redisbloom <<<\n')
            env['PREFIX'] = str(root / 'installed')
            env['BUILD_BLOOM'] = 'yes'
            result = script('deploy.sh', env, 'redis')
            self.assertEqual(0, result.returncode, result.stdout)
            config = (root / 'redis-full.conf').read_text()
            self.assertIn('loadmodule ./modules/other/other.so', config)
            self.assertNotIn('/redisbloom.so', config)
            self.assertNotIn('cf-initial-size', config)

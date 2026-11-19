"""Run the real init script with local command mocks, never set the host clock."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'board/x96max/overlay/etc/init.d/S41settime'


class SettimeTests(unittest.TestCase):
    def run_script(self, action, epoch='1794990000'):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            calls = base / 'calls'
            commands = {
                'wget': 'printf "%s\\n" "$MOCK_EPOCH"',
                'date': 'echo mock-time',
                'sleep': ':',
            }
            for name, result in commands.items():
                command = base / name
                command.write_text('#!/bin/sh\n'
                                   f'echo "{name} $*" >> "$MOCK_CALLS"\n{result}\n')
                command.chmod(0o700)
            text = SCRIPT.read_text().replace(
                '. /etc/default/pix-stream-linux 2>/dev/null || true', ':')
            env = dict(os.environ, PATH=f'{base}:/usr/bin:/bin',
                       MOCK_CALLS=str(calls), MOCK_EPOCH=epoch)
            args = ['sh', '-c', text, 'settime-test']
            if action is not None:
                args.append(action)
            result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=5)
            return result, calls.read_text().splitlines() if calls.exists() else []

    def test_stop_does_not_contact_network_set_clock_or_sleep(self):
        result, calls = self.run_script('stop')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [])

    def test_start_sets_clock_and_stops_retrying(self):
        result, calls = self.run_script('start')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sum(x.startswith('wget ') for x in calls), 1)
        self.assertIn('date -u -s @1794990000', calls)
        self.assertFalse(any(x.startswith('sleep ') for x in calls))

    def test_network_failure_is_bounded_and_does_not_set_clock(self):
        result, calls = self.run_script('start', epoch='')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sum(x.startswith('wget ') for x in calls), 30)
        self.assertEqual(sum(x.startswith('sleep ') for x in calls), 30)
        self.assertFalse(any(x.startswith('date ') for x in calls))
        self.assertIn('could not reach', result.stderr)

    def test_invalid_actions_fail_without_side_effects(self):
        for action in (None, 'restart', 'invalid'):
            with self.subTest(action=action):
                result, calls = self.run_script(action)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()

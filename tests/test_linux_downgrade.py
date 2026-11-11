"""Game-free: folder dispatch, default version admission and the real Tk update handlers before any worker starts."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder/linux"))
import linux_update


class Value:
    def __init__(self, value=""):
        self.value = value
    def get(self):
        return self.value
    def set(self, value):
        self.value = value


class Widget:
    def __init__(self):
        self.config = {}
        self.visible = False
    def winfo_exists(self):
        return True
    def configure(self, **kwargs):
        self.config.update(kwargs)
    def pack(self, **kwargs):
        self.visible = True
    def pack_forget(self):
        self.visible = False


def handlers(answer):
    """Compile complete production handlers; only UI/thread/filesystem observations are authored boundaries."""
    tree = ast.parse((ROOT / "builder/linux/hookshot_builder.py").read_text(encoding="utf-8"))
    builder = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "Builder")
    names = {"older_ok", "begin_update", "build", "apply_mode", "apply_launch"}
    methods = [n for n in builder.body if isinstance(n, ast.FunctionDef) and n.name in names]
    assert len(methods) == len(names)
    events = []
    popup = types.SimpleNamespace(askyesno=lambda *a: events.append("confirm") or answer)
    worker = types.SimpleNamespace(Thread=lambda **kw: types.SimpleNamespace(start=lambda: events.append("worker")))
    namespace = dict(os=os, linux_update=linux_update, messagebox=popup, threading=worker,
                     tk=types.SimpleNamespace(TclError=RuntimeError), folder_has_dlss=lambda _: False)
    exec(compile(ast.Module(body=methods, type_ignores=[]), "production-builder-handlers", "exec"), namespace)
    app = types.SimpleNamespace(tools="v2.1.0", latest=None, source_picked=False, update_tag=None,
                                fetched=True, update_folder="prior", out=Value("authored-game"),
                                switching=lambda: False, launch_folder="", build_button=Widget(),
                                status=Value(), launch_status=Value(), launch_update=Widget(), px=lambda n: n,
                                dlss=Value(True), dlss_toggled=False,
                                show_progress=lambda _: events.append("progress"))
    for name in names:
        setattr(app, name, types.MethodType(namespace[name], app))
    return app, events


class Admission(unittest.TestCase):
    def test_default_never_rebuilds_a_known_newer_game_backwards(self):
        for game, tools, latest in [("v2.2.0", "v2.1.0", None), ("v2.2.0", "v2.0.14", "v2.1.0"),
                                    ("v2.1.0", "v2.1.0-rc.10", None),
                                    ("v2.1.0-rc.10", "v2.1.0-rc.9", None),
                                    ("v2.1.0-3-gabc1234", "v2.1.0", None)]:
            with self.subTest(game=game, tools=tools, latest=latest):
                self.assertEqual(linux_update.choose_mode(game, tools, latest, False, True), "up-to-date")
                self.assertEqual(linux_update.choose_mode(game, tools, latest, False, True, True), "update-game")

    def test_forward_and_unordered_nightly_contract_is_preserved(self):
        for game, tools, latest in [("v2.0.9", "v2.0.10", None), ("v2.1.0-rc.9", "v2.1.0-rc.10", None),
                                    ("v2.1.0-rc.10", "v2.1.0", None), ("v2.1.0", "v2.1.0-3-gabc1234", None),
                                    ("v2.1.0-3-gabc1234", "v2.1.0-4-gdef1234", "v2.1.0-4-gdef1234"),
                                    ("authored-hash", "v2.1.0", None)]:
            with self.subTest(game=game, tools=tools):
                self.assertEqual(linux_update.choose_mode(game, tools, latest, False, True), "update-game")
        self.assertEqual(linux_update.choose_mode("v2.2.0", "v2.1.0", None, True, True), "build")
        self.assertEqual(linux_update.choose_mode("v2.2.0", "v2.1.0", None, False, False), "up-to-date")

    def test_direct_launcher_update_decline_starts_nothing_and_preserves_page(self):
        app, events = handlers(False)
        with mock.patch.object(linux_update, "game_version", return_value=("v2.2.0", True)):
            app.begin_update("authored-game")
        self.assertEqual(events, ["confirm"])
        self.assertEqual(app.update_folder, "prior")

    def test_acceptance_and_forward_update_start_exactly_one_worker(self):
        for game, answer, expected in [("v2.2.0", True, ["confirm", "progress", "worker"]),
                                        ("v2.0.14", False, ["progress", "worker"])]:
            app, events = handlers(answer)
            with mock.patch.object(linux_update, "game_version", return_value=(game, True)):
                app.begin_update("authored-game")
            self.assertEqual(events, expected)
            self.assertEqual(app.update_folder, "authored-game")

    def test_form_and_launcher_do_not_offer_an_implicit_downgrade(self):
        app, events = handlers(False)
        with mock.patch.object(linux_update, "game_version", return_value=("v2.2.0", True)):
            app.apply_mode()
            self.assertEqual(app.mode, "up-to-date")
            self.assertEqual(app.build_button.config["text"], "Play")
            self.assertIn("newer than", app.status.get())
            app.launch_folder = "authored-game"
            app.apply_launch()
            self.assertFalse(app.launch_update.visible)
            self.assertIn("newer than", app.launch_status.get())
        self.assertEqual(events, [])

    def test_dlss_rebuild_cannot_bypass_downgrade_confirmation(self):
        app, events = handlers(False)
        app.dlss_toggled = True
        with mock.patch.object(linux_update, "game_version", return_value=("v2.2.0", True)):
            app.apply_mode()
            self.assertEqual(app.mode, "update-game")
            app.build()
        self.assertEqual(events, ["confirm"])
        self.assertEqual(app.update_folder, "prior")

    def test_explicit_track_downgrade_is_confirmed_once(self):
        app, events = handlers(True)
        app.switching = lambda: True
        with mock.patch.object(linux_update, "game_version", return_value=("v2.2.0", True)):
            app.apply_mode()
            self.assertEqual(app.mode, "update-game")
            app.build()
        self.assertEqual(events, ["confirm", "progress", "worker"])


@unittest.skipUnless(sys.platform.startswith("linux"), "actual POSIX folder launcher")
class FolderDispatch(unittest.TestCase):
    def test_recorded_builder_owns_its_game_with_installed_and_game_fallbacks(self):
        with tempfile.TemporaryDirectory(prefix="builder-dispatch-") as folder:
            root = Path(folder)
            game = root / "Authored 'game % folder"
            (game / "bin").mkdir(parents=True)
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder/linux/hookshot-folder", script)
            script.chmod(0o700)
            recorded = root / "Recorded 'Builder.AppImage"
            installed = root / "data/hookshot/builder/versions/v2.0.14/AppRun"
            installed.parent.mkdir(parents=True)
            current = installed.parents[2] / "current"
            current.write_text("v2.0.14\n")
            direct = game / "bin/hookshot"
            for path, label in [(recorded, "recorded"), (installed, "installed"), (direct, "game")]:
                path.write_text("#!/bin/sh\nprintf '%s\\n' " + label + " \"$@\"\n")
                path.chmod(0o700)
            stamp = game / "hookshot_version.txt"
            stamp.write_text("version=v2.1.0-rc.3\nbuilder=" + str(recorded) + "\n")
            env = dict(os.environ, HOOKSHOT_BUILDER_HOME=str(root / "data"))
            def run(expected):
                ran = subprocess.run([str(script)], env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(ran.returncode, 0, ran.stderr)
                self.assertEqual(ran.stdout.splitlines(), [expected, "--launch", str(game)] if expected != "game" else ["game"])
            run("recorded")
            recorded.chmod(0o600)
            run("installed")
            recorded.unlink()
            run("installed")
            current.unlink()
            run("game")
            stamp.unlink()
            run("game")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Evaluate with a local nixpkgs on NIX_PATH and capture initool's literal argv."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ACTIVATION = """
{ fixture, settings, paths }:
let
  pkgs = import <nixpkgs> {};
  lib = pkgs.lib;
  module = import ./nix/hm-module.nix { self.packages.${pkgs.stdenv.hostPlatform.system} = pkgs; };
  evaluated = lib.evalModules {
    specialArgs = {
      lib = lib // { hm.dag.entryAfter = _: script: script; };
      pkgs = pkgs // { initool = { type = "derivation"; outPath = fixture; meta.mainProgram = "initool"; }; };
    };
    modules = [ module {
      options.home.packages = lib.mkOption { type = lib.types.listOf lib.types.package; };
      options.home.activation = lib.mkOption { type = lib.types.attrsOf lib.types.str; };
      config.lightning-matrix-client = {
        enable = true;
        package = pkgs.hello;
        settings = builtins.fromJSON settings;
      } // builtins.fromJSON paths;
    } ];
  };
in evaluated.config.home.activation.lightningMatrixClientMergeSettings
"""


class HomeManagerSettings(unittest.TestCase):
    def test_activation_passes_literal_arguments_and_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp).resolve()
            initool = work / "bin/initool"
            initool.parent.mkdir()
            initool.write_text('#!/usr/bin/env bash\nprintf "%s\\0" "$@" >> "$ARGV_LOG"\ncat -- "$2"\n')
            initool.chmod(0o755)
            text = "a \"Display\" 'Ω' $FONT `printf expanded` \\ $(touch ran)\nnext"
            section, key, value = ("section " + text, "key " + text, "value " + text)
            xdg = work / "XDG config"
            custom = work / "custom $FONT `printf expanded`" / "settings file"
            for paths, expected in [({}, xdg / "MatrixClient/matrix-client.conf"),
                                    ({"settingsFilePath": str(custom)}, custom)]:
                with self.subTest(paths=paths):
                    args = ["nix-instantiate", "--eval", "--strict", "--json",
                            "--option", "allow-import-from-derivation", "false",
                            "--expr", ACTIVATION, "--argstr", "fixture", str(work),
                            "--argstr", "settings", json.dumps({section: {key: value}}),
                            "--argstr", "paths", json.dumps(paths)]
                    script = json.loads(subprocess.check_output(args, cwd=ROOT))
                    log = work / "argv"
                    log.write_bytes(b"")
                    subprocess.run(["bash", "-eu", "-c", 'run() { "$@"; }\n' + script],
                                   cwd=work, check=True,
                                   env={"PATH": os.environ["PATH"], "XDG_CONFIG_HOME": str(xdg),
                                        "ARGV_LOG": str(log), "FONT": "expanded"})
                    self.assertTrue(expected.is_file())
                    self.assertEqual(log.read_bytes().decode().split("\0"),
                                     ["set", str(expected), section, key, value, ""])
                    self.assertFalse((work / "ran").exists())


if __name__ == "__main__":
    unittest.main()

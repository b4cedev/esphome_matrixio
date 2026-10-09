import os
import subprocess
import sys

from .components import ExternalComponent


class CompileTest:
    def __init__(self, component: ExternalComponent):
        self.comp = component

    def run_tests(self) -> int:
        failed = []
        for cfgFile in sorted(os.listdir(self.comp.testsRoot)):
            if not cfgFile.endswith(".yaml"):
                continue
            print("Testing config: ", cfgFile, flush=True)
            cfgPath = os.path.join(self.comp.testsRoot, cfgFile)
            # Compile every config in its own process: ESPHome keeps state between runs in one
            # process (e.g. the registered entity names), so every config after the first one
            # failed with "Duplicate ... entity".
            result = subprocess.run(
                [sys.executable, "-m", "esphome", "-q", "compile", cfgPath]
            )
            if result.returncode != 0:
                failed.append(cfgFile)
        if failed:
            print("Failed configs: ", ", ".join(failed), flush=True)
            return 1
        return 0

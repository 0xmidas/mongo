"""The unittest.TestCase for Server Discovery and Monitoring JSON tests."""
import os
import os.path
from . import interface
from ... import core
from ... import config
from ... import utils
from ...utils import globstar
from ... import errors


class SDAMJsonTestCase(interface.ProcessTestCase):
    """Server Discovery and Monitoring JSON test case."""

    REGISTERED_NAME = "sdam_json_test"
    TEST_DIR = os.path.normpath("src/mongo/client/sdam/json_tests/sdam_tests")

    def __init__(self, logger, json_test_file, program_options=None):
        """Initialize the TestCase with the executable to run."""
        interface.ProcessTestCase.__init__(self, logger, "SDAM Json Test", json_test_file)

        self.program_executable = self._find_executable()
        self.json_test_file = os.path.normpath(json_test_file)
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def _find_executable(self):  # pylint: disable=no-self-use
        binary = os.path.join(config.INSTALL_DIR, "sdam_json_test")
        if os.name == "nt":
            binary += ".exe"

        if not os.path.isfile(binary):
            raise errors.StopExecution(f"Failed to locate sdam_json_test binary at {binary}")
        return binary

    def _make_process(self):
        command_line = [self.program_executable]
        command_line += ["--source-dir", self.TEST_DIR]
        command_line += ["-f", self.json_test_file]
        return core.programs.make_process(self.logger, command_line)

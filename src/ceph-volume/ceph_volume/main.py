import os
import pkg_resources
import sys
import logging

from tambo import Transport
import ceph_volume
from ceph_volume.decorators import catches
from ceph_volume import log


class Volume(object):
    _help = """
ceph-volume: Deploy Ceph OSDs using different device technologies like lvm or
physical disks

Version: %s

Global Options:
--log, --logging    Set the level of logging. Acceptable values:
                    debug, warning, error, critical
--log-path          Change the default location ('/var/lib/ceph') for logging

Log Path: %s

Subcommands:
lvm
%s

%s
    """

    def __init__(self, argv=None, parse=True):
        self.mapper = {}
        self.plugin_help = "No plugins found/loaded"
        if argv is None:
            argv = sys.argv
        if parse:
            self.main(argv)

    def help(self):
        return self._help % (
            ceph_volume.__version__,
            ceph_volume.config.get('log_path'),
            self.plugin_help,
            self.get_environ_vars()
        )

    def get_environ_vars(self):
        environ_vars = []
        for key, value in os.environ.items():
            if key.startswith('CEPH_VOLUME'):
                environ_vars.append("%s=%s" % (key, value))
        if not environ_vars:
            return ''
        else:
            environ_vars.insert(0, 'Environ Variables:')
            return '\n'.join(environ_vars)

    def enable_plugins(self):
        """
        Load all plugins available, add them to the mapper and extend the help
        string with the information from each one
        """
        plugins = _load_library_extensions()
        for plugin in plugins:
            self.mapper[plugin._ceph_volume_name_] = plugin
        self.plugin_help = '\n'.join(['%-19s %s\n' % (
            plugin.name, getattr(plugin, 'help_menu', ''))
            for plugin in plugins])

    @catches((KeyboardInterrupt, RuntimeError))
    def main(self, argv):
        options = [['--log', '--logging']]
        parser = Transport(argv, mapper=self.mapper,
                           options=options, check_help=False,
                           check_version=False)
        parser.parse_args()
        ceph_volume.config['verbosity'] = parser.get('--log', 'info')
        log.setup(ceph_volume.config)
        self.enable_plugins()
        parser.catch_help = self.help()
        parser.catch_version = ceph_volume.__version__
        parser.mapper = self.mapper
        if len(argv) <= 1:
            return parser.print_help()
        parser.dispatch()
        parser.catches_help()
        parser.catches_version()


def _load_library_extensions():
    """
    Locate all setuptools entry points by the name 'ceph_volume_handlers'
    and initialize them.
    Any third-party library may register an entry point by adding the
    following to their setup.py::

        entry_points = {
            'ceph_volume_handlers': [
                'plugin_name = mylib.mymodule:Handler_Class',
            ],
        },

    `plugin_name` will be used to load it as a sub command.
    """
    logger = logging.getLogger('ceph_volume.plugins')
    group = 'ceph_volume_handlers'
    entry_points = pkg_resources.iter_entry_points(group=group)
    plugins = []
    for ep in entry_points:
        try:
            logger.debug('loading %s' % ep.name)
            plugin = ep.load()
            plugin._ceph_volume_name_ = ep.name
            plugins.append(plugin)
        except Exception as error:
            logger.exception("Error initializing plugin %s: %s" % (ep, error))
    return plugins
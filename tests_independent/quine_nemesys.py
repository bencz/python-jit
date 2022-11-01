import __pyjit__
import posix

posix.write(1, __pyjit__.module_source(__name__))

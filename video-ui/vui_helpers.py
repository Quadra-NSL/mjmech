import functools
import logging
import os
import re
import signal
import sys
import time
import traceback

sys.path.append(os.path.join(os.path.dirname(__file__), '../'))
sys.path.append(os.path.join(os.path.dirname(__file__), '../legtool/'))

import trollius as asyncio
import gbulb

# common helpers for vclient.py and vserver.py
g_quit_handlers = list()

class FCMD(object):
    """Fire_cmd constants"""
    off = 0         # Motor disabled
    inpos1 = 1      # Fire 1 shot when inposition
    inpos2 = 2      # Fire 2 shots when inposition
    inpos3 = 3      # Fire 3 shots when inposition
    inpos5 = 5      # Fire 5 shots when inposition
    now1   = 11     # Fire 1 shot immediately
    cont   = 20     # Keep firing for some time

    @classmethod
    def _numshots(cls, x):
        if x in [cls.off, cls.cont]:
            return 0
        elif x in [cls.inpos1, cls.now1]:
            return 1
        elif x == cls.inpos2:
            return 2
        elif x == cls.inpos3:
            return 3
        elif x == cls.inpos5:
            return 5
        assert False, 'Unknown FCMD input: %r' % (x, )

    @classmethod
    def _is_inpos(cls, x):
        return x in [cls.inpos1, cls.inpos2, cls.inpos3, cls.inpos5]

_error_logger = logging.getLogger('fatal-error')

def wrap_event(callback):
    """Wrap event callback so the app exit if it crashes"""
    def wrapped(*args, **kwargs):
        try:
            return callback(*args, **kwargs)
        except BaseException as e:
            _error_logger.error("Callback %r crashed:", callback)
            _error_logger.error(" %s %s" % (e.__class__.__name__, e))
            for line in traceback.format_exc().split('\n'):
                _error_logger.error('| %s', line)
            for cb in g_quit_handlers:
                cb()
            raise
    return wrapped

@wrap_event
def _sigint_handler():
    # wrap_event decorator will make sure the exception stops event loop.
    raise Exception('Got SIGINT')

@wrap_event
def _sigterm_handler():
    # wrap_event decorator will make sure the exception stops event loop.
    raise Exception('Got SIGTERM')

def CriticalTask(coro, exit_ok=False):
    """Just like asyncio.Task, but if it ever completes, the program
    exits.  (unless @p exit_ok is True, in which case non-exception
    exit is ok)
    """
    task = asyncio.Task(coro)
    task.add_done_callback(
        functools.partial(_critical_task_done, exit_ok=exit_ok))
    return task

@wrap_event
def _critical_task_done(task, exit_ok=False):
    if exit_ok and task.done() and (task.exception() is None):
        # No errors
        return

    # Reach inside task's privates to get exception traceback
    # (if this fails for some reason, wrap_event will terminate us anyway)
    logger = logging.getLogger('fatal-error')
    if task._tb_logger:
        tb_text = task._tb_logger.tb
        if task._tb_logger.source_traceback:
            # Do not care
            tb_text.append('Task creation information not shown')
    else:
        tb_text = ['No traceback info']
    _error_logger.error("Critical task (%r) exited:", task._coro)
    e = task.exception()
    if e is None:
        _error_logger.error('Successful (but unexpected) exit')
    else:
        _error_logger.error(" %s %s" % (e.__class__.__name__, e))
    for line in ''.join(tb_text).split('\n'):
        _error_logger.error('| %s', line.rstrip())

    # Terminate program
    for cb in g_quit_handlers:
        cb()

_INVALID_CHARACTER_RE = re.compile('[^\x20-\x7E]')
def sanitize_stdout(line):
    """Strip newline from end of line, call repr if any nonprintables
    left ater that.
    """
    if line.endswith('\n'):
        line = line[:-1]
    if _INVALID_CHARACTER_RE.search(line):
        return repr(line)
    return line


@asyncio.coroutine
def dump_lines_from_fd(fd, print_func):
    """Given a file descriptor (integer), asyncronously read lines from it.
    Sanitize each line and pass as a sole argument to @p print_func.
    """
    fdobj = os.fdopen(fd, 'r')
    loop = asyncio.get_event_loop()
    reader = asyncio.streams.StreamReader(loop=loop)
    transport, _ = yield asyncio.From(loop.connect_read_pipe(
            lambda: asyncio.streams.StreamReaderProtocol(reader),
            fdobj))

    while True:
        line = yield asyncio.From(reader.readline())
        if line == '': # EOF
            break
        print_func(sanitize_stdout(line))
    transport.close()

def asyncio_misc_init():
    asyncio.set_event_loop_policy(gbulb.GLibEventLoopPolicy())

    main_loop = asyncio.get_event_loop()
    main_loop.add_signal_handler(signal.SIGINT, _sigint_handler)
    main_loop.add_signal_handler(signal.SIGTERM, _sigterm_handler)
    g_quit_handlers.append(
        lambda:  main_loop.call_soon_threadsafe(main_loop.stop))

def add_pair(a, b, scale=1.0):
    return (a[0] + b[0] * scale,
            a[1] + b[1] * scale)

def logging_init(verbose=True):
    root = logging.getLogger()
    root.setLevel(logging.DEBUG)

    # Code below is like basicConfig, but we do not apply limits on loggers;
    # instead we apply them on handlers.
    outhandler = logging.StreamHandler()
    outhandler.setFormatter(
        logging.Formatter(
            fmt=("%(asctime)s.%(msecs).3d [%(levelname).1s]"
                    " %(name)s: %(message)s"),
            datefmt="%T"))
    root.addHandler(outhandler)
    if not verbose:
        outhandler.setLevel(logging.INFO)

class MemoryLoggingHandler(logging.Handler):
    """Handler that just appends data to python array.
    The elements are tuples:
       (time, level, logger_name, message)
    """
    SHORT_LEVEL_NAMES = {
        logging.CRITICAL: 'C',
        logging.ERROR: 'E',
        logging.WARNING: 'W',
        logging.INFO: 'I',
        logging.DEBUG: 'D',
        }

    def __init__(self, install=False, max_records=10000):
        logging.Handler.__init__(self)
        self.data = list()
        self.max_records = max_records
        self.on_record = list()
        self.last_time = 0
        if install:
            logging.getLogger().addHandler(self)

    def emit(self, record):
        """Part of logging.Handler interface"""
        ts = record.created
        if ts <= self.last_time:
            # timestamp must always increase
            ts = self.last_time + 1.0e-6
        self.last_time = ts
        self.data.append(
            (ts,
             record.levelno,
             record.name,
             record.getMessage()))
        while len(self.data) > self.max_records:
            self.data.pop(0)
        for cb in self.on_record:
            cb()

    @staticmethod
    def to_dict(mtuple, time_field='time'):
        """Given a 4-tuple, convert it to dict"""
        return {
            time_field: mtuple[0],
            'levelno': mtuple[1],
            'name': mtuple[2],
            'message': mtuple[3]}

    @classmethod
    def to_string(cls, mtuple):
        """Given a 4-tuple, convert it to string (default formatted)
        """
        return "%s [%s] %s: %s" % (
            time.strftime("%T", time.localtime(mtuple[0])),
            cls.SHORT_LEVEL_NAMES.get(mtuple[1], mtuple[1]),
            mtuple[2], mtuple[3])

    @staticmethod
    def relog(mtuple, delta_t=0, prefix=''):
        """Given a 4-tuple, re-log it to local logger"""
        # NOTE: this igores whole logger hierarchy. If we ever use it, pass a
        # name here.
        root = logging.getLogger()
        assert len(mtuple) == 4
        rec = root.makeRecord(
            prefix + mtuple[2], mtuple[1], 'remote-file', -1, mtuple[3],
            [], None, 'remote-func', None)
        # Override time. There is no better way.
        ct = delta_t + mtuple[0]
        rec.created = ct
        rec.msecs = (ct - long(ct)) * 1000
        rec.relativeCreated = (rec.created - logging._startTime) * 1000
        # Dispatch.
        root.handle(rec)

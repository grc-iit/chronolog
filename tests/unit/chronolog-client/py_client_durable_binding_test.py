# Checks the Python binding of Durability, LogEventOptions and the
# status-returning StoryHandle.log_event overload. Needs only the built
# py_chronolog_client module on PYTHONPATH, no daemons.
import re
import sys

import py_chronolog_client as m

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


check(int(m.Durability.Accepted) == 1, "Durability.Accepted is not 1")
check(int(m.Durability.Durable) == 2, "Durability.Durable is not 2")

options = m.LogEventOptions()
check(options.durability == m.Durability.Accepted, "default durability is not Accepted")
check(options.timeout_ms == 10000, "default timeout_ms is not 10000")
check(options.attempts == 3, "default attempts is not 3")

options.durability = m.Durability.Durable
options.timeout_ms = 250
options.attempts = 5
check(options.durability == m.Durability.Durable, "durability is not writable")
check(options.timeout_ms == 250, "timeout_ms is not writable")
check(options.attempts == 5, "attempts is not writable")

# pybind numbers each overload of an overloaded method in its docstring.
doc = m.StoryHandle.log_event.__doc__ or ""
overloads = re.findall(r"^\d+\. log_event\(", doc, re.MULTILINE)
check(len(overloads) == 2, "StoryHandle.log_event lists %d overloads, want 2:\n%s" % (len(overloads), doc))
check("LogEventOptions" in doc, "no log_event overload takes LogEventOptions:\n%s" % doc)


# Calling the bound C++ overload on a Python subclass goes through the
# PyStoryHandle trampoline to the subclass's log_event(record, options).
class FakeHandle(m.StoryHandle):
    def log_event(self, record, options):
        return (int(options.durability), 42)


fake = FakeHandle()
durable = m.LogEventOptions()
durable.durability = m.Durability.Durable
check(m.StoryHandle.log_event(fake, "x", durable) == (2, 42),
      "trampoline did not reach the Python override with options")


# A subclass without a log_event override: the default C++ body of
# log_event(record, options) reaches the pure log_event(record), which has
# no override either. That must raise a Python exception, not crash.
class NoOverride(m.StoryHandle):
    pass


try:
    m.StoryHandle.log_event(NoOverride(), "x", m.LogEventOptions())
    check(False, "log_event(record, options) without an override did not raise")
except RuntimeError:
    pass

for failure in failures:
    print("FAIL: " + failure)
sys.exit(1 if failures else 0)

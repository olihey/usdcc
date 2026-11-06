"""The pipes between usdcc and the RTX helper process (docs/RTX_RENDER.md section 4).

Commands go to the helper's stdin, one JSON object per line: {"cmd": name, ...}.

Events come back on its stdout, each a JSON header line {"ev": name, ...}, followed by
"size" bytes of payload when the header has "size" (an image's RGBA8888 pixels).
Standard library only: both usdcc's Python and the helper's import this.
"""

import json


def command(name, **fields):
    """The bytes for one command."""
    return (json.dumps(dict(fields, cmd=name)) + "\n").encode("utf-8")


def parse_command(line):
    """(name, fields) of a command line."""
    fields = json.loads(line)
    return fields.pop("cmd"), fields


def event(name, payload=None, **fields):
    """The bytes for one event, with an optional binary payload."""
    if payload is not None:
        fields["size"] = len(payload)
    header = (json.dumps(dict(fields, ev=name)) + "\n").encode("utf-8")
    return header + bytes(payload) if payload is not None else header


class EventReader:
    """Turns what arrives on the helper's stdout, in reads of any size, into
    (header, payload) pairs; payload is None for an event without one."""

    def __init__(self):
        self._buffer = bytearray()
        self._header = None  # read, waiting for its payload

    def feed(self, data):
        self._buffer += data
        events = []
        while True:
            if self._header is None:
                end = self._buffer.find(b"\n")
                if end < 0:
                    break
                line = bytes(self._buffer[:end])
                del self._buffer[: end + 1]
                if not line.strip():
                    continue
                header = json.loads(line)
                if "size" not in header:
                    events.append((header, None))
                    continue
                self._header = header
            size = self._header["size"]
            if len(self._buffer) < size:
                break
            payload = bytes(self._buffer[:size])
            del self._buffer[:size]
            events.append((self._header, payload))
            self._header = None
        return events

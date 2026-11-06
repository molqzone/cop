"""`cop` against synthetic pages: every verb is tested without a board.

The tests build the exact bytes a core leaves behind -- header, telemetry ring, name
service, command region -- and then ask the CLI what it sees.  That is the only way to
test the two behaviours that matter and that no board will tell you politely: a page
whose fields are not where the tool assumes, and a tool that offers to "fix" a page it
does not own.
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
COP = REPO / "cop"

# The values below are stated independently of the tool on purpose: a test that reads
# the tool's own constants back proves nothing.
PAGE = 4096
MAGIC = 0x31506461
ABI = 1
SLOTS = 58
SLOT_BYTES = 64
PAYLOAD = 24
REGION = 4064
TAG = 6
TABLE = 3840
TABLE_MAGIC = 0x3130534E
ENTRY = 42
NAME = 28

CONTRACT = {
    "schema": 1,
    "page": {"bytes": PAGE, "magic": MAGIC, "header_bytes": 32, "abi_version": ABI,
             "wire_tag": TAG},
    "telemetry": {"offset": 64, "slot_count": SLOTS, "slot_bytes": SLOT_BYTES,
                  "head_offset": 3776, "write_state_offset": 3780},
    "frame": {"header_bytes": 16, "payload_bytes": 48},
    "channels": {"offset": TABLE, "bytes": 184, "magic": TABLE_MAGIC,
                 "entry_bytes": ENTRY, "name_bytes": NAME, "capacity": 4,
                 "entries": [{"name": "imu", "endpoint": 1024, "flags": 1}]},
    "region": {"offset": REGION, "payload_bytes": PAYLOAD,
               "write_state_offset": 4088, "seq_offset": 4092},
    "command": {"magic": 0x31444D47, "ping": 1, "rejected": 0xFFFFFFFF},
    "console": {"bytes": PAGE, "index_offset": 0, "ring_offset": 64, "ring_bytes": 4032},
}


def load_cop():
    loader = importlib.machinery.SourceFileLoader("cop", str(COP))
    spec = importlib.util.spec_from_loader("cop", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


cop = load_cop()


class BytesPage:
    """The read/u32 slice of cop.Page over an in-memory page."""

    def __init__(self, blob: bytes):
        self.blob = blob

    def read(self, offset: int, length: int) -> bytes:
        return self.blob[offset:offset + length]

    def u32(self, offset: int) -> int:
        return struct.unpack("<I", self.read(offset, 4))[0]


def formatted_page(channels=(("imu", 1024, 1),), head: int = 42) -> bytes:
    """Exactly what a core that builds the page leaves behind."""
    page = bytearray(PAGE)
    struct.pack_into("<8I", page, 0, MAGIC, ABI, PAGE, SLOTS, SLOT_BYTES, PAYLOAD,
                     REGION, TAG)
    struct.pack_into("<I", page, 3776, head)
    struct.pack_into("<I", page, 3780, 0)
    struct.pack_into("<4I", page, TABLE, TABLE_MAGIC, len(channels), 3, 0)
    for index, (name, endpoint, flags) in enumerate(channels):
        raw = name.encode()
        struct.pack_into("<IIIH%ds" % NAME, page, TABLE + 16 + index * ENTRY,
                         0x1234 + index, endpoint, flags, len(raw), raw)
    struct.pack_into("<I", page, REGION, CONTRACT["command"]["magic"])
    struct.pack_into("<I", page, REGION + 4, 1)
    struct.pack_into("<I", page, REGION + 8, 7)
    return bytes(page)


class HeaderTests(unittest.TestCase):
    def test_a_cold_page_is_unformatted(self) -> None:
        header = cop.parse_header(BytesPage(bytes(PAGE)))
        self.assertEqual(cop.state_name(header), "UNFORMATTED")

    def test_a_foreign_page_is_reported_as_foreign(self) -> None:
        page = bytearray(PAGE)
        struct.pack_into("<I", page, 0, 0xDEADBEEF)
        self.assertEqual(cop.state_name(cop.parse_header(BytesPage(bytes(page)))),
                         "FOREIGN")

    def test_a_formatted_page_carries_its_geometry(self) -> None:
        header = cop.parse_header(BytesPage(formatted_page()))
        self.assertEqual(cop.state_name(header), "FORMATTED")
        self.assertEqual((header["abi"], header["tag"], header["slot_count"]),
                         (ABI, TAG, SLOTS))

    def test_the_derived_offsets_match_the_layout(self) -> None:
        geometry = cop.geometry(cop.parse_header(BytesPage(formatted_page())))
        self.assertEqual(geometry, {"telemetry": 64, "head": 3776,
                                    "write_state": 3780, "channels": 3840})


class ChannelTests(unittest.TestCase):
    def test_the_name_service_is_decoded(self) -> None:
        page = BytesPage(formatted_page((("imu", 1024, 1), ("gyro", 1025, 2))))
        offset, count, generation, entries = cop.channels(page, cop.parse_header(page))
        self.assertEqual((offset, count, generation), (3840, 2, 3))
        self.assertEqual([entry[0] for entry in entries], ["imu", "gyro"])
        self.assertEqual(entries[1][1], 1025)

    def test_a_page_without_a_table_reports_none(self) -> None:
        page = BytesPage(formatted_page(channels=()))
        _, count, _, entries = cop.channels(page, cop.parse_header(page))
        self.assertEqual((count, entries), (0, []))


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / "page.bin"
        self.contract = Path(self.directory.name) / "contract.json"
        self.contract.write_text(json.dumps(CONTRACT))

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write(self, blob: bytes) -> None:
        self.path.write_bytes(blob)

    def run_cop(self, *args: str, timeout: float = 20) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(COP), "--page", str(self.path), *args],
            capture_output=True, text=True, check=False, timeout=timeout,
        )


class CliTests(Fixture):
    def test_status_reports_the_page_it_sees(self) -> None:
        self.write(formatted_page())
        done = self.run_cop("status")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("FORMATTED", done.stdout)
        self.assertIn("slots=58", done.stdout)
        self.assertIn("channels  : 1 announced", done.stdout)
        self.assertIn("imu", done.stdout)

    def test_status_on_a_cold_page_says_so_and_does_not_guess(self) -> None:
        self.write(bytes(PAGE))
        done = self.run_cop("status")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("UNFORMATTED", done.stdout)

    def test_dump_shows_bytes_at_an_offset(self) -> None:
        self.write(formatted_page())
        done = self.run_cop("dump", "--offset", "0", "--bytes", "16")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("61 64 50 31", done.stdout)  # little-endian page magic

    def test_send_needs_a_contract(self) -> None:
        self.write(formatted_page())
        done = self.run_cop("send", "--type", "1", "--payload", "")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("--contract", done.stderr)

    def test_send_refuses_a_page_without_an_owner(self) -> None:
        self.write(bytes(PAGE))
        done = self.run_cop("send", "--contract", str(self.contract), "--type", "1",
                            "--payload", "")
        self.assertEqual(done.returncode, 1)
        self.assertIn("UNFORMATTED", done.stderr)

    def test_send_writes_the_slot_before_ringing(self) -> None:
        # The mailbox is absent in a test process, so the failure has to be the doorbell
        # -- which proves the command slot was written first.
        self.write(formatted_page())
        done = self.run_cop("send", "--contract", str(self.contract), "--type", "1",
                            "--payload", "")
        self.assertEqual(done.returncode, 1)
        self.assertIn("cvi-rtos-cmdqu", done.stderr)

    def test_watch_refuses_a_page_without_an_owner(self) -> None:
        self.write(bytes(PAGE))
        done = self.run_cop("watch", timeout=5)
        self.assertEqual(done.returncode, 1)
        self.assertIn("UNFORMATTED", done.stderr)

    def test_there_is_no_format_verb(self) -> None:
        self.write(bytes(PAGE))
        done = self.run_cop("format")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("invalid choice", done.stderr)


class ContractTests(unittest.TestCase):
    """A contract must describe the same page the tool derives from the header."""

    def test_the_page_constants_agree(self) -> None:
        channels = CONTRACT["channels"]
        self.assertEqual(cop.PAGE_MAGIC, CONTRACT["page"]["magic"])
        self.assertEqual(cop.HEADER_BYTES, CONTRACT["page"]["header_bytes"])
        self.assertEqual(cop.CHANNEL_TABLE_MAGIC, channels["magic"])
        self.assertEqual(cop.CHANNEL_ENTRY_BYTES, channels["entry_bytes"])
        self.assertEqual(cop.CHANNEL_NAME_BYTES, channels["name_bytes"])

    def test_the_declared_offsets_are_what_the_tool_derives(self) -> None:
        telemetry = CONTRACT["telemetry"]
        header = {"magic": CONTRACT["page"]["magic"],
                  "abi": CONTRACT["page"]["abi_version"],
                  "page_size": CONTRACT["page"]["bytes"],
                  "slot_count": telemetry["slot_count"],
                  "slot_bytes": telemetry["slot_bytes"],
                  "payload_bytes": CONTRACT["frame"]["payload_bytes"],
                  "region_offset": CONTRACT["region"]["offset"],
                  "tag": CONTRACT["page"]["wire_tag"]}
        geometry = cop.geometry(header)
        self.assertEqual(geometry["telemetry"], telemetry["offset"])
        self.assertEqual(geometry["head"], telemetry["head_offset"])
        self.assertEqual(geometry["write_state"], telemetry["write_state_offset"])
        self.assertEqual(geometry["channels"], CONTRACT["channels"]["offset"])


if __name__ == "__main__":
    unittest.main()

# cop

**Linux-side CLI for a coprocessor shared page: uio discovery, self-describing header,
named channels, mailbox doorbell. It defines no format — the core that writes the page
does.**

C++20, no product header, no contract file. `cop` reads the page's own header at runtime
and derives every offset from it. It links only [`shared-page-xr`](https://github.com/molqzone/shared-page-xr)
and [`libxr`](https://github.com/jiu-xiao/libxr); it never includes a product's
`GuidanceContract` (or any equivalent) and never parses a JSON schema.

```sh
cop list                              # the pages the device tree exposes, with address and size
cop status                            # state, layout, channels, head/seq
cop dump --offset 3776 --bytes 8      # forensics
cop log [--new]                       # stream the console ring
cop watch                             # print new telemetry slots as opaque bytes
cop send --type N [--payload HEX]     # one generic command through the mailbox
cop wait-console [--timeout 30]       # wait for the console descriptor
cop wipe-shared --confirm             # zero the shared page (boot path only)
```

## What it is not

- **It does not define the page format.** The header is written by the core that owns
  the page; `cop` reads it. There is no schema here, no ELF section, and no
  `pages.json` beside it.
- **It does not format the page.** There is deliberately no `format` verb. A page
  nobody built is a fact to report (`UNFORMATTED` / `FOREIGN`), not something to
  overwrite from the other side.
- **It does not know what commands mean.** `send --type N` is the transport escape
  hatch; the product's own tooling names the types.
- **It does not carry product data.** Channel names come from the page's name-service
  table; frame layout comes from the header. The payload of `send` is opaque bytes.

## What a page looks like

Two 4 KiB pages are typical (`console` and `shared`); the tool finds them by
`reg-names` in `/sys/class/uio/*/maps/mapN/name`, never by a hardcoded map index or
address.

The shared page starts with the self-describing header, little endian (36 bytes):

| offset | field | meaning |
|---|---|---|
| 0 | `magic` | page magic; `0` means nobody formatted it |
| 4 | `page_size` | bytes |
| 8 | `slot_count` | telemetry slots |
| 12 | `sample_size` | bytes per slot |
| 16 | `payload_size` | command-region payload bytes |
| 20 | `region_offset` | where the command region starts |
| 24 | `tag` | the two sides must agree on this |
| 28 | `command_magic` | marks the command slot; `0` means the page has none |
| 32 | `header_bytes` | `sizeof(PageHeader)`, for readers that grow the header |

Everything else is derived: the telemetry ring starts at the first cache line after the
header, `head` and `write_state` follow the ring, and the name-service table starts at
the next cache line after those. The command region is at `region_offset` because the
header says so. `cop status` reports `MISMATCH` when the header's own numbers do not
add up, instead of reading past the page.

The command slot's first 8 bytes are a generic header (`magic`, `type`, `seq`); the rest
is the product payload. `cop send` writes that header plus the hex payload; the firmware
decides whether the type is meaningful.

The console page has its own `ConsoleDescriptor` at offset 4: magic `CLOG`, version,
`index_offset`, `ring_offset`, `ring_bytes`, `page_bytes`. `cop log` follows it; a page
whose descriptor is not published is reported, not guessed.

## The doorbell

The shared page is memory; the *notification* is the vendor mailbox. `send` writes the
command region, then issues one blocking `RTOS_CMDQU_SEND_WAIT` ioctl on
`/dev/cvi-rtos-cmdqu` and checks that the reply equals the sequence number it sent. If
the mailbox interrupt belongs to another driver (it usually does), the uio maps carry no
interrupt — which is why `log` and `watch` poll.

## Building

The two dependencies are ordinary checkouts; the image's vendor build points them at
the exact headers the firmware was compiled with, so the tool and the firmware cannot
disagree about the transport:

```sh
git clone https://github.com/jiu-xiao/libxr third_party/libxr
git clone https://github.com/molqzone/shared-page-xr third_party/shared-page-xr

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Cross build for the board:

```sh
cmake -S . -B build-rv \
  -DCMAKE_CXX_COMPILER=riscv64-linux-gnu-g++ \
  -DCOP_STATIC=ON -DCOP_BUILD_TESTS=OFF
cmake --build build-rv -j
qemu-riscv64-static build-rv/cop --help
```

`COP_LIBXR_DIR` / `COP_SHARED_PAGE_DIR` override the default `third_party/` locations.

## Testing without a board

Every verb except `list` runs against a page file:

```sh
cop --page /tmp/page.bin status
cop --page /tmp/page.bin dump --offset 0 --bytes 16
```

`ctest` builds a fixture page with `SharedPage` and runs the CLI against it, so the
runtime layout and the compile-time layout are checked against the same bytes.

## Provenance

This repository is the source. Board images vendor a pinned static binary and record
its commit and sha256 next to it, so a shipped tool can always be traced back to a
commit here. The binary is built with the image's `libxr`/`shared-page-xr` checkouts;
no header is copied into the image.

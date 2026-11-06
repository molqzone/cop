# cop

**Linux-side CLI for a coprocessor shared page: uio discovery, self-describing header,
named channels, mailbox doorbell. It defines no format — the core that writes the page
does.**

One file, Python 3 standard library only. No build step, no daemon, no configuration of
its own.

```sh
cop list                        # the pages the device tree exposes, with address and size
cop status [--contract FILE]    # decode the page's own header: state, layout, channels
cop dump --offset 3776 --bytes 8
cop log [--contract FILE] [--new]   # stream the console ring
cop watch [--contract FILE]     # print new telemetry slots as opaque bytes
cop send --contract FILE --type N --payload HEX   # typed command through the mailbox
```

## What it is not

These are the boundaries, and they are the reason it can be a single file:

- **It does not define the page format.** The header is written by the core that owns
  the page; `cop` reads it. There is no schema here, and no resource table: no ELF
  section, no firmware-side table for the kernel to parse, no `pages.json` beside it.
- **It does not format the page.** There is deliberately no `format` verb. A page
  nobody built is a fact to report (`UNFORMATTED` / `FOREIGN`), not something to
  overwrite from the other side.
- **It does not know what commands mean.** `send --type N` is the transport escape
  hatch; the product's own tooling names the types.
- **It does not carry product data.** Channel names, frame layout and command values
  come from an optional contract JSON that the product publishes; the tool only needs it
  for `send` and for the console ring geometry.

## What a page looks like

Two 4 KiB pages are typical (`console` and `shared`); the tool finds them by
`reg-names` in `/sys/class/uio/*/maps/mapN/name`, never by a hardcoded map index or
address.

The shared page's first 32 bytes are the self-describing header, little endian:

| offset | field | meaning |
|---|---|---|
| 0 | `magic` | page magic; `0` means nobody formatted it, anything else unexpected means it is not this page |
| 4 | `abi` | layout version |
| 8 | `page_size` | bytes |
| 12 | `slot_count` | telemetry slots |
| 16 | `slot_bytes` | bytes per slot |
| 20 | `payload_bytes` | command payload size |
| 24 | `region_offset` | where the command region starts |
| 28 | `wire_tag` | the two sides must agree on this |

Everything else is derived: the telemetry ring starts at the next cache line, `head` and
`write_state` follow the ring, and the name-service table starts at the next cache line
after those. The command region is at `region_offset` because the header says so.

The name-service table is one generation word plus fixed-size entries (key, endpoint,
flags, name). `cop status` prints what is actually announced, so "the core is up but
publishes nothing" is visible instead of inferred.

## The doorbell

The shared page is memory; the *notification* is the vendor mailbox. `send` writes the
command region, then issues one blocking `RTOS_CMDQU_SEND_WAIT` ioctl on
`/dev/cvi-rtos-cmdqu` and checks that the reply equals the sequence number it sent. If
the mailbox interrupt belongs to another driver (it usually does), the uio maps carry no
interrupt — which is why `log` and `watch` poll.

## Testing without a board

Every verb except `list` runs against a page file:

```sh
cop --page /tmp/page.bin status
cop --page /tmp/page.bin dump --offset 0 --bytes 16
```

That is also how the tests build the exact bytes a core leaves behind and ask the CLI
what it sees:

```sh
python3 -m unittest discover -s tests
```

## Provenance

This repository is the source. Board images vendor a pinned copy of `cop` and record its
commit and sha256 next to it, so a shipped tool can always be traced back to a commit
here.

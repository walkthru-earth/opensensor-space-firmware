# Decision records

Short records of design choices that span firmware, the uploader, the mobile
app or the STAC spec. Each one names the options, the choice, who owns which
part and what would reopen it. Code and contracts are the truth. A record says
why they look the way they do.

A record is Proposed until the owner signs it off on its issue, then Accepted.
A later record can supersede an earlier one, and the old one stays as history.

| Record | Issue | Status |
| --- | --- | --- |
| [0001 Who writes STAC for a standalone Wi-Fi Thing](0001-standalone-thing-stac.md) | [#109](https://github.com/walkthru-earth/opensensor-space-firmware/issues/109) | Accepted 2026-11-04, the board writes STAC (OD-2026-11-04-D) |
| [0002 Small Parquet files, compaction and range reads](0002-small-parquet-files.md) | [#25](https://github.com/walkthru-earth/opensensor-space-firmware/issues/25) | Proposed, pending owner sign off |

## Writing a record

- Name it `NNNN-short-slug.md` with the next free number.
- Open with a small table of status, date, issue and the sources it rests on.
- Cite code and docs by repo path and spec text by full commit. Use numbers
  only from the repo, its bench records or the issue threads.
- Add a row to the table above and link the record from the pages it affects.
- Decisions that touch the mobile app or stac-opensensor go in the durable
  ecosystem register,
  [stac-opensensor `docs/decisions.md`](https://github.com/walkthru-earth/stac-opensensor/blob/9943282984e3bb8bb37cc2188cabf8e7de5d52c2/docs/decisions.md).

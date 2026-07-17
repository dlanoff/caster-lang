# Third-Party Notices

Vendored third-party dependencies and their subdependencies are governed by their respective licenses and copyright notices.

## cJSON

- Path: `vendor/cjson`
- Upstream: https://github.com/DaveGamble/cJSON
- Pinned version: 1.7.19
- License: MIT
- Original license: `vendor/cjson/LICENSE`
- Modification status: Vendored source snapshot. No Caster source patches.

## H2O

- Path: `vendor/h2o`
- Upstream: https://github.com/h2o/h2o
- Pinned version: H2O 2.3.0-DEV, libh2o 0.16.0
- Pinned commit: edd7a120bfc4af11ac0cbebce2a43cc1f93f9af1
- License: MIT
- Original license: `vendor/h2o/LICENSE`
- Modification status: Vendored source snapshot. No Caster source patches.

H2O includes bundled third-party dependencies under `vendor/h2o/deps`.
Their license and attribution files are preserved in place, including:

- `vendor/h2o/deps/hiredis/COPYING`
- `vendor/h2o/deps/libyrmcds/COPYING`
- `vendor/h2o/deps/mruby-input-stream/LICENSE`
- `vendor/h2o/deps/mruby/LICENSE`
- `vendor/h2o/deps/neverbleed/LICENSE`
- `vendor/h2o/deps/picotls/deps/cifra/COPYING`
- `vendor/h2o/deps/picotls/deps/micro-ecc/LICENSE.txt`
- `vendor/h2o/deps/quicly/LICENSE`
- `vendor/h2o/deps/ssl-conservatory/LICENSE`
- `vendor/h2o/deps/yaml/License`

## hashmap.c

- Path: `vendor/hashmap.c`
- Upstream: https://github.com/tidwall/hashmap.c
- Pinned commit: 3735986914fc1c4e6a9123b406d0970666aa4aec
- License: MIT
- Original license: `vendor/hashmap.c/LICENSE`
- Modification status: Vendored source snapshot. No Caster source patches.

## SQLite

- Path: `vendor/sqlite`
- Upstream: https://sqlite.org/
- Pinned version: 3.53.3 amalgamation
- License: Public domain dedication with SQLite blessing text
- Original notice: Preserved in `vendor/sqlite/sqlite3.c` and `vendor/sqlite/sqlite3.h` source headers
- Modification status: Vendored amalgamation snapshot. No Caster source patches.

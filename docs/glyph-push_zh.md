# Dynamic Text Glyph Push Extension

This file is the English translation of the former Chinese `docs/glyph-push_zh.md`.

**Complete specification:** [glyph-push.md](./glyph-push.md)

The device may advertise `features.glyph_push: true` and a `text_font` object (`bundle`, `charset`, `size`, `bpp`) in the hello message. The server may then attach bitmap glyphs to `stt` / `tts` JSON so characters missing from the on-device font still render. This does not change audio or STT text semantics.

If `glyph_push` is missing or false, do not send glyph payloads. Full field layout and versioning: [glyph-push.md](./glyph-push.md).

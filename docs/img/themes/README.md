# Theme previews

Host renders of `main/ui.c`'s status screen in each theme, produced by
`tools/preview_ui.py` from the real font tables, the real layout offsets, and
the real `main/theme.h` colour table, including the per-section hues, resolved per 8-row band exactly as `display_st7789.c` resolves them — quantised to RGB565, so these show what
the panel can produce rather than what each source scheme asked for.

**These are renders, not photographs (R-005).** They are proof that the layout
fits and that the colours are what the table says; they are not evidence about
the panel, its backlight, or its viewing angle. Nothing here has been compared
against hardware.

Regenerate after any change to the fonts, the layout, or the theme table:

    python3 tools/preview_ui.py --png docs/img/themes/ui

The 0.42" OLED gets one image and no themes: that panel is 1bpp, so every ink
role renders as a lit pixel and a theme is a genuine no-op there.

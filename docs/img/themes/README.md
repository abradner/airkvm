# Theme previews

Host renders of `firmware/main/ui.c`'s status screen in each theme, produced by
`firmware/tools/preview_ui.py` from the real font tables, the real layout offsets, and
the real `firmware/main/theme.h` colour table, including the per-section hues, resolved per 8-row band exactly as `display_st7789.c` resolves them — quantised to RGB565, so these show what
the panel can produce rather than what each source scheme asked for.

**These are renders, not photographs (R-005).** They are proof that the layout
fits and that the colours are what the table says; they are not evidence about
the panel, its backlight, or its viewing angle. Nothing here has been compared
against hardware.

Regenerate after any change to the fonts, the layout, or the theme table:

    python3 firmware/tools/preview_ui.py --png docs/img/themes/ui

Run it from the repository root: the script finds the firmware sources from
its own location, but `--png` is resolved against the working directory, so
running it from `firmware/` silently writes the PNGs into `firmware/docs/`
instead of updating the ones committed here.

The 0.42" OLED gets no themes: that panel is 1bpp, so every ink role renders as
a lit pixel and a theme is a genuine no-op there. It still gets one image per
link state, because the signal gauge's SHAPE differs between them even where
its colour cannot.

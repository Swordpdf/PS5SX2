#pragma once
#include <stddef.h>
#include <stdint.h>

/* the pictures a tile can be made from: any may be NULL (or a file that isn't there) */
typedef struct
{
	const char* cover; /* the PS2 case's front */
	const char* hero;  /* SteamGridDB hero art (a wide banner) */
	const char* logo;  /* SteamGridDB logo (transparent PNG) */
	const char* grid;  /* SteamGridDB square grid */
	const char* shot;  /* a screenshot for the background (libretro's title screen or in-game snap, no key needed) */
	const char* title; /* the title screen, for the icon */
	const char* font;  /* a TTF/OTF for the label */
	const char* label; /* "PlayStation 2" under the logo */
} TileArt;

/* 0 on success: icon_out gets a 512x512 PNG, pic_out a 3840x2160 BC7 DDS (either may be NULL); used says what was used */
int tile_make_art(const TileArt* a, const char* icon_out, const char* pic_out, char* used, size_t used_size);
int tile_make(const char* cover_path, const char* icon_out, const char* pic_out);
/* The "Reload PS2 disc" tile's pictures, drawn (no source picture): icon_out a 512x512 PNG, pic_out a 3840x2160 BC7 DDS (either may be NULL);
 * label (e.g. "RELOAD PS2 DISC") is written under the sign with the font file when both are given. 0 on success. */
int tile_make_reload_art(const char* icon_out, const char* pic_out, const char* font, const char* label);
/* A title card drawn from the game's name alone (a silver disc and the name under it, up to three lines of ASCII): for a disc whose cover could not be
 * fetched. icon_out a 512x512 PNG, pic_out a 3840x2160 BC7 DDS (either may be NULL); without a font only the disc is drawn. 0 on success. */
int tile_make_title_art(const char* icon_out, const char* pic_out, const char* font, const char* title);
/* 1: an icon made from a cover only is the whole case over a blurred backdrop; 0 (the default): the middle of the cover fills the square */
void tile_set_icon_case(int on);
int tile_write_png(const char* path, const uint8_t* rgb, int w, int h);
int tile_write_bc7_dds(const char* path, const uint8_t* rgb, int w, int h);

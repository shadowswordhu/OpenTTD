#include "stdafx.h"
#include "ttd.h"
#include "window.h"
#include "gui.h"
#include "gfx.h"
#include "sound.h"
#include "hal.h"

#define NUM_SONGS_AVAILABLE 22


static byte _playlist_all[] = {
	1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,0,
};

static byte _playlist_old_style[] = {
	1, 8, 2, 9, 14, 15, 19, 13, 0,
};

static byte _playlist_new_style[] = {
	6, 11, 10, 17, 21, 18, 5, 0
};

static byte _playlist_ezy_street[] = {
	12, 7, 16, 3, 20, 4, 0
};

static byte * const _playlists[] = {
	_playlist_all,
	_playlist_old_style,
	_playlist_new_style,
	_playlist_ezy_street,
	msf.custom_1,
	msf.custom_2,
};


static void SkipToPrevSong()
{
	byte *b = _cur_playlist;
	byte *p = b;
	byte t;

	// empty playlist
	if (b[0] == 0)
		return;

	// find the end
	do p++; while (p[0] != 0);

	// and copy the bytes
	t = *--p;
	while (p != b) {
		p--;
		p[1] = p[0];
	}
	*b = t;

	_song_is_active = false;
}

static void SkipToNextSong()
{
	byte *b = _cur_playlist, t;

	if ((t=b[0]) != 0) {
		while (b[1]) {
			b[0] = b[1];
			b++;
		}
		b[0] = t;
	}

	_song_is_active = false;
}

static void MusicVolumeChanged(byte new_vol)
{
	_music_driver->set_volume(new_vol);
}

static void DoPlaySong()
{
	char filename[256];
	sprintf(filename, "%sgm_tt%.2d.gm",  _path.gm_dir, _music_wnd_cursong - 1);
	_music_driver->play_song(filename);
}

static void DoStopMusic()
{
	_music_driver->stop_song();
}

static void SelectSongToPlay()
{
	int i;

	memset(_cur_playlist, 0, 33);
	strcpy(_cur_playlist, _playlists[msf.playlist]);

	if (msf.shuffle) {
		i = 500;
		do {
			uint32 r = InteractiveRandom();
			byte *a = &_cur_playlist[r & 0x1F];
			byte *b = &_cur_playlist[(r >> 8)&0x1F];

			if (*a != 0 && *b != 0) {
				byte t = *a;
				*a = *b;
				*b = t;
			}
		} while (--i);
	}
}

static void StopMusic()
{
	_music_wnd_cursong = 0;
	DoStopMusic();
	_song_is_active = false;
	InvalidateWindowWidget(WC_MUSIC_WINDOW, 0, 9);
}

static void PlayPlaylistSong()
{
	if (_cur_playlist[0] == 0) {
		SelectSongToPlay();
		if (_cur_playlist[0] == 0)
			return;
	}
	_music_wnd_cursong = _cur_playlist[0];
	DoPlaySong();
	_song_is_active = true;

	InvalidateWindowWidget(WC_MUSIC_WINDOW, 0, 9);
}

void ResetMusic()
{
	_music_wnd_cursong = 1;
	DoPlaySong();
}

void MusicLoop()
{
	if (!msf.btn_down && _song_is_active) {
		StopMusic();
	} else if (msf.btn_down && !_song_is_active) {
		PlayPlaylistSong();
	}

	if (_song_is_active == false)
		return;

	if (!_music_driver->is_song_playing()) {
		StopMusic();
		SkipToNextSong();
		PlayPlaylistSong();
	}
}

static void MusicTrackSelectionWndProc(Window *w, WindowEvent *e)
{
	switch(e->event) {
	case WE_PAINT: {
		int y,i;
		byte *p;

		w->disabled_state = (msf.playlist  <= 3) ? (1 << 11) : 0;
		w->click_state |= 0x18;
		DrawWindowWidgets(w);

		GfxFillRect(3, 23, 3+177,23+191,0);
		GfxFillRect(251, 23, 251+177,23+191,0);

		DrawStringCentered(92, 15, STR_01EE_TRACK_INDEX, 0);

		SET_DPARAM16(0, STR_01D5_ALL + msf.playlist);
		DrawStringCentered(340, 15, STR_01EF_PROGRAM, 0);

		for(i=1; (uint)i <= NUM_SONGS_AVAILABLE; i++) {
			SET_DPARAM16(0, i);
			SET_DPARAM16(2, i);
			SET_DPARAM16(1, SPECSTR_SONGNAME);
			DrawString(4, 23+(i-1)*6, (i < 10) ? STR_01EC_0 : STR_01ED, 0);
		}

		for(i=0; i!=6; i++) {
			DrawStringCentered(216, 45 + i*8, STR_01D5_ALL + i, (i==msf.playlist) ? 0xC : 0x10);
		}

		DrawStringCentered(216, 45+8*6+16, STR_01F0_CLEAR, 0);
		DrawStringCentered(216, 45+8*6+16*2, STR_01F1_SAVE, 0);

		y = 23;
		for(p = _playlists[msf.playlist],i=0; (i=*p) != 0; p++) {
			SET_DPARAM16(0, i);
			SET_DPARAM16(2, i);
			SET_DPARAM16(1, SPECSTR_SONGNAME);
			DrawString(252, y, (i < 10) ? STR_01EC_0 : STR_01ED, 0);
			y += 6;
		}
		break;
	}

	case WE_CLICK:
		switch(e->click.widget) {
		case 3: { /* add to playlist */
			int y = (e->click.pt.y - 23) / 6;
			int i;
			byte *p;
			if (msf.playlist < 4) return;
			if ((uint)y >= NUM_SONGS_AVAILABLE) return;

			p = _playlists[msf.playlist];
			for(i=0; i!=32; i++) {
				if (p[i] == 0) {
					p[i] = (byte)(y + 1);
					p[i+1] = 0;
					SetWindowDirty(w);
					SelectSongToPlay();
					break;
				}
			}

		} break;
		case 11: /* clear */
			_playlists[msf.playlist][0] = 0;
			SetWindowDirty(w);
			StopMusic();
			SelectSongToPlay();
			break;
		case 12: /* save */
			ShowInfo("MusicTrackSelectionWndProc:save not implemented\n");
			break;
		case 5: case 6: case 7: case 8: case 9: case 10: /* set playlist */
			msf.playlist = e->click.widget - 5;
			SetWindowDirty(w);
			InvalidateWindow(WC_MUSIC_WINDOW, 0);
			StopMusic();
			SelectSongToPlay();
			break;
		}
		break;
	}
}

static const Widget _music_track_selection_widgets[] = {
{    WWT_TEXTBTN,    14,     0,    10,     0,    13, STR_00C5,STR_018B_CLOSE_WINDOW},
{    WWT_CAPTION,    14,    11,   431,     0,    13, STR_01EB_MUSIC_PROGRAM_SELECTION, STR_018C_WINDOW_TITLE_DRAG_THIS},
{     WWT_IMGBTN,    14,     0,   431,    14,   217, 0x0,			STR_NULL},
{     WWT_IMGBTN,    14,     2,   181,    22,   215, 0x0,			STR_01FA_CLICK_ON_MUSIC_TRACK_TO},
{     WWT_IMGBTN,    14,   250,   429,    22,   215, 0x0,			STR_01F2_CURRENT_PROGRAM_OF_MUSIC},
{ WWT_PUSHIMGBTN,    14,   186,   245,    44,    51, 0x0,			STR_01F3_SELECT_ALL_TRACKS_PROGRAM},
{ WWT_PUSHIMGBTN,    14,   186,   245,    52,    59, 0x0,			STR_01F4_SELECT_OLD_STYLE_MUSIC},
{ WWT_PUSHIMGBTN,    14,   186,   245,    60,    67, 0x0,			STR_01F5_SELECT_NEW_STYLE_MUSIC},
{ WWT_PUSHIMGBTN,    14,   186,   245,    68,    75, 0x0,			STR_0330_SELECT_EZY_STREET_STYLE},
{ WWT_PUSHIMGBTN,    14,   186,   245,    76,    83, 0x0,			STR_01F6_SELECT_CUSTOM_1_USER_DEFINED},
{ WWT_PUSHIMGBTN,    14,   186,   245,    84,    91, 0x0,			STR_01F7_SELECT_CUSTOM_2_USER_DEFINED},
{ WWT_PUSHIMGBTN,    14,   186,   245,   108,   115, 0x0,			STR_01F8_CLEAR_CURRENT_PROGRAM_CUSTOM1},
{ WWT_PUSHIMGBTN,    14,   186,   245,   124,   131, 0x0,			STR_01F9_SAVE_MUSIC_SETTINGS_TO},
{   WIDGETS_END},
};

static const WindowDesc _music_track_selection_desc = {
	104, 131, 432, 218,
	WC_MUSIC_TRACK_SELECTION,0,
	WDF_STD_TOOLTIPS | WDF_STD_BTN | WDF_DEF_WIDGET | WDF_UNCLICK_BUTTONS,
	_music_track_selection_widgets,
	MusicTrackSelectionWndProc
};

static void ShowMusicTrackSelection()
{
	AllocateWindowDescFront(&_music_track_selection_desc, 0);
}

static void MusicWindowWndProc(Window *w, WindowEvent *e)
{
	switch(e->event) {
	case WE_PAINT: {
		int i,num;
		StringID str;

		w->click_state |= 0x280;
		DrawWindowWidgets(w);

		GfxFillRect(187, 16, 200, 33, 0);

		num = 8;
		for (i=0; i!=num; i++) {
			int color = 0xD0;
			if (i > 4) {
				color = 0xBF;
				if (i > 6) {
					color = 0xB8;
				}
			}
			GfxFillRect(187, 33 - i*2, 200, 33 - i*2, color);
		}

		GfxFillRect(60, 46, 239, 52, 0);

		str = STR_01E3;
		if (_song_is_active != 0 && _music_wnd_cursong != 0) {
			str = STR_01E4_0;
			SET_DPARAM8(0, _music_wnd_cursong);
			if (_music_wnd_cursong >= 10)
				str = STR_01E5;
		}
		DrawString(62, 46, str, 0);

		str = STR_01E6;
		if (_song_is_active != 0 && _music_wnd_cursong != 0) {
			str = STR_01E7;
			SET_DPARAM16(0, SPECSTR_SONGNAME);
			SET_DPARAM16(1, _music_wnd_cursong);
		}
		DrawStringCentered(155, 46, str, 0);


		DrawString(60, 38, STR_01E8_TRACK_XTITLE, 0);

		for(i=0; i!=6; i++) {
			DrawStringCentered(25+i*50, 59, STR_01D5_ALL+i, msf.playlist == i ? 0xC : 0x10);
		}

		DrawStringCentered(31, 43, STR_01E9_SHUFFLE, (msf.shuffle ? 0xC : 0x10));
		DrawStringCentered(269, 43, STR_01EA_PROGRAM, 0);
		DrawStringCentered(141, 15, STR_01DB_MUSIC_VOLUME, 0);
		DrawStringCentered(141, 29, STR_01DD_MIN_MAX, 0);
		DrawStringCentered(247, 15, STR_01DC_EFFECTS_VOLUME, 0);
		DrawStringCentered(247, 29, STR_01DD_MIN_MAX, 0);

		DrawFrameRect(108, 23, 174, 26, 14, 0x20);
		DrawFrameRect(214, 23, 280, 26, 14, 0x20);

		DrawFrameRect(108 + (msf.music_vol>>1),
									22,
									111 + (msf.music_vol>>1),
									28,
									14,
									0);

		DrawFrameRect(214 + (msf.effect_vol>>1),
									22,
									217 + (msf.effect_vol>>1),
									28,
									14,
									0);
	} break;

	case WE_CLICK:
		switch(e->click.widget) {
		case 2: // skip to prev
			if (!_song_is_active)
				return;
			SkipToPrevSong();
			break;
		case 3: // skip to next
			if (!_song_is_active)
				return;
			SkipToNextSong();
			break;
		case 4: // stop playing
			msf.btn_down = false;
			break;
		case 5: // start playing
			msf.btn_down = true;
			break;
		case 6:{ // volume sliders
			byte *vol,new_vol;
			int x = e->click.pt.x - 88;

			if (x < 0)
				return;

			vol = &msf.music_vol;
			if (x >= 106) {
				vol = &msf.effect_vol;
				x -= 106;
			}

			new_vol = min(max(x-21,0)*2,127);
			if (new_vol != *vol) {
				*vol = new_vol;
				if (vol == &msf.music_vol)
					MusicVolumeChanged(new_vol);
				SetWindowDirty(w);
			}

			_left_button_clicked = false;
		} break;
		case 10: //toggle shuffle
			msf.shuffle ^= 1;
			StopMusic();
			SelectSongToPlay();
			break;
		case 11: //show track selection
			ShowMusicTrackSelection();
			break;
		case 12: case 13: case 14: case 15: case 16: case 17: // playlist
			msf.playlist = e->click.widget - 12;
			SetWindowDirty(w);
			InvalidateWindow(WC_MUSIC_TRACK_SELECTION, 0);
			StopMusic();
			SelectSongToPlay();
			break;
		}
		break;

	case WE_MOUSELOOP:
		InvalidateWindowWidget(WC_MUSIC_WINDOW, 0, 7);
		break;
	}

}

static const Widget _music_window_widgets[] = {
{    WWT_TEXTBTN,    14,     0,    10,     0,    13, STR_00C5,	STR_018B_CLOSE_WINDOW},
{    WWT_CAPTION,    14,    11,   299,     0,    13, STR_01D2_JAZZ_JUKEBOX, STR_018C_WINDOW_TITLE_DRAG_THIS},
{ WWT_PUSHIMGBTN,    14,     0,    21,    14,    35, 0x2C5,			STR_01DE_SKIP_TO_PREVIOUS_TRACK},
{ WWT_PUSHIMGBTN,    14,    22,    43,    14,    35, 0x2C6,			STR_01DF_SKIP_TO_NEXT_TRACK_IN_SELECTION},
{ WWT_PUSHIMGBTN,    14,    44,    65,    14,    35, 0x2C7,			STR_01E0_STOP_PLAYING_MUSIC},
{ WWT_PUSHIMGBTN,    14,    66,    87,    14,    35, 0x2C8,			STR_01E1_START_PLAYING_MUSIC},
{     WWT_IMGBTN,    14,    88,   299,    14,    35, 0x0,				STR_01E2_DRAG_SLIDERS_TO_SET_MUSIC},
{     WWT_IMGBTN,    14,   186,   201,    15,    34, 0x0,				STR_NULL},
{     WWT_IMGBTN,    14,     0,   299,    36,    57, 0x0,				STR_NULL},
{     WWT_IMGBTN,    14,    59,   240,    45,    53, 0x0,				STR_NULL},
{ WWT_PUSHIMGBTN,    14,     6,    55,    42,    49, 0x0,				STR_01FB_TOGGLE_PROGRAM_SHUFFLE},
{ WWT_PUSHIMGBTN,    14,   244,   293,    42,    49, 0x0,				STR_01FC_SHOW_MUSIC_TRACK_SELECTION},
{ WWT_PUSHIMGBTN,    14,     0,    49,    58,    65, 0x0,				STR_01F3_SELECT_ALL_TRACKS_PROGRAM},
{ WWT_PUSHIMGBTN,    14,    50,    99,    58,    65, 0x0,				STR_01F4_SELECT_OLD_STYLE_MUSIC},
{ WWT_PUSHIMGBTN,    14,   100,   149,    58,    65, 0x0,				STR_01F5_SELECT_NEW_STYLE_MUSIC},
{ WWT_PUSHIMGBTN,    14,   150,   199,    58,    65, 0x0,				STR_0330_SELECT_EZY_STREET_STYLE},
{ WWT_PUSHIMGBTN,    14,   200,   249,    58,    65, 0x0,				STR_01F6_SELECT_CUSTOM_1_USER_DEFINED},
{ WWT_PUSHIMGBTN,    14,   250,   299,    58,    65, 0x0,				STR_01F7_SELECT_CUSTOM_2_USER_DEFINED},
{   WIDGETS_END},
};

static const WindowDesc _music_window_desc = {
	0, 22, 300, 66,
	WC_MUSIC_WINDOW,0,
	WDF_STD_TOOLTIPS | WDF_STD_BTN | WDF_DEF_WIDGET | WDF_UNCLICK_BUTTONS,
	_music_window_widgets,
	MusicWindowWndProc
};

void ShowMusicWindow()
{
	AllocateWindowDescFront(&_music_window_desc, 0);
}

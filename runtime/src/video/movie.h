/* Intro movie playback (replaces Bink). */
#pragma once

/* Play the movie for a game path such as "data\movie\intro.bik" (its .mp4
 * conversion), over the game window, until it ends or a key or controller
 * button is pressed. Returns 0 if there is no converted movie. */
int movie_play(const char *bik_path);

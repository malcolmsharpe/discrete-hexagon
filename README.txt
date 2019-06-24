Discrete Hexagon

Concept:
	A variant of Super Hexagon using the Necrodancer move mechanic.

How to run:
        Run "./discrete-hexagon".

        Might require the SDL2 dylibs to be placed in /usr/local/lib (or another dylib directory)--
        or install the required SDL2 libraries:
            brew install sdl2
            brew install sdl2_image
            brew install sdl2_ttf

How to play:
	NOTE: There is no music provided, so use your own once you learn the controls.
	Use arrow keys to move:
		left -- rotate counterclockwise
		up -- stay still
		right -- rotate clockwise
		down -- jump a hurdle (a green bar)
	Press the arrow key you want once per beat.
	Avoid colliding with the obstacles.
	You win once there are no more obstacles left, after about 300 beats.

	Use backspace to restart when you die or finish.

Modding:
	The file data/patterns.txt specifies the patterns that are randomly selected from to produce a level.

	This file may be edited to introduce different patterns.
	Format:
		First line is the number of lanes.
		Each pattern consists of the number of rows, then the rows, with 4 characters per line.
		Legend:
			. -- empty space
			# -- wall
			o -- hurdle
		Blank lines may be used freely.
		The file is terminated with a 0.

	There are also other versions of this file included that you can try, by copying over data/patterns.txt.

Comments:
	The Super Hexagon soundtrack works well as music. :)

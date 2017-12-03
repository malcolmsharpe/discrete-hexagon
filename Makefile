discrete-hexagon: main.cpp
	g++ -O -Wall -I/usr/local/include/SDL2 -std=c++11 -lSDL2 -lSDL2_image -lSDL2_ttf main.cpp -o discrete-hexagon

discrete-hexagon.html: main.cpp
	emcc -O main.cpp -std=c++11 -s USE_SDL=2 -s USE_SDL_IMAGE=2 -s USE_SDL_TTF=2 -s SDL2_IMAGE_FORMATS='["png"]' -o discrete-hexagon.html --preload-file data

clean:
	rm -f discrete-hexagon discrete-hexagon.html

emcc -o index.html main.c -Os -std=c99 -I../../Clone/raylib/src -L../../Clone/raylib/src -lraylib -s USE_GLFW=3 -s ASYNCIFY --shell-file shell.html --preload-file "assets/"
mkdir dist
mv index.data index.html index.js index.wasm dist/
zip -r game.zip dist
rm -rf dist
echo "created game.zip"
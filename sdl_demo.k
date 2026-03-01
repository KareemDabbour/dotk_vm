import "sdl.k"

var x = 120
var y = 120
var speed = 5

var started = sdlInit(900, 560, "DotK + SDL2 fallback")
if(!started){
  print_error("SDL init failed")
}

sdlSetTargetFps(60)

while(!sdlShouldClose()){
  if(sdlKeyDown(SDL_SCANCODE_LEFT)) x -= speed
  if(sdlKeyDown(SDL_SCANCODE_RIGHT)) x += speed
  if(sdlKeyDown(SDL_SCANCODE_UP)) y -= speed
  if(sdlKeyDown(SDL_SCANCODE_DOWN)) y += speed

  sdlBegin(20, 22, 30, 255)
  sdlRect(24, 24, 260, 70, 40, 60, 120, 255)
  sdlRect(x, y, 160, 90, 100, 170, 255, 255)
  sdlRect(x + 65, y + 30, 30, 30, 245, 220, 120, 255)
  sdlEnd()
}

sdlClose()

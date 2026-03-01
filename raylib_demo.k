import "raylib.k"

var x = 120
var y = 120
var speed = 220

var started = rlInit(900, 560, "DotK + raylib")
if(!started){
  print_error("raylib init failed (WSL/OpenGL context). Try: LIBGL_ALWAYS_SOFTWARE=1 ./dotk.out raylib_demo.k")
}
rlSetTargetFps(60)

while(!rlShouldClose()){
  var dt = rlFrameTime()

  if(rlKeyDown(RL_KEY_LEFT)) x = x - (speed * dt)
  if(rlKeyDown(RL_KEY_RIGHT)) x = x + (speed * dt)
  if(rlKeyDown(RL_KEY_UP)) y = y - (speed * dt)
  if(rlKeyDown(RL_KEY_DOWN)) y = y + (speed * dt)

  rlBegin()
  rlClear(25, 28, 38, 255)
  rlText("DotK + raylib GUI", 24, 20, 30, 235, 235, 235, 255)
  rlText("Move with arrow keys", 24, 62, 22, 180, 210, 255, 255)
  rlRect(x, y, 160, 90, 111, 166, 255, 255)
  rlCircle(x + 80, y + 45, 16, 250, 230, 120, 255)
  rlEnd()
}

rlClose()

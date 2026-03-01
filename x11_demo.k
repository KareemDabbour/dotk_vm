import "x11.k"

var x = 120
var y = 120
var speed = 5

var started = x11Init(900, 560, "DotK + X11 fallback")
if(!started){
  print_error("X11 init failed")
}

x11SetTargetFps(60)

while(!x11ShouldClose()){
  if(x11KeyDown(X11_KEY_LEFT)) x -= speed
  if(x11KeyDown(X11_KEY_RIGHT)) x += speed
  if(x11KeyDown(X11_KEY_UP)) y -= speed
  if(x11KeyDown(X11_KEY_DOWN)) y += speed
  if(x11KeyDown(X11_KEY_ESCAPE)) break

  x11Begin(20, 22, 30)
  x11Rect(24, 24, 260, 70, 40, 60, 120)
  x11Rect(x, y, 160, 90, 100, 170, 255)
  x11Rect(x + 65, y + 30, 30, 30, 245, 220, 120)
  x11End()
}

x11Close()

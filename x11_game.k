import "x11.k"

var WIDTH = 900
var HEIGHT = 560

fn clamp(v, lo, hi){
  if(v < lo) return lo
  if(v > hi) return hi
  return v
}

fn overlaps(ax, ay, aw, ah, bx, by, bw, bh){
  return ax < (bx + bw) && (ax + aw) > bx && ay < (by + bh) && (ay + ah) > by
}

var started = x11Init(WIDTH, HEIGHT, "DotK X11 - Dodger")
if(!started){
  print_error("X11 game init failed")
}

x11SetTargetFps(60)

var playerW = 70
var playerH = 16
var playerX = (WIDTH // 2) - (playerW // 2)
var playerY = HEIGHT - 50
var playerSpeed = 8

var e1x = randInt(20, WIDTH - 40)
var e1y = -40
var e1s = 4

var e2x = randInt(20, WIDTH - 40)
var e2y = -220
var e2s = 5

var e3x = randInt(20, WIDTH - 40)
var e3y = -380
var e3s = 6

var score = 0
var best = 0
var gameOver = false

var restartX = 325
var restartY = 300
var restartW = 250
var restartH = 34

fn resetGame(){
  playerX = (WIDTH // 2) - (playerW // 2)
  e1x = randInt(20, WIDTH - 40)
  e2x = randInt(20, WIDTH - 40)
  e3x = randInt(20, WIDTH - 40)
  e1y = -40
  e2y = -220
  e3y = -380
  score = 0
  gameOver = false
}

while(!x11ShouldClose()){
  var mx = x11MouseX()
  var my = x11MouseY()

  if(!gameOver){
    if(x11KeyDown(X11_KEY_LEFT)) playerX -= playerSpeed
    if(x11KeyDown(X11_KEY_RIGHT)) playerX += playerSpeed
    if(x11MouseDown(X11_MOUSE_LEFT)) playerX = mx - (playerW // 2)
    playerX = clamp(playerX, 0, WIDTH - playerW)

    e1y += e1s
    e2y += e2s
    e3y += e3s

    if(e1y > HEIGHT){
      e1y = -randInt(40, 180)
      e1x = randInt(20, WIDTH - 40)
      score += 1
    }
    if(e2y > HEIGHT){
      e2y = -randInt(120, 260)
      e2x = randInt(20, WIDTH - 40)
      score += 1
    }
    if(e3y > HEIGHT){
      e3y = -randInt(180, 320)
      e3x = randInt(20, WIDTH - 40)
      score += 1
    }

    if(score > best) best = score

    if(overlaps(playerX, playerY, playerW, playerH, e1x, e1y, 28, 28) ||
       overlaps(playerX, playerY, playerW, playerH, e2x, e2y, 30, 30) ||
       overlaps(playerX, playerY, playerW, playerH, e3x, e3y, 32, 32)){
      gameOver = true
    }
  } else {
    if(x11KeyDown(X11_KEY_SPACE)){
      resetGame()
    }

    if(x11MouseClicked(X11_MOUSE_LEFT) && overlaps(mx, my, 1, 1, restartX, restartY, restartW, restartH)){
      resetGame()
    }

    if(x11KeyDown(X11_KEY_ESCAPE)){
      break
    }
  }

  x11Begin(16, 18, 28)

  x11Rect(0, HEIGHT - 30, WIDTH, 30, 36, 40, 55)
  x11Rect(playerX, playerY, playerW, playerH, 120, 210, 255)
  x11Circle(playerX + (playerW // 2), playerY + (playerH // 2), 5, 245, 232, 150)

  x11Rect(e1x, e1y, 28, 28, 255, 120, 120)
  x11Rect(e2x, e2y, 30, 30, 255, 150, 110)
  x11Rect(e3x, e3y, 32, 32, 255, 180, 100)

  x11Text("DOTK DODGER", 20, 28, 200, 220, 255)
  x11Text("score: " + score, 20, 48, 180, 255, 180)
  x11Text("best: " + best, 20, 68, 180, 230, 255)

  if(gameOver){
    x11Rect(210, 220, 470, 110, 30, 30, 45)
    x11Text("GAME OVER", 380, 255, 255, 180, 180)
    x11Text("SPACE = restart, ESC = quit", 300, 285, 220, 220, 220)
    x11Rect(restartX, restartY, restartW, restartH, 55, 75, 130)
    x11Text("Click to Restart", restartX + 72, restartY + 22, 240, 240, 255)
  }

  x11End()
}

x11Close()

import "x11.k"
import "file"

var WIDTH = 980
var HEIGHT = 620

var camX = 0.0
var camY = 0.0
var camZ = 0.0
var camYaw = 0.0
var camPitch = 0.0
var fov = 520.0

var OBJ_PARSE_VERTS = {}
var OBJ_PARSE_EDGES = {}
var OBJ_PARSE_SEEN = .{}

var CUBE_VERTS = {
  {-1, -1, -1},
  { 1, -1, -1},
  { 1,  1, -1},
  {-1,  1, -1},
  {-1, -1,  1},
  { 1, -1,  1},
  { 1,  1,  1},
  {-1,  1,  1}
}

var CUBE_EDGES = {
  {0, 1}, {1, 2}, {2, 3}, {3, 0},
  {4, 5}, {5, 6}, {6, 7}, {7, 4},
  {0, 4}, {1, 5}, {2, 6}, {3, 7}
}

fn clamp(v, lo, hi){
  if(v < lo) return lo
  if(v > hi) return hi
  return v
}

fn absf(v){
  if(v < 0) return -v
  return v
}

fn asNumberOr(v, fallback){
  var tv = type(v)
  if(tv == "int" || tv == "float") return v
  return fallback
}

fn computeYawFromMouse(mx){
  var safeMx = asNumberOr(mx, WIDTH // 2)
  var centered = safeMx - (WIDTH // 2)
  return centered / 500.0
}

fn computePitchFromMouse(my){
  var safeMy = asNumberOr(my, HEIGHT // 2)
  var centered = safeMy - (HEIGHT // 2)
  return -(centered / 560.0)
}

fn drawLine(x0, y0, x1, y1, r, g, b){
  var dx = x1 - x0
  var dy = y1 - y0
  var steps = absf(dx)
  if(absf(dy) > steps) steps = absf(dy)

  if(steps < 1){
    x11Rect(x0, y0, 2, 2, r, g, b)
    return null
  }

  var sx = dx / steps
  var sy = dy / steps
  var x = x0
  var y = y0
  var i = 0
  while(i <= steps){
    x11Rect(x, y, 2, 2, r, g, b)
    x += sx
    y += sy
    i += 1
  }
}

fn projectPoint(wx, wy, wz){
  var x = wx - camX
  var y = wy - camY
  var z = wz - camZ

  var cy = cos(camYaw)
  var sy = sin(camYaw)
  var xz = (x * cy) - (z * sy)
  var zz = (x * sy) + (z * cy)

  var cp = cos(camPitch)
  var sp = sin(camPitch)
  var yz = (y * cp) - (zz * sp)
  var zz2 = (y * sp) + (zz * cp)

  if(zz2 <= 0.2) return null

  var sx = (WIDTH // 2) + ((xz * fov) / zz2)
  var sy2 = (HEIGHT // 2) - ((yz * fov) / zz2)

  return .{"x"::sx, "y"::sy2, "z"::zz2}
}

fn drawLine3D(ax, ay, az, bx, by, bz, r, g, b){
  var a = projectPoint(ax, ay, az)
  var d = projectPoint(bx, by, bz)
  if(a && d){
    drawLine(a["x"], a["y"], d["x"], d["y"], r, g, b)
  }
}

fn splitWs(s){
  var cleaned = s.replace("\t", " ")
  var parts = cleaned.split(" ")
  var out = {}
  var i = 0
  while(i < len(parts)){
    var p = parts[i].trim()
    if(len(p) > 0) out.append(p)
    i += 1
  }
  return out
}

fn parseObjIndexToken(tok, vertCount){
  var first = tok.split("/")[0]
  var idx = int(first)
  if(idx < 0) return vertCount + idx
  return idx - 1
}

fn addEdgeUnique(edges, seen, a, b){
  if(a == b) return null

  var lo = a
  var hi = b
  if(lo > hi){
    var t = lo
    lo = hi
    hi = t
  }

  var key = lo + "|" + hi
  if(!seen.get(key, false)){
    seen[key] = true
    edges.append({lo, hi})
  }
}

fn drawWireModel(model, cx, cy, cz, scale, rotY, r, g, b){
  if(!model) return null

  var verts = model["verts"]
  var edges = model["edges"]
  var proj = [len(verts)]

  var cr = cos(rotY)
  var sr = sin(rotY)

  var i = 0
  while(i < len(verts)){
    var v = verts[i]
    var lx = v[0] * scale
    var ly = v[1] * scale
    var lz = v[2] * scale

    var rx = (lx * cr) - (lz * sr)
    var rz = (lx * sr) + (lz * cr)

    proj[i] = projectPoint(cx + rx, cy + ly, cz + rz)
    i += 1
  }

  i = 0
  while(i < len(edges)){
    var e = edges[i]
    var a = proj[e[0]]
    var d = proj[e[1]]
    if(a && d){
      drawLine(a["x"], a["y"], d["x"], d["y"], r, g, b)
    }
    i += 1
  }
}

fn drawWireCube(cx, cy, cz, size, r, g, b){
  var pts = [8]

  var i = 0
  while(i < 8){
    var v = CUBE_VERTS[i]
    pts[i] = projectPoint(cx + (v[0] * size), cy + (v[1] * size), cz + (v[2] * size))
    i += 1
  }

  i = 0
  while(i < len(CUBE_EDGES)){
    var e = CUBE_EDGES[i]
    var a = pts[e[0]]
    var d = pts[e[1]]
    if(a && d){
      drawLine(a["x"], a["y"], d["x"], d["y"], r, g, b)
    }
    i += 1
  }
}

fn makeObstacle(){
  return .{
    "x"::(randInt(-90, 90) / 10.0),
    "y"::(randInt(-50, 50) / 10.0),
    "z"::(randInt(25, 75) * 1.0),
    "speed"::(randInt(14, 34) / 220.0),
    "size"::(randInt(8, 16) / 10.0),
    "rot"::(randInt(0, 628) / 100.0),
    "rotSpeed"::(randInt(6, 20) / 400.0)
  }
}

fn resetObstacle(o){
  o["x"] = randInt(-90, 90) / 10.0
  o["y"] = randInt(-50, 50) / 10.0
  o["z"] = randInt(35, 85) * 1.0
  o["speed"] = randInt(14, 34) / 220.0
  o["size"] = randInt(8, 16) / 10.0
  o["rot"] = randInt(0, 628) / 100.0
  o["rotSpeed"] = randInt(6, 20) / 400.0
}

fn drawCrosshair(cx, cy, r, g, b){
  x11Rect(cx - 7, cy, 14, 1, r, g, b)
  x11Rect(cx, cy - 7, 1, 14, r, g, b)
}

fn tryShoot(obstacles, aimX, aimY){
  var bestIdx = -1
  var bestDepth = 999999

  var i = 0
  while(i < len(obstacles)){
    var p = projectPoint(obstacles[i]["x"], obstacles[i]["y"], obstacles[i]["z"])
    if(p && p["z"] > 0.3){
      var dx = p["x"] - aimX
      var dy = p["y"] - aimY
      var radPx = ((obstacles[i]["size"] * fov) / p["z"]) + 8
      if((dx * dx + dy * dy) <= (radPx * radPx) && p["z"] < bestDepth){
        bestDepth = p["z"]
        bestIdx = i
      }
    }
    i += 1
  }

  if(bestIdx != -1){
    resetObstacle(obstacles[bestIdx])
    return true
  }

  return false
}

var started = x11Init(WIDTH, HEIGHT, "DotK X11 - 3D Shooter Demo")
if(!started){
  print_error("x11Init failed (did you build x11-module?)")
}

x11SetTargetFps(60)

var targetModel = null
try {
  var objPath = "models/demo_ship.obj"
  var objFile = File(objPath, "r")
  var objLines = objFile.readLines(true)
  objFile.close()

  OBJ_PARSE_VERTS = {}
  OBJ_PARSE_EDGES = {}
  OBJ_PARSE_SEEN = .{}

  for (var li = 0; li < len(objLines); li += 1){
    var raw = objLines[li].trim()
    if(len(raw) > 0 && raw[0] != "#"){
      var tokens = splitWs(raw)
      if(len(tokens) > 0){
        if(tokens[0] == "v" && len(tokens) >= 4){
          OBJ_PARSE_VERTS.append({float(tokens[1]), float(tokens[2]), float(tokens[3])})
        } else if(tokens[0] == "f" && len(tokens) >= 4){
          var faceIdx = {}
          for (var j = 1; j < len(tokens); j += 1){
            var idx = parseObjIndexToken(tokens[j], len(OBJ_PARSE_VERTS))
            if(idx >= 0 && idx < len(OBJ_PARSE_VERTS)) faceIdx.append(idx)
          }

          if(len(faceIdx) >= 2){
            for (var k = 0; k < len(faceIdx); k += 1){
              var next = (k + 1) % len(faceIdx)
              addEdgeUnique(OBJ_PARSE_EDGES, OBJ_PARSE_SEEN, faceIdx[k], faceIdx[next])
            }
          }
        }
      }
    }
  }

  if(len(OBJ_PARSE_VERTS) > 0 && len(OBJ_PARSE_EDGES) > 0){
    targetModel = .{"verts"::OBJ_PARSE_VERTS, "edges"::OBJ_PARSE_EDGES}
    print("loaded obj: " + objPath + " (verts=" + len(OBJ_PARSE_VERTS) + ", edges=" + len(OBJ_PARSE_EDGES) + ")")
  } else {
    print_error("OBJ parsed but had no usable geometry: " + objPath)
  }
} catch(e){
  print_error("OBJ load failed: " + e.message)
}

if(!targetModel){
  print_error("using cube fallback renderer for targets")
}

var obstacles = {makeObstacle(), makeObstacle(), makeObstacle(), makeObstacle(), makeObstacle()}

var playerRadius = 0.85
var moveSpeed = 0.14
var score = 0
var best = 0
var shots = 0
var hits = 0
var gameOver = false
var muzzleFlash = 0

while(!x11ShouldClose()){
  var mxRaw = x11MouseX()
  var myRaw = x11MouseY()

  var mx = asNumberOr(mxRaw, WIDTH // 2)
  var my = asNumberOr(myRaw, HEIGHT // 2)

  mx = clamp(mx, 0, WIDTH)
  my = clamp(my, 0, HEIGHT)

  if(!gameOver){
    if(x11KeyDown(X11_KEY_LEFT)) camX -= moveSpeed
    if(x11KeyDown(X11_KEY_RIGHT)) camX += moveSpeed
    if(x11KeyDown(X11_KEY_UP)) camY += moveSpeed
    if(x11KeyDown(X11_KEY_DOWN)) camY -= moveSpeed

    camX = clamp(camX, -8.0, 8.0)
    camY = clamp(camY, -5.0, 5.0)

    camYaw = computeYawFromMouse(mx)
    camPitch = computePitchFromMouse(my)

    if(x11MouseClicked(X11_MOUSE_LEFT)){
      shots += 1
      muzzleFlash = 3
      if(tryShoot(obstacles, mx, my)){
        hits += 1
        score += 5
        if(score > best) best = score
      }
    }

    var i = 0
    while(i < len(obstacles)){
      obstacles[i]["z"] = obstacles[i]["z"] - obstacles[i]["speed"]
      obstacles[i]["rot"] = obstacles[i]["rot"] + obstacles[i]["rotSpeed"]

      if(obstacles[i]["z"] < 0.6){
        var dx = obstacles[i]["x"] - camX
        var dy = obstacles[i]["y"] - camY
        var hitR = playerRadius + (obstacles[i]["size"] * 1.2)
        if((dx * dx + dy * dy) < (hitR * hitR)){
          gameOver = true
        } else {
          score += 1
          if(score > best) best = score
          resetObstacle(obstacles[i])
        }
      }

      i += 1
    }
  } else {
    if(x11KeyDown(X11_KEY_SPACE)){
      camX = 0
      camY = 0
      camYaw = 0
      camPitch = 0
      score = 0
      shots = 0
      hits = 0
      muzzleFlash = 0

      var j = 0
      while(j < len(obstacles)){
        resetObstacle(obstacles[j])
        j += 1
      }
      gameOver = false
    }

    if(x11KeyDown(X11_KEY_ESCAPE)){
      break
    }
  }

  x11Begin(10, 14, 26)

  var gx = -12
  while(gx <= 12){
    drawLine3D(gx, -2.5, 6, gx, -2.5, 42, 42, 42, 65)
    gx += 2
  }

  var gz = 6
  while(gz <= 36){
    drawLine3D(-12, -2.5, gz, 12, -2.5, gz, 38, 38, 58)
    gz += 2
  }

  drawWireCube(camX, camY, 6.0, playerRadius, 120, 230, 255)

  var i2 = 0
  while(i2 < len(obstacles)){
    var depthTint = clamp(260 - (obstacles[i2]["z"] * 3), 100, 255)
    if(targetModel){
      drawWireModel(targetModel, obstacles[i2]["x"], obstacles[i2]["y"], obstacles[i2]["z"], obstacles[i2]["size"], obstacles[i2]["rot"], 255, depthTint, 120)
    } else {
      drawWireCube(obstacles[i2]["x"], obstacles[i2]["y"], obstacles[i2]["z"], obstacles[i2]["size"], 255, depthTint, 120)
    }
    i2 += 1
  }

  var gunX = (WIDTH // 2)
  var gunY = HEIGHT - 32
  var shotCol = 180
  if(muzzleFlash > 0){
    shotCol = 255
    muzzleFlash -= 1
  }

  drawLine(gunX, gunY, mx, my, shotCol, shotCol, 180)
  x11Rect(gunX - 16, gunY - 5, 34, 10, 90, 110, 145)
  x11Rect(gunX + 14, gunY - 3, 12, 6, 120, 130, 160)

  drawCrosshair(mx, my, 235, 245, 255)

  x11Text("DOTK SOFTWARE 3D SHOOTER", 16, 26, 195, 220, 255)
  x11Text("score: " + score, 16, 46, 170, 240, 170)
  x11Text("best: " + best, 16, 66, 170, 210, 255)
  x11Text("shots: " + shots + "  hits: " + hits, 16, 86, 255, 210, 170)
  x11Text("arrows move, mouse look + aim, left click shoot", 16, 108, 180, 180, 200)
  x11Text("space restart, esc quit", 16, 128, 180, 180, 200)

  if(gameOver){
    x11Rect(235, 225, 510, 120, 26, 26, 42)
    x11Text("GAME OVER", 425, 265, 255, 170, 170)
    x11Text("SPACE = restart, ESC = quit", 340, 300, 220, 220, 230)
  }

  x11End()
}

x11Close()

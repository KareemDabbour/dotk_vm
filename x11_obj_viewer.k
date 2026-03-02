import "x11.k"
import "file"

var WIDTH = 1100
var HEIGHT = 720

var camX = 0.0
var camY = 0.0
var camZ = -6.0
var camRotX = 0.0
var camRotY = 0.0
var fov = 620.0
var DEPTH_CELL = 2
var DEPTH_EPS = 0.05
var ZW = WIDTH // DEPTH_CELL
var ZH = HEIGHT // DEPTH_CELL
var ZSIZE = ZW * ZH
var depthZ = [ZSIZE]{0.0}
var depthStamp = [ZSIZE]{0}
var depthFrame = 1
var useDepthCull = false

fn rebuildDepthBuffers(){
  if(DEPTH_CELL < 1) DEPTH_CELL = 1
  if(DEPTH_CELL > 6) DEPTH_CELL = 6

  ZW = WIDTH // DEPTH_CELL
  ZH = HEIGHT // DEPTH_CELL
  ZSIZE = ZW * ZH
  depthZ = [ZSIZE]{0.0}
  depthStamp = [ZSIZE]{0}
  depthFrame = 1
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

fn drawDepthPoint(x, y, z, r, g, b){
  var px = int(x // DEPTH_CELL)
  var py = int(y // DEPTH_CELL)

  if(px < 0 || py < 0 || px >= ZW || py >= ZH) return null

  var idx = (py * ZW) + px
  if(depthStamp[idx] != depthFrame){
    depthStamp[idx] = depthFrame
    depthZ[idx] = z
    x11Rect(px * DEPTH_CELL, py * DEPTH_CELL, DEPTH_CELL, DEPTH_CELL, r, g, b)
    return null
  }

  if(z < (depthZ[idx] - DEPTH_EPS)){
    depthZ[idx] = z
    x11Rect(px * DEPTH_CELL, py * DEPTH_CELL, DEPTH_CELL, DEPTH_CELL, r, g, b)
  }
}

fn drawLineDepth(x0, y0, z0, x1, y1, z1, r, g, b){
  var dx = x1 - x0
  var dy = y1 - y0
  var steps = absf(dx)
  if(absf(dy) > steps) steps = absf(dy)

  if(steps < 1){
    drawDepthPoint(x0, y0, z0, r, g, b)
    return null
  }

  var sx = dx / steps
  var sy = dy / steps
  var sz = (z1 - z0) / steps
  var x = x0
  var y = y0
  var z = z0
  var i = 0
  while(i <= steps){
    drawDepthPoint(x, y, z, r, g, b)
    x += sx
    y += sy
    z += sz
    i += 1
  }
}

fn drawLineFast(x0, y0, x1, y1, r, g, b){
  x11Line(int(x0), int(y0), int(x1), int(y1), r, g, b)
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

fn projectPoint(wx, wy, wz){
  var x = wx - camX
  var y = wy - camY
  var z = wz - camZ

  var cy = cos(camRotY)
  var sy = sin(camRotY)
  var cx = cos(camRotX)
  var sx = sin(camRotX)

  var x1 = (x * cy) + (z * sy)
  var z1 = (-x * sy) + (z * cy)
  var y2 = (y * cx) + (z1 * sx)
  var z2 = (-y * sx) + (z1 * cx)

  if(z2 <= 0.2) return null

  var sx2 = (WIDTH // 2) + ((x1 * fov) / z2)
  var sy2 = (HEIGHT // 2) - ((y2 * fov) / z2)
  return .{"x"::sx2, "y"::sy2, "z"::z2}
}

fn screenOutCode(p){
  var code = 0
  if(p["x"] < 0) code = code | 1
  if(p["x"] >= WIDTH) code = code | 2
  if(p["y"] < 0) code = code | 4
  if(p["y"] >= HEIGHT) code = code | 8
  return code
}

fn drawWireModel(model, posX, posY, posZ, scale, rotX, rotY, r, g, b){
  if(!model) return null

  var verts = model["verts"]
  var edges = model["edges"]
  var proj = [len(verts)]

  var cy = cos(rotY)
  var sy = sin(rotY)
  var cx = cos(rotX)
  var sx = sin(rotX)

  for (var i = 0; i < len(verts); i += 1){
    var v = verts[i]

    var lx = v[0] * scale
    var ly = v[1] * scale
    var lz = v[2] * scale

    var x1 = (lx * cy) - (lz * sy)
    var z1 = (lx * sy) + (lz * cy)

    var y2 = (ly * cx) - (z1 * sx)
    var z2 = (ly * sx) + (z1 * cx)

    proj[i] = projectPoint(posX + x1, posY + y2, posZ + z2)
  }

  for (var ei = 0; ei < len(edges); ei += 1){
    var e = edges[ei]
    var a = proj[e[0]]
    var d = proj[e[1]]
    if(a && d){
          drawLineFast(a["x"], a["y"], d["x"], d["y"], r, g, b)
    #   var codeA = screenOutCode(a)
    #   var codeD = screenOutCode(d)

    #   if((codeA & codeD) == 0){
        # if(useDepthCull){
        #   drawLineDepth(a["x"], a["y"], a["z"], d["x"], d["y"], d["z"], r, g, b)
        # } else {
        # }
    #   }
    }
  }
}

var objPath = "models/demo_ship.obj"
if(len(ARG_V) > 0){
  objPath = ARG_V[0]
}

var model = null
try {
  model = x11LoadObj(objPath)
  if(model){
    print("loaded obj: " + objPath + " (verts=" + len(model["verts"]) + ", edges=" + len(model["edges"]) + ")")
  }
} catch(e){
  print_error("OBJ load failed for '" + objPath + "': " + e.message)
}

if(!model){
  print_error("Could not load model: " + objPath)
}

var ok = x11Init(WIDTH, HEIGHT, "DotK OBJ Viewer")
if(!ok){
  print_error("x11Init failed")
}

x11SetTargetFps(60)

var modelScale = 1.3
var modelX = 0.0
var modelY = 0.0
var modelZ = 8.0
var camMoveSpeed = 0.12
var rotX = 0.0
var rotY = 0.0
var autoSpin = true
var fps = 0.0
var fpsFrames = 0
var fpsLastTime = x11Time()
var lastMouseX = 0.0
var lastMouseY = 0.0
var leftDragMoved = false

while(!x11ShouldClose()){
  fpsFrames += 1
  var now = x11Time()
  if(type(now) == "float" && type(fpsLastTime) == "float"){
    var dt = now - fpsLastTime
    if(dt >= 0.25){
      fps = fpsFrames / dt
      fpsFrames = 0
      fpsLastTime = now
    }
  } else {
    fpsLastTime = now
  }

  if(x11KeyDown(X11_KEY_LEFT)) rotY -= 0.04
  if(x11KeyDown(X11_KEY_RIGHT)) rotY += 0.04
  if(x11KeyDown(X11_KEY_UP)) rotX -= 0.03
  if(x11KeyDown(X11_KEY_DOWN)) rotX += 0.03

  if(x11KeyDown(X11_KEY_A)) camX -= camMoveSpeed
  if(x11KeyDown(X11_KEY_D)) camX += camMoveSpeed
  if(x11KeyDown(X11_KEY_W)) camZ += camMoveSpeed
  if(x11KeyDown(X11_KEY_S)) camZ -= camMoveSpeed
  if(x11KeyDown(X11_KEY_Q)) camY += camMoveSpeed
  if(x11KeyDown(X11_KEY_E)) camY -= camMoveSpeed

  var mx = x11MouseX()
  var my = x11MouseY()
  var dx = mx - lastMouseX
  var dy = my - lastMouseY

  if(x11MouseDown(X11_MOUSE_LEFT)){
    if(absf(dx) + absf(dy) > 0.0) leftDragMoved = true
    rotY += dx * 0.006
    rotX += dy * 0.006
    rotX = clamp(rotX, -1.4, 1.4)
    autoSpin = false
  } else {
    leftDragMoved = false
  }

  if(x11MouseDown(X11_MOUSE_RIGHT)){
    camRotY -= dx * 0.006
    camRotX += dy * 0.006
    camRotX = clamp(camRotX, -1.4, 1.4)
    autoSpin = false
  }
  lastMouseX = mx
  lastMouseY = my

  if(x11MouseClicked(X11_MOUSE_WHEEL_UP)) modelScale += 0.1
  if(x11MouseClicked(X11_MOUSE_WHEEL_DOWN)) modelScale -= 0.1
  modelScale = clamp(modelScale, 0.2, 8.0)

  if(x11MouseClicked(X11_MOUSE_LEFT) && !leftDragMoved) autoSpin = !autoSpin
  if(x11MouseClicked(X11_MOUSE_MIDDLE)) useDepthCull = !useDepthCull
  if(autoSpin) rotY += 0.01

  if(x11KeyDown(X11_KEY_SPACE)){
    rotX = 0
    rotY = 0
    modelScale = 1.3
    camX = 0.0
    camY = 0.0
    camZ = -6.0
    camRotX = 0.0
    camRotY = 0.0
  }

  if(x11KeyDown(X11_KEY_ESCAPE)) break

  x11Begin(12, 14, 22)

  if(useDepthCull){
    if(model){
      depthFrame += 1
      if(depthFrame > 2000000000){
        depthFrame = 1
        depthStamp = [ZSIZE]{0}
      }
      drawWireModel(model, modelX, modelY, modelZ, modelScale, rotX, rotY, 180, 230, 255)
    }
  } else {
    x11DrawObjWire(objPath, modelX, modelY, modelZ, modelScale, rotX, rotY, camX, camY, camZ, camRotX, camRotY, fov, 180, 230, 255)
  }

  x11Text("DotK OBJ Viewer", 16, 26, 200, 220, 255)
  x11Text("model: " + objPath, 16, 48, 170, 210, 255)
  x11Text("L drag rotate obj | RMB drag look (inverted) | WASD+Q/E move camera", 16, 72, 180, 190, 210)
  x11Text("wheel scale | L click spin | M click depth mode | space reset | esc quit", 16, 84, 180, 190, 210)
  x11Text("scale: " + modelScale, 16, 96, 180, 255, 180)
  x11Text("depth: " + useDepthCull + "  mode: " + (useDepthCull ? "script-z" : "native-direct") + "  eps: " + DEPTH_EPS, 16, 108, 255, 220, 170)
  x11Text("cam: (" + camX + ", " + camY + ", " + camZ + ")", 16, 120, 220, 220, 255)
  x11Text("look: (" + camRotX + ", " + camRotY + ")", 16, 132, 220, 220, 255)
  x11Text("fps: " + fps, 16, 144, 200, 255, 200)

  x11End()
}

x11Close()

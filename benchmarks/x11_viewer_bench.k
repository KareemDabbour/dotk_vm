import "x11.k"

var WIDTH = 1100
var HEIGHT = 720

var camX = 0.0
var camY = 0.0
var camZ = -6.0
var camRotX = 0.0
var camRotY = 0.0
var fov = 620.0

fn clamp(v, lo, hi){
  if(v < lo) return lo
  if(v > hi) return hi
  return v
}

fn absf(v){
  if(v < 0) return -v
  return v
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

fn drawLineFast(x0, y0, x1, y1, r, g, b){
  x11Line(int(x0), int(y0), int(x1), int(y1), r, g, b)
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
    }
  }
}

var objPath = "models/cow.obj"
if(len(ARG_V) > 0) objPath = ARG_V[0]

var mode = "script"
if(len(ARG_V) > 1) mode = ARG_V[1]

var frames = 300
if(len(ARG_V) > 2) frames = int(ARG_V[2])
frames = clamp(frames, 30, 5000)

var warmup = 60
if(len(ARG_V) > 3) warmup = int(ARG_V[3])
warmup = clamp(warmup, 0, 1000)

var model = x11LoadObj(objPath)
if(!model){
  print_error("BENCH_FAIL load obj: " + objPath)
}

if(!x11Init(WIDTH, HEIGHT, "DotK X11 Bench")){
  print_error("BENCH_FAIL x11Init")
}

x11SetTargetFps(10000)

var poses = {
  .{"x"::0.0, "y"::0.0, "z"::8.0,  "s"::1.1},
  .{"x"::2.8, "y"::0.6, "z"::11.0, "s"::0.9},
  .{"x"::-2.8, "y"::-0.6, "z"::11.0, "s"::0.9},
  .{"x"::0.0, "y"::2.2, "z"::13.0, "s"::0.8},
  .{"x"::0.0, "y"::-2.2, "z"::13.0, "s"::0.8}
}

var rotX = 0.0
var rotY = 0.0
var measureStart = 0.0
var measureEnd = 0.0

var totalFrames = warmup + frames
for (var fi = 0; fi < totalFrames; fi += 1){
  if(x11ShouldClose()) break

  if(fi == warmup) measureStart = x11Time()

  rotY = rotY + 0.012
  rotX = rotX + 0.004
  camRotY = camRotY + 0.001
  camRotX = clamp(sin(rotX * 0.3) * 0.18, -0.35, 0.35)

  x11Begin(12, 14, 22)

  for (var pi = 0; pi < len(poses); pi += 1){
    var p = poses[pi]
    if(mode == "native"){
      x11DrawObjWire(objPath, p["x"], p["y"], p["z"], p["s"], rotX, rotY, camX, camY, camZ, camRotX, camRotY, fov, 180, 230, 255)
    } else {
      drawWireModel(model, p["x"], p["y"], p["z"], p["s"], rotX, rotY, 180, 230, 255)
    }
  }

  x11End()
  measureEnd = x11Time()
}

x11Close()

var dt = measureEnd - measureStart
if(dt <= 0) dt = 0.000001
var fps = frames / dt
var ms = dt * 1000.0

print("BENCH mode=" + mode + " frames=" + frames + " ms=" + ms + " fps=" + fps)
